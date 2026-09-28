#!/usr/bin/env python3
"""Phase 4c guard: the VM must not carry a built-in standard library.

io/math/fs are library .n sources compiled by the compiler whose native
functions are served by nlang_<ns>.dll through the NativeHost ABI. The
hardcoded path that preceded that (kStdLibTable + the ExecuteIntrinsicMath /
Io / Fs families + their intrinsic ids) is retired; this guard fails if any
piece of it returns, and fails if the library sources it depends on are
deleted instead of the table.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


# 1. Retired symbols and ids must be gone from all C++ sources.
BANNED = [
    r"\bkStdLibTable\b", r"\bStdLibEntry\b", r"\bFindStdLibFunction\b",
    r"\bIsStdLibNamespaceName\b",
    r"\bExecuteIntrinsicMath\b", r"\bExecuteIntrinsicIo\b",
    r"\bExecuteIntrinsicFs\b",
    r"\bINTR_Math_", r"\bINTR_Io_", r"\bINTR_FileSystem_",
    r"\bkMathIntrinsic", r"\bkIoIntrinsic", r"\bkFileSystemIntrinsic",
]
for sub in ("src", "include"):
    for path in sorted((ROOT / sub).rglob("*")):
        if not path.is_file() or path.suffix not in (
                ".h", ".hpp", ".cpp", ".cxx", ".y", ".l"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in BANNED:
            check(not re.search(pattern, text),
                  f"{path.relative_to(ROOT)}: retired symbol {pattern} present")

# 2. The three family TUs must not exist (the string family and the file
# stream family are different code paths and stay).
for gone in ("src/vm/IntrinsicsMath.cpp", "src/vm/IntrinsicsIo.cpp",
             "src/vm/IntrinsicsFs.cpp"):
    check(not (ROOT / gone).exists(), f"{gone} must be deleted")

# 3. Positive control: the mechanism the deletion leans on is still there.
NATIVE_DECL = re.compile(r"^\s*native\s+\S", re.MULTILINE)
for ns, expected in (("math", 25), ("io", 5), ("fs", 8)):
    source = (ROOT / "stdlib" / f"{ns}.n").read_text(encoding="utf-8")
    found = len(NATIVE_DECL.findall(source))
    check(found == expected,
          f"stdlib/{ns}.n must declare {expected} native functions, found {found}")

# 4. The surviving intrinsic path must survive: string methods still dispatch.
backend = (ROOT / "src/vm/VmExecutorIntrinsics.cpp").read_text(encoding="utf-8")
check("ExecuteIntrinsicString" in backend,
      "the string-method family dispatch must remain wired")

if failures:
    for f in failures:
        print("FAIL: " + f)
    print(f"{len(failures)} problem(s): the built-in standard library is not fully retired")
    sys.exit(1)
print("no builtin stdlib: table, families and ids are retired; stdlib sources intact")
