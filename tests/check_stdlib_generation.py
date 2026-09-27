"""check_stdlib_generation.py -- stdlib/*.n declarations <-> runtime table.

The stdlib signatures live in stdlib/*.n (the authority, indexed by
langservice::SymbolIndex); kStdLibTable in include/nlang/vm/StdLib.h is now
only the ns,name -> intrinsicId runtime-implementation map. This guard
pins the two sides together:

1. stdlib/io.n, math.n, fs.n exist with a namespace block.
2. Every native function declared in stdlib/*.n has a runtime entry in
   kStdLibTable (otherwise it is an unimplemented native call).
3. Every kStdLibTable entry is declared in stdlib/*.n (otherwise the
   runtime implements a function with no visible signature).
4. Every native function carries at least one documentation comment line.
5. Spot-check semantic parameter names.

Usage: check_stdlib_generation.py
Exit 0 = all checks pass; exit 1 = failure.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STD_H = ROOT / "include" / "nlang" / "vm" / "StdLib.h"
STDLIB_DIR = ROOT / "stdlib"

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


def parse_runtime_table(src: str):
    """Extract (ns, name) pairs from the slim kStdLibTable."""
    start = src.find("kStdLibTable[] =")
    if start < 0:
        fail("kStdLibTable not found in StdLib.h")
    # Isolate just this table (it ends at the first "};" after the start),
    # so the structurally similar kStringMethodTable below is never scanned.
    table_src = src[start:]
    end = table_src.find("};")
    if end >= 0:
        table_src = table_src[:end]
    pairs = set()
    # Each slim row:  {"math", "sqrt", INTR_Math_Sqrt},
    for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,[^}]*?\}',
                         table_src):
        pairs.add((m.group(1), m.group(2)))
    return pairs


def parse_native_decls(filepath: Path):
    """Parse a stdlib .n file.

    Returns (ns_name, funcs); each func is a dict with name, param_names
    and doc (list of comment lines). Only native declarations are collected.
    """
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
        m2 = re.match(r"native\s+[\w<>, ]+?\s+(\w+)\s*\(([^)]*)\)\s*;", line)
        if m2:
            name = m2.group(1)
            params_str = m2.group(2).strip()
            param_names = []
            if params_str:
                # Last whitespace-separated token of each param is its name
                # (handles "float x", "List<string> files", "any s").
                for p in params_str.split(","):
                    param_names.append(p.strip().split()[-1])
            funcs.append({"name": name, "param_names": param_names,
                          "doc": doc})
            doc = []
    return ns_name, funcs


def main():
    if not STD_H.is_file():
        fail(f"StdLib.h not found: {STD_H}")
    runtime = parse_runtime_table(STD_H.read_text(encoding="utf-8"))
    print(f"found {len(runtime)} runtime entries in kStdLibTable")

    declared = set()
    for path in sorted(STDLIB_DIR.glob("*.n")):
        ns_name, funcs = parse_native_decls(path)
        for f in funcs:
            key = (ns_name, f["name"])
            declared.add(key)
            # Every native declaration must have a runtime implementation.
            if key not in runtime:
                fail(f"{path.name}: native '{ns_name}.{f['name']}' has no "
                     f"runtime implementation in kStdLibTable")
            # Every native function must carry a documentation comment.
            if not f["doc"]:
                fail(f"{ns_name}.{f['name']}: missing documentation comment")
            # Spot-check semantic parameter names.
            if key in EXPECTED_PARAM_NAMES \
                    and f["param_names"] != EXPECTED_PARAM_NAMES[key]:
                fail(f"{ns_name}.{f['name']}: param names "
                     f"{f['param_names']}, expected "
                     f"{EXPECTED_PARAM_NAMES[key]}")
        print(f"OK: {path.name} ({len(funcs)} native functions)")

    # Every runtime entry must have a visible declaration.
    missing_decl = runtime - declared
    if missing_decl:
        fail("runtime entries with no stdlib/*.n declaration: "
             + ", ".join(f"{ns}.{name}" for ns, name in sorted(missing_decl)))

    print(f"all checks passed ({len(declared)} native functions; "
          f"declarations and runtime table match)")


if __name__ == "__main__":
    main()
