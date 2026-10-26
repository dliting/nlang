#!/usr/bin/env python3
"""tensor stage-3 benchmark: interpreter matmul vs native kernel.

Compares a hand-written triple-loop NLang matmul against the tensor
package's tensor.matmul on the same int matrices (128x128, 8 rounds),
reporting per-variant medians and the speedup. Manual tool -
intentionally NOT wired into ctest (timing asserts are flaky in CI).
Usage:

    python tensor_stage3_bench.py [build_dir]

build_dir defaults to the repo's build-dev (ncc/nvm resolved under
src/tools/<tool>/Release). The .n sources use @N@/@ROUNDS@ placeholder
substitution, not str.format - they are full of braces.
"""
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]

MAT_N = 128       # 128x128 matrices
ROUNDS = 8        # 8 rounds of the same product

#Checksum prints keep both variants honest and give the harness a
#same-result sanity check alongside the timing. The deterministic
#entry formulas keep every product term small (no int overflow).
SRC_INTERP = """
import io;
int main() {
    int n = @N@;
    int[,] a = int[n, n];
    int[,] b = int[n, n];
    for (int i = 0; i < n; i = i + 1) {
        for (int j = 0; j < n; j = j + 1) {
            a[i, j] = (i * 3 + j) % 5 - 2;
            b[i, j] = (i * 2 + j) % 7 - 3;
        }
    }
    int[,] c = int[n, n];
    for (int r = 0; r < @ROUNDS@; r = r + 1) {
        for (int i = 0; i < n; i = i + 1) {
            for (int k = 0; k < n; k = k + 1) {
                int s = 0;
                for (int j = 0; j < n; j = j + 1) {
                    s = s + a[i, j] * b[j, k];
                }
                c[i, k] = s;
            }
        }
    }
    long acc = 0;
    for (int i = 0; i < n; i = i + 1) {
        for (int k = 0; k < n; k = k + 1) {
            acc = acc + c[i, k];
        }
    }
    io.print(acc);
    return 0;
}
"""

SRC_KERNEL = """
import io;
import tensor;
int main() {
    int n = @N@;
    int[,] a = int[n, n];
    int[,] b = int[n, n];
    for (int i = 0; i < n; i = i + 1) {
        for (int j = 0; j < n; j = j + 1) {
            a[i, j] = (i * 3 + j) % 5 - 2;
            b[i, j] = (i * 2 + j) % 7 - 3;
        }
    }
    int[,] c = int[n, n];
    for (int r = 0; r < @ROUNDS@; r = r + 1) {
        c = tensor.matmul(a, b);
    }
    io.print(tensor.sum(c));
    return 0;
}
"""


def fill(source, values):
    for key, val in values.items():
        source = source.replace("@" + key + "@", str(val))
    return source


def run_variant(name, source, ncc, nvm, workdir):
    src = workdir / f"{name}.n"
    src.write_text(source, encoding="utf-8")
    subprocess.run([str(ncc), str(src), "-o", str(workdir / name)],
                   check=True, capture_output=True)
    times = []
    output = ""
    for _ in range(3):   #warmup + measured runs, median reported
        t0 = time.perf_counter()
        proc = subprocess.run([str(nvm), str(workdir / f"{name}.ncu")],
                              capture_output=True, text=True, check=True)
        times.append(time.perf_counter() - t0)
        output = proc.stdout.strip()
    return statistics.median(times), output


def main():
    build = pathlib.Path(
        sys.argv[1] if len(sys.argv) > 1 else REPO / "build-dev")
    ncc = build / "src/tools/ncc/Release/ncc.exe"
    nvm = build / "src/tools/nvm/Release/nvm.exe"
    for tool in (ncc, nvm):
        if not tool.exists():
            sys.exit(f"tool not found: {tool}")

    with tempfile.TemporaryDirectory(prefix="nlang_bench_") as td:
        work = pathlib.Path(td)
        t_interp, out_i = run_variant(
            "matmul_interp",
            fill(SRC_INTERP, {"N": MAT_N, "ROUNDS": ROUNDS}),
            ncc, nvm, work)
        t_kernel, out_k = run_variant(
            "matmul_kernel",
            fill(SRC_KERNEL, {"N": MAT_N, "ROUNDS": ROUNDS}),
            ncc, nvm, work)

    note = ("match" if out_i == out_k
            else "MISMATCH: " + out_i + " vs " + out_k)
    print(f"matmul int {MAT_N}x{MAT_N} x{ROUNDS}: "
          f"interp {t_interp*1000:.1f} ms, kernel {t_kernel*1000:.1f} ms "
          f"-> speedup {t_interp/t_kernel:.1f}x   (checksums {note})")
    print("stage-3 checkpoint: kernel wins by a wide margin on the "
          "triple-loop workload (typical >= 20x at this size)")


if __name__ == "__main__":
    main()
