"""Seal final shared UI optimizations and the opt-in native profiler.

Historical exploratory runs remain separate. Never replaces an existing seal.
"""
import hashlib
import json
import re
import shutil
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/local/incremental-ui"
read = lambda p: json.loads(p.read_text(encoding="utf-8-sig"))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()


def tests(path, count):
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    assert f"100% tests passed, 0 tests failed out of {count}" in text, path
    return {"count": count, "seconds": float(re.search(r"Total Test time \(real\) =\s*([0-9.]+)", text)[1])}


assert not (out / "run-metadata.json").exists(), "Already sealed; use a new stage"
prior_path = root / "artifacts/local/interaction-profile/run-metadata.json"
prior = read(prior_path)
for name, expected in prior["artifacts"].items():
    assert sha(root / name) == expected, name
for name, expected in prior["sources"].items():
    assert sha(prior_path.parent / "source-snapshot" / name) == expected, name
    assert sha(out / "baseline-source" / name) == expected, name

runs = {}
for name, config in (("release", "Release"), ("debug", "Debug"), ("asan", "Release")):
    folder = out / f"toggle-window-{name}"
    s, w, p, t = [read(folder / f) for f in ("preview-session.json", "thread-work.json", "live-profiler.json", "profile-summary.json")]
    assert s["frames"] == s["actual_completed_submissions"] == 160 and s["peak_in_flight"] == 2
    assert s["card_geometry_checks"] == 288 and s["cached_full_packet_checks"] == 17
    assert s["cpu_pixel_samples"] == 1680 and s["cpu_pixel_max_difference"] <= 3 and s["debug_warning_error_count"] == 0
    assert w["accepted_inputs"] == w["processed_inputs"] + w["coalesced_inputs"] == 15
    assert not p["enabled_at_close"] and p["owner_samples"] > 0 and p["render_samples"] > 0 and p["hud_updates"] > 1
    assert not t["complete_run_coverage"] and t["sampled_inputs"] == 3 and not any(t["dropped_events"].values())
    runs[name] = {"tests": tests(out / ("accepted-asan-ctest.log" if name == "asan" else f"complete-{name}-ctest.log"), 2 if name == "asan" else 87),
                  "window": s, "thread": w, "profiler": p, "trace": t}
    if name != "asan":
        runs[name]["final_toggle_scope"] = tests(out / f"toggle-{name}-ctest.log", 2)
    build = root / ("build/windows-asan" if name == "asan" else "build/windows-msvc")
    dest = out / "verification" / name
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / "CMakeCache.txt", dest / "CMakeCache.txt")
    for binary in ("weft_d3d11_probe.exe", "weft_style_tests.exe", "weft_scene_contract_tests.exe"):
        shutil.copy2(build / "bin" / config / binary, dest / binary)
    if name == "asan":
        shutil.copy2(build / "bin" / config / "clang_rt.asan_dynamic-x86_64.dll", dest / "clang_rt.asan_dynamic-x86_64.dll")
    else:
        shutil.copy2(build / "bin" / config / "weft_gallery_trace_tests.exe", dest / "weft_gallery_trace_tests.exe")
        trace_contract = build / "artifacts" / f"gallery-trace-{config}"
        ring, toggle = [read(trace_contract / f) for f in ("ring.json", "toggle.json")]
        assert len(ring["traceEvents"]) == 65536 and ring["dropped_events"] == 4
        assert ring["traceEvents"][0]["name"] == "event4" and ring["traceEvents"][-1]["name"] == "event65539"
        assert len(toggle["traceEvents"]) == 2 and not (trace_contract / "off.json").exists()
        shutil.copytree(trace_contract, dest / "gallery-trace", dirs_exist_ok=True)
        for folder_name in ("gallery-cluster", "gallery-preview", "scene-gallery", "scene-native", "scene-surface-retirement"):
            shutil.copytree(build / "artifacts" / f"{folder_name}-{config}", dest / folder_name, dirs_exist_ok=True)

