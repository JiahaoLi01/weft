"""Prove that a refactor leaves generated code unchanged (docs/coding-style.md).

    python tools/disassembly_equivalence.py dump <build_dir> <out_dir>
    python tools/disassembly_equivalence.py compare <before_dir> <after_dir> [--show N]
    python tools/disassembly_equivalence.py functions <before.txt> <after.txt>

`dump` runs `dumpbin /disasm` on every project Release object (third-party objects skipped) of a
Visual Studio build tree. `compare` reports per object, after normalizing MSVC block-scope ids in
mangled names and comparing function by function (emission order ignored):
  identical / equal-normalized   instructions equal
  equal-mod-strings              also differs in string-literal symbols (#expression test macros)
  equal-mod-lines                also differs in CHECK(...) __LINE__ immediates
  DIFFERENT                      anything else; inspect with `functions`
Build without debug information (the default Release preset) so line numbers do not leak in.
"""
import concurrent.futures
import difflib
import glob
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

SKIP = ("weft_unibreak.dir", "weft_unicode_upstream_tests.dir", "third_party")
SCOPE_MANGLED = re.compile(r"\?(?:[0-9]|[A-P]+@)\?\?")
SCOPE_DEMANGLED = re.compile(r"`\d+'")
STRING_SYMBOL = re.compile(r"\?\?_C@_[0-9A-P]+@[A-P]+@[^\]\s,]*")
LINE_IMMEDIATE = re.compile(r"(?:41 B8|BA) [0-9A-F]{2} [0-9A-F]{2} 00 00(\s+mov\s+(?:r8d|edx),)[0-9A-F]+h")
ADDRESS = re.compile(r"^\s+[0-9A-F]{16}: ")
TARGET = re.compile(r"\b[0-9A-F]{16}\b")


def find_dumpbin():
    found = shutil.which("dumpbin")
    if found:
        return found
    pattern = r"C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe"
    candidates = sorted(glob.glob(pattern))
    if not candidates:
        raise SystemExit("dumpbin.exe not found; run from a Visual Studio developer prompt")
    return candidates[-1]


def dump(build, out_dir):
    dumpbin = find_dumpbin()
    out_dir.mkdir(parents=True, exist_ok=True)
    objects = [p for p in build.rglob("*.obj")
               if f"{os.sep}Release{os.sep}" in str(p) and not any(s in str(p) for s in SKIP)]

    def one(obj):
        result = subprocess.run([dumpbin, "/nologo", "/disasm", str(obj)], capture_output=True, text=True,
                                errors="replace")
        lines = [l for l in result.stdout.splitlines() if not l.startswith(("Dump of file", "File Type"))]
        text = "\n".join(lines) + "\n"
        name = obj.parent.parent.name.replace(".dir", "") + "__" + obj.stem
        (out_dir / (name + ".txt")).write_text(text, encoding="utf-8")
        return name, hashlib.sha256(text.encode()).hexdigest()

    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        manifest = dict(sorted(pool.map(one, objects)))
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"{len(manifest)} objects dumped to {out_dir}")


def normalize(text, strings=False, lines=False):
    text = SCOPE_MANGLED.sub("?N??", text)
    text = SCOPE_DEMANGLED.sub("`N'", text)
    if strings:
        text = STRING_SYMBOL.sub("??_C@STR", text)
    if lines:
        text = LINE_IMMEDIATE.sub(r"IMM LINE\1LINE", text)
    summary = text.find("\n  Summary\n")
    return text if summary < 0 else text[:summary]


def functions(text):
    result, name, body = {}, None, []
    for line in text.splitlines():
        if line and not line[0].isspace():
            if name:
                result.setdefault(name, []).append("\n".join(body))
            name, body = line, []
        elif line.strip():
            body.append(line)
    if name:
        result.setdefault(name, []).append("\n".join(body))
    return {k: sorted(v) for k, v in result.items()}


def compare(before, after, show):
    a_manifest = json.loads((before / "manifest.json").read_text())
    b_manifest = json.loads((after / "manifest.json").read_text())
    levels, different = {}, []
    for name in sorted(set(a_manifest) | set(b_manifest)):
        if name not in a_manifest or name not in b_manifest:
            different.append((name, ["object only on one side"]))
            continue
        if a_manifest[name] == b_manifest[name]:
            levels.setdefault("identical", []).append(name)
            continue
        a_text = (before / (name + ".txt")).read_text(encoding="utf-8")
        b_text = (after / (name + ".txt")).read_text(encoding="utf-8")
        for level, strings, lines in (("equal-normalized", False, False), ("equal-mod-strings", True, False),
                                      ("equal-mod-lines", True, True)):
            if functions(normalize(a_text, strings, lines)) == functions(normalize(b_text, strings, lines)):
                levels.setdefault(level, []).append(name)
                break
        else:
            a = functions(normalize(a_text, True, True))
            b = functions(normalize(b_text, True, True))
            names = [n for n in sorted(set(a) | set(b)) if a.get(n) != b.get(n)]
            different.append((name, names))
    for level in ("identical", "equal-normalized", "equal-mod-strings", "equal-mod-lines"):
        names = levels.get(level, [])
        detail = "" if level in ("identical", "equal-normalized") else f" {names}"
        print(f"{level}: {len(names)}{detail}")
    print(f"DIFFERENT: {len(different)}")
    for name, names in different:
        print(f"== {name}: {len(names)} differing functions")
        for function in names[:show]:
            print("   " + function[:220])
    return 1 if different else 0


def function_diff(before_file, after_file):
    def table(path):
        text = normalize(pathlib.Path(path).read_text(encoding="utf-8"), True, True)
        result = {}
        for name, bodies in functions(text).items():
            result[name] = [[TARGET.sub("T", ADDRESS.sub("", l))[19:] for l in body.splitlines()] for body in bodies]
        return result

    a = table(before_file)
    b = table(after_file)
    for name in sorted(set(a) | set(b)):
        if a.get(name) != b.get(name):
            print("== " + name[:220])
            if name in a and name in b:
                diff = difflib.unified_diff(a[name][0], b[name][0], lineterm="", n=0)
                for line in [l for l in diff if l[:1] in "+-" and l[:3] not in ("+++", "---")][:30]:
                    print("     " + line[:200])


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("dump", "compare", "functions"):
        raise SystemExit(__doc__)
    command, first, second = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    if command == "dump":
        dump(first, second)
    elif command == "compare":
        show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 10
        sys.exit(compare(first, second, show))
    else:
        function_diff(first, second)


if __name__ == "__main__":
    main()
