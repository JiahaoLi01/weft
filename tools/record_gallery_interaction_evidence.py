"""Seal actual shared-cluster, resource and interaction-profile acceptance.

Requires the final full logs and all four hardware runs in both configurations.
Historical failed/interrupted runs are retained without counting as acceptance.
"""
import hashlib
import json
import re
import shutil
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/local/interaction-profile"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tests(path, count):
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    assert f"100% tests passed, 0 tests failed out of {count}" in text, path
    return {"count": count, "seconds": float(re.search(r"Total Test time \(real\) =\s*([0-9.]+)", text)[1])}


assert not (out / "run-metadata.json").exists(), "Already sealed; use a new stage."
prior_path = root / "artifacts/local/render-stages/run-metadata.json"
prior = read(prior_path)
for name, value in prior["artifacts"].items():
    assert sha(root / name) == value, name
for name, value in prior["sources"].items():
    assert sha(prior_path.parent / "source-snapshot" / name) == value, name

runs = {}
for name, config in [("release", "Release"), ("debug", "Debug")]:
    runs[name] = {"full": tests(root / f"artifacts/local/gallery-profile-{name}-tests.log", 86)}
    path = out / f"hardware-{name}"
    gpu = read(path / "cluster/report.json")
    assert gpu["case_count"] == 30 and len(gpu["captures"]) == 210 and gpu["input_count"] == 50
    assert gpu["submissions"] == gpu["completions"] == 630 and gpu["debug_warning_error_count"] == 0 and gpu["max_mean"] <= 3
    window = read(path / "window/preview-session.json")
    assert window["frames"] == window["actual_completed_submissions"] == 160
    assert window["card_geometry_checks"] == 288 and window["cached_full_packet_checks"] == 17
    assert window["cpu_pixel_samples"] == 1680 and window["cpu_pixel_max_difference"] <= 3
    thread = read(path / "window/thread-work.json")
    assert thread["accepted_inputs"] == thread["processed_inputs"] + thread["coalesced_inputs"] == 16
    scroll = read(path / "scroll/preview-session.json")
    assert scroll["frames"] == scroll["actual_completed_submissions"] == 120 and scroll["scroll_regression"]
    assert window["debug_warning_error_count"] == scroll["debug_warning_error_count"] == 0
    surface = read(path / "surface/report.json")
    assert "result=8" in surface["old_policy_rejection"] and surface["old_policy_imported_bytes"] == 468048384
    assert surface["fixed_submissions"] == surface["fixed_completions"] == 206
    assert surface["group_scratch_reentry_submissions"] == 140 and surface["group_scratch_reentry_fallbacks"] == 0
    assert surface["group_scratch_bytes_after_retirement"] == surface["debug_warning_error_count"] == 0
    destination = out / "verification" / name
    for folder in ["gallery-cluster", "gallery-preview", "scene-gallery", "scene-native", "scene-surface-retirement"]:
        shutil.copytree(root / f"build/windows-msvc/artifacts/{folder}-{config}", destination / folder, dirs_exist_ok=True)
    audit = read(destination / "gallery-cluster/native/independent-audit.json")
    assert audit["state_pairs"] == audit["dom_pairs"] == 210 and audit["html_geometry_fields"] == 4408
    for binary in (root / f"build/windows-msvc/bin/{config}").glob("*.exe"):
        target = destination / "bin" / binary.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, target)
    shutil.copy2(root / "build/windows-msvc/CMakeCache.txt", destination / "CMakeCache.txt")
    runs[name] |= {"gpu": gpu, "window": window, "scroll": scroll, "surface": surface, "browser_audit": audit}

runs["gc_contracts"] = tests(root / "artifacts/local/gallery-profile-gc-contracts.log", 5)
gc = out / "verification/gc"
gc.mkdir(parents=True, exist_ok=True)
for name in ["weft_script_dom_tests", "weft_owner_executor_tests", "weft_style_tests", "weft_scene_contract_tests", "weft_scene_cluster_tests", "weft_scene_cluster_contract_tests"]:
    target = gc / "bin" / (name + ".exe")
    target.parent.mkdir(exist_ok=True)
    shutil.copy2(root / "build/windows-gc-stress/bin/Debug" / target.name, target)
shutil.copy2(root / "build/windows-gc-stress/CMakeCache.txt", gc / "CMakeCache.txt")

