"""check_stdlib_generation.py -- verify stdlib/*.n generated from kStdLibTable.

Tests:
1. stdlib/io.n, math.n, fs.n exist with a namespace block
2. Every kStdLibTable entry has a corresponding native declaration
3. Signatures match (return type, parameter count, parameter types)
4. Every native function has at least one documentation comment line
5. Semantic parameter names (writeFile -> path/content, not s/x)

Usage: check_stdlib_generation.py
Exit 0 = all checks pass; exit 1 = failure.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STD_H = ROOT / "include" / "nlang" / "vm" / "StdLib.h"
STDLIB_DIR = ROOT / "stdlib"

RET_TYPE = {
    "SLRT_Void": "void",
    "SLRT_Int32": "int",
    "SLRT_Float": "float",
    "SLRT_String": "string",
    "SLRT_ListString": "List<string>",
}

PARAM_TYPE = {
    "RTK_Int32": "int",
    "RTK_Float": "float",
    "RTK_String": "string",
}

# Expected semantic parameter names for spot checks.
EXPECTED_PARAM_NAMES = {
    ("io", "writeFile"): ["path", "content"],
    ("io", "appendFile"): ["path", "content"],
    ("io", "readFile"): ["path"],
    ("math", "atan2"): ["y", "x"],
    ("math", "randomi"): ["min", "max"],
}


def fail(msg: str):
    print(f"FAIL: {msg}")
    sys.exit(1)


def parse_kstdlib(src: str):
    """Extract entries from kStdLibTable (same logic as generator)."""
    start = src.find("kStdLibTable[] =")
    if start < 0:
        fail("kStdLibTable not found in StdLib.h")
    i = src.find("{", start) + 1
    entries = []
    depth = 1
    entry_start = -1
    while i < len(src) and depth > 0:
        ch = src[i]
        if ch == "{":
            if depth == 1:
                entry_start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 1 and entry_start >= 0:
                entries.append(src[entry_start:i+1])
                entry_start = -1
        i += 1
    return entries


def parse_entry(raw: str):
    """Parse one entry string into fields."""
    inner = raw[1:raw.rfind("}")]
    parts, cur, d = [], "", 0
    for ch in inner:
        if ch == "{":
            d += 1; cur += ch
        elif ch == "}":
            d -= 1; cur += ch
        elif ch == "," and d == 0:
            parts.append(cur.strip()); cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    ns = parts[0].strip('"')
    name = parts[1].strip('"')
    kinds = [k.strip() for k in parts[2].strip("{} ").split(",") if k.strip()]
    ret = parts[5]
    coerce = len(parts) > 7 and parts[7] == "true"
    return ns, name, kinds, ret, coerce


def parse_generated_n(filepath: Path):
    """Parse a generated .n file.

    Returns (ns_name, funcs); each func is a dict with ret, name,
    param_types, param_names and doc (list of comment lines)."""
    if not filepath.is_file():
        fail(f"missing file: {filepath}")
    text = filepath.read_text(encoding="utf-8")
    m = re.search(r"namespace\s+(\w+)\s*\{(.*?)\}", text, re.DOTALL)
    if not m:
        fail(f"no namespace block in {filepath.name}")
    ns_name = m.group(1)
    body = m.group(2)
    funcs = []
    doc: list[str] = []
    for line in body.splitlines():
        line = line.strip()
        if line.startswith("//"):
            doc.append(line[2:].strip())
            continue
        m2 = re.match(r"native\s+([\w<>, ]+?)\s+(\w+)\s*\(([^)]*)\)\s*;", line)
        if m2:
            ret = m2.group(1).strip()
            name = m2.group(2)
            params_str = m2.group(3).strip()
            param_types, param_names = [], []
            if params_str:
                for p in params_str.split(","):
                    tokens = p.strip().split()
                    param_types.append(tokens[0])
                    param_names.append(tokens[1])
            funcs.append({"ret": ret, "name": name,
                          "param_types": param_types,
                          "param_names": param_names, "doc": doc})
            doc = []
    return ns_name, funcs


def main():
    if not STD_H.is_file():
        fail(f"StdLib.h not found: {STD_H}")
    src = STD_H.read_text(encoding="utf-8")

    raw_entries = parse_kstdlib(src)
    expected = {}
    for raw in raw_entries:
        ns, name, kinds, ret, coerce = parse_entry(raw)
        expected.setdefault(ns, []).append((name, kinds, ret, coerce))

    total = sum(len(v) for v in expected.values())
    print(f"found {total} entries in kStdLibTable: "
          f"{', '.join(f'{ns}={len(v)}' for ns, v in sorted(expected.items()))}")

    for ns, entries in sorted(expected.items()):
        path = STDLIB_DIR / f"{ns}.n"
        gen_ns, funcs = parse_generated_n(path)
        if gen_ns != ns:
            fail(f"{path.name}: namespace is '{gen_ns}', expected '{ns}'")
        gen_by_name = {f["name"]: f for f in funcs}
        if len(gen_by_name) != len(entries):
            fail(f"{path.name}: has {len(gen_by_name)} functions, "
                 f"expected {len(entries)}")
        for name, kinds, ret, coerce in entries:
            if name not in gen_by_name:
                fail(f"{path.name}: missing function '{name}'")
            f = gen_by_name[name]
            expected_ret = RET_TYPE[ret]
            if f["ret"] != expected_ret:
                fail(f"{ns}.{name}: return type '{f['ret']}', "
                     f"expected '{expected_ret}'")
            expected_types = (["any"] * len(kinds)) if coerce \
                else [PARAM_TYPE[k] for k in kinds]
            if f["param_types"] != expected_types:
                fail(f"{ns}.{name}: param types {f['param_types']}, "
                     f"expected {expected_types}")
            # Every function must carry a documentation comment.
            if not f["doc"]:
                fail(f"{ns}.{name}: missing documentation comment")
            # Spot-check semantic parameter names.
            key = (ns, name)
            if key in EXPECTED_PARAM_NAMES \
                    and f["param_names"] != EXPECTED_PARAM_NAMES[key]:
                fail(f"{ns}.{name}: param names {f['param_names']}, "
                     f"expected {EXPECTED_PARAM_NAMES[key]}")
        print(f"OK: {path.name} ({len(entries)} functions)")

    print("all checks passed")


if __name__ == "__main__":
    main()
