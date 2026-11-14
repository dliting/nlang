#!/usr/bin/env python3
"""tensor SIMD kernel micro-benchmark: matmul GFLOP/s at budget shapes.

Times `tensor.matmul` inside nvm (the shipped kernel path, not the
unit-test harness) and reports GFLOP/s per shape, derived by two-point
extrapolation — wall(1 round) vs wall(R rounds), so process startup and
stdlib packing cancel out of the slope. Manual tool — intentionally NOT
wired into ctest. Usage:

    python tensor_simd_bench.py [build_dir]

build_dir defaults to the repo's build-bench (ncc/nvm under
src/tools/<tool>/Release). The budget shapes (2026-11-11 tensor vector
ops spec §9): 512x512 float32 >= 20 GFLOP/s and the MNIST layer-1 shape
(32x784)·(784x100) >= 8 GFLOP/s on the AVX2 tier.
"""

import pathlib
import statistics
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]

#(rows, inner, cols, rounds): 512-cube is the "large" budget shape; the
#MNIST trio is the workload the end-to-end benchmark rides.
SHAPES = [
    (512, 512, 512, 24),
    (32, 784, 100, 4000),
    (100, 32, 784, 2000),
]

#The @N@ placeholders keep the .n source brace-clean (stage bench
#convention); arange/fill give deterministic operands, sum prints the
#checksum so a mis-compiled kernel cannot silently time itself.
SRC_TEMPLATE = """
import io;
import tensor;
int main() {
    int m = @M@;
    int k = @K@;
    int n = @N@;
    float[,] a = float[m, k];
    float[,] b = float[k, n];
    for (int i = 0; i < m; i += 1) {
        for (int j = 0; j < k; j += 1) {
            a[i, j] = (((i * 31 + j * 17) % 19 - 9) / 4.0) as float;
        }
    }
    for (int i = 0; i < k; i += 1) {
        for (int j = 0; j < n; j += 1) {
            b[i, j] = (((i * 13 + j * 7) % 11 - 5) / 4.0) as float;
        }
    }
    float[,] c = float[m, n];
    for (int r = 0; r < @ROUNDS@; r += 1) {
        c = tensor.matmul(a, b);
    }
    io.print(tensor.sum(c));
    return 0;
}
"""


def wall_seconds(nvm, npkg, cwd):
    start = time.perf_counter()
    subprocess.run([str(nvm), str(npkg.resolve())], cwd=cwd, check=True,
                   capture_output=True, text=True)
    return time.perf_counter() - start


def main() -> int:
    build_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                             else REPO / "build-bench").resolve()
    ncc = build_dir / "src/tools/ncc/Release/ncc.exe"
    nvm = build_dir / "src/tools/nvm/Release/nvm.exe"
    with tempfile.TemporaryDirectory() as scratch:
        work = pathlib.Path(scratch)
        print("matmul GFLOP/s (tier follows NLANG_TENSOR_TIER in this "
              "shell; unset = detected)")
        for rows, inner, cols, rounds in SHAPES:
            src = (SRC_TEMPLATE.replace("@M@", str(rows))
                   .replace("@K@", str(inner)).replace("@N@", str(cols)))
            src1 = src.replace("@ROUNDS@", "1")
            srcR = src.replace("@ROUNDS@", str(rounds))
            (work / "bench1.n").write_text(src1)
            (work / "benchR.n").write_text(srcR)
            subprocess.run([str(ncc), "build", "bench1.n",
                            "-o", "bench1.ncu"], cwd=work, check=True,
                           capture_output=True)
            subprocess.run([str(ncc), "build", "benchR.n",
                            "-o", "benchR.ncu"], cwd=work, check=True,
                           capture_output=True)
            t1 = statistics.median(wall_seconds(nvm, work / "bench1.ncu",
                                                work) for _ in range(3))
            tR = statistics.median(wall_seconds(nvm, work / "benchR.ncu",
                                                work) for _ in range(3))
            perRound = (tR - t1) / (rounds - 1)
            gflop = 2.0 * rows * inner * cols * 1e-9
            flops = gflop / perRound
            print(f"  ({rows}x{inner})·({inner}x{cols}): "
                  f"{flops:8.2f} GFLOP/s   ({perRound * 1e3:.3f} ms/round)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