comparison = read(out / "comparison-accepted/summary.json")
assert len(comparison["cpu"]["after"]) == 25 and len(comparison["window"]) == 5
for variant, source in (("before", root / "build/incremental-ui-baseline/bin/Release/weft_d3d11_probe.exe"), ("after", root / "build/windows-msvc/bin/Release/weft_d3d11_probe.exe")):
    shutil.copy2(source, out / "comparison-accepted" / f"{variant}.exe")

manifest = read(root / "tests/fixtures/crpg-gallery/manifest.json")
original = Path(manifest["source_root"])
drift = {"checked": len(manifest["files"]), "source_root": str(original), "changed": [], "missing": []}
for name, expected in manifest["files"].items():
    path = original / name
    if not path.is_file():
        drift["missing"].append(name)
    elif sha(path) != expected:
        drift["changed"].append(name)

names = ["AGENTS.md", "README.md", "CMakeLists.txt", "CMakePresets.json", "third_party/manifest.json", "third_party/gallery-runtime/manifest.json"]
for folder in ("include/caseelephant", "src", "probes", "docs", "tests", "examples", "shaders", "tools"):
    names += [p.relative_to(root).as_posix() for p in (root / folder).rglob("*") if p.is_file() and not any(v in {"node_modules", "__pycache__"} for v in p.parts)]
sources = {name: sha(root / name) for name in sorted(set(names))}
for name in sources:
    target = out / "source-snapshot" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, target)

# Final verification/performance, manual evidence and logs only. Large baseline
# dependency trees and historical explorations remain outside artifact claims.
folders = [out / name for name in ("source-snapshot", "verification", "comparison-accepted", "toggle-window-release", "toggle-window-debug", "toggle-window-asan", "manual-received-latest")]
artifacts = {p.relative_to(root).as_posix(): sha(p) for folder in folders for p in folder.rglob("*") if p.is_file()}
for p in out.glob("*.log"):
    # Its redirected output is still being written until this process exits.
    if p.name != "seal.log":
        artifacts[p.relative_to(root).as_posix()] = sha(p)
for name in ("run-comparison-accepted.ps1", "summarize_comparison.py"):
    artifacts[(out / name).relative_to(root).as_posix()] = sha(out / name)

record = {"format": "incremental-ui-profiler-seal-v1", "recorded_utc": datetime.now(timezone.utc).isoformat(),
          "tests": runs, "comparison": comparison, "source_drift": drift, "sources": sources, "artifacts": artifacts,
          "previous_seal": {"path": prior_path.relative_to(root).as_posix(), "sha256": sha(prior_path), "verified_sources": len(prior["sources"]), "verified_artifacts": len(prior["artifacts"]), "independent_baseline_source": True},
          "environment": prior["environment"] | {"sampling": "quiet serial alternating three pairs CPU+Kline/Anime/Scatter; one Framer/Spring pair; default affinity/priority; 240 fixed60Hz,16/224 warm; instrumentation on, heavy curve HUD off; no GPU readback"},
          "native_cases": 68, "catalogue_cases": 72, "pending_cases": 4,
          "limits": ["K-Line/Anime still exceed game UI millisecond budget", "timing/trace overhead included; old trace per-event writes/new memory ring", "old eight D3 hidden rAF continues/new uniform host hidden rAF pause; timers still pump", "GPU elapsed includes queue gaps and is not pure busy time", "physical mouse-to-display, long-run game and fair Ultralight comparison unverified", "ASan acceptance is two owner contracts and 4K window only; no full87/GC repeat", "mobile and 2 WebGL/4 Lottie remain unverified"],
          "historical_failures": "Preserved separately: first overlay capacity fault, missing ASan DLL/target configuration, exploratory and phase1 samples. Only specified final runs count as acceptance."}
(out / "run-metadata.json").write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"SEALED {len(sources)} sources / {len(artifacts)} artifacts; original drift changed={len(drift['changed'])} missing={len(drift['missing'])}")