cpu = {key: read(out / key / "report.json") for key in ["before", "accepted-cpu"]}
assert all(len(value["cases"]) == 25 and value["frames_per_case"] == 240 for value in cpu.values())
assert all(case["warm_ticks_ms"]["samples"] == 224 and not case["final_state"]["errors"] for value in cpu.values() for case in value["cases"])
benchmarks = {}
for name in ["kline", "scatter", "framer", "spring", "anime"]:
    path = out / "final-trace" / name
    session, benchmark, work, trace = [read(path / filename) for filename in ["preview-session.json", "benchmark.json", "thread-work.json", "profile-summary.json"]]
    assert session["frames"] == session["actual_completed_submissions"] == benchmark["frames"] == 240
    assert session["debug_warning_error_count"] == 0 and not benchmark["capture_readback"]
    assert benchmark["warm_frames"] == 224 and work["owner_updates"] == 240
    assert work["accepted_inputs"] == work["processed_inputs"] + work["coalesced_inputs"]
    assert not any(trace["dropped_events"].values())
    benchmarks[name] = {"session": session, "benchmark": benchmark, "work": work, "trace": trace}

manifest = read(root / "tests/fixtures/crpg-gallery/manifest.json")
original = Path(manifest["source_root"])
drift = {"checked": len(manifest["files"]), "source_root": str(original), "changed": [], "missing": []}
for name, value in manifest["files"].items():
    path = original / name
    if not path.is_file():
        drift["missing"].append(name)
    elif sha(path) != value:
        drift["changed"].append(name)

names = ["AGENTS.md", "README.md", "CMakeLists.txt", "CMakePresets.json", "third_party/manifest.json", "third_party/gallery-runtime/manifest.json"]
for folder in ["include/caseelephant", "src", "probes", "docs", "tests", "examples/crpg-gallery", "shaders", "tools"]:
    names += [path.relative_to(root).as_posix() for path in (root / folder).rglob("*") if path.is_file() and not any(part in {"node_modules", "__pycache__"} for part in path.parts)]
source_hashes = {name: sha(root / name) for name in sorted(set(names))}
for name in source_hashes:
    target = out / "source-snapshot" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, target)
logs = out / "logs"
logs.mkdir(exist_ok=True)
for pattern in ["gallery-profile*.log", "gallery-cluster*.log"]:
    for path in (root / "artifacts/local").glob(pattern):
        shutil.copy2(path, logs / path.name)
artifact_hashes = {path.relative_to(root).as_posix(): sha(path) for path in out.rglob("*") if path.is_file() and path.name != "run-metadata.json"}
record = {"format": "gallery-interaction-seal-v1", "recorded_utc": datetime.now(timezone.utc).isoformat(),
    "tests": runs, "cpu_profiles": cpu, "benchmarks": benchmarks, "source_drift": drift,
    "sources": source_hashes, "artifacts": artifact_hashes,
    "previous_seal": {"path": str(prior_path.relative_to(root)), "sha256": sha(prior_path), "verified_records": len(prior["artifacts"]) + len(prior["sources"])},
    "environment": {key: prior["environment"][key] for key in ["os", "cpu", "gpu", "gpu_driver", "compiler", "performance_configuration"]} |
        {"surface": [3840, 2160], "scene_scale": 2, "owner_scene_vms": 64, "canvas_vms": 2, "sampling": "single quiet serial run; default affinity/priority; fixed 240x60Hz, 16 warmup/224 samples; no GPU readback; opt-in trace overhead included; see gallery-interaction-profile.md for input and memory limits"},
    "native_cases": 66, "catalogue_cases": 72, "pending_cases": 6,
    "limits": ["baseline binary retained but no independent complete baseline source snapshot", "fixed HWND input benchmark waits each publication; cannot prove asynchronous physical input latency", "K-Line/Anime full viewport still over 2ms game UI budget", "trace cap 65536 events per thread", "original Anime Throw stationary in Chrome/native; pinned-library inertia counterexample separately verified", "2 WebGL/4 Lottie pending", "optional 30-case forced-GC replay stopped after 115/210 captures; partial/truncated output is not acceptance; five focused forced-GC contracts are the accepted scope", "game integration, public owner C ABI, long run, total VRAM, fair Ultralight comparison and every mobile stage unverified"],
    "historical_failed_runs": "Retained for diagnosis; acceptance uses only the final full and hardware logs above."}
(out / "run-metadata.json").write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"SEALED {len(source_hashes)} sources / {len(artifact_hashes)} artifacts; original drift {len(drift['changed'])}/{len(drift['missing'])}")
