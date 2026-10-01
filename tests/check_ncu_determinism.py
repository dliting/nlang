"""check_nmod_determinism.py -- .ncu output must be byte-reproducible.

The same ncc compiling the same source must produce byte-identical .ncu
output on every run. Regression net for the class-field access shadowing
bug (2026-09-25): SnClassField declared an uninitialized derived-class
m_access member that Access() returned, and the backend serialized it
into the class table's fieldAccess bytes — so two runs of the same
compiler emitted different bytes for the same program. The fixture
covers bare and explicit access modifiers, inheritance, structs, methods
and several field types so every per-class serialized vector reaches the
wire.

Usage: check_nmod_determinism.py <ncc.exe>
Exit 0 = three compiles byte-identical; exit 1 = any failure.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

COMPILE_RUNS = 3
COMPILE_TIMEOUT = 60  # seconds per compile

FIXTURE = """\
class DetBare { int a; }
class DetPrivate { private int a; }
class DetPublic { public int a; }
class DetProtected { protected int a; }
class DetDerived : DetPrivate { int b; }
struct DetPoint { int x; int y; }
class DetMixed {
    int a;
    int b;
    float f;
    string s;
    int sum() { return a + b; }
}
int main() { return 0; }
"""


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_nmod_determinism.py <ncc>")
    ncc = Path(sys.argv[1]).resolve()
    if not ncc.is_file():
        sys.exit(f"ncc not found: {ncc}")

    outputs = []
    with tempfile.TemporaryDirectory() as raw_dir:
        work = Path(raw_dir)
        src = work / "det_fixture.n"
        src.write_text(FIXTURE, encoding="utf-8")
        #Same stem in separate directories: ncc derives the module name
        #from the output stem, so differing stems would legitimately
        #differ in the embedded module-name bytes.
        for i in range(COMPILE_RUNS):
            out_dir = work / f"run{i}"
            out_dir.mkdir()
            out = out_dir / "det_fixture.ncu"
            result = subprocess.run(
                [str(ncc), "build", str(src), "-o", str(out)],
                capture_output=True, text=True, timeout=COMPILE_TIMEOUT)
            if result.returncode != 0:
                sys.exit(f"fixture compile failed:\n{result.stderr}")
            outputs.append(out.read_bytes())

    if not all(b == outputs[0] for b in outputs):
        sizes = [len(b) for b in outputs]
        sys.exit("ncu determinism FAILED: same source and compiler "
                 f"produced differing bytes across {COMPILE_RUNS} runs "
                 f"(sizes {sizes})")
    print("ncu determinism: OK")


if __name__ == "__main__":
    main()
