#!/usr/bin/env python3
"""mnist_mlp whole-process wall clock: NLang cpu leg vs cuda leg.

Times `nvm main.npkg` end to end (process start, data load, training,
evaluation) on both tensor backends, the same whole-program methodology
the numpy/cupy comparison (mnist_numpy_cpu_gpu_bench.py) uses. The
question this answers: after the asynchronous-execution rework, does
the device leg still pay a per-operation synchronization wait?

Manual tool, intentionally NOT wired into ctest (timing asserts are
flaky in CI). The cuda leg needs a machine with the CUDA plugin and a
working GPU. Usage (run from anywhere):

    python tests/bench/nlang_mnist_wall.py [--nvm PATH] [--npkg PATH]
        [--runs N]

Defaults: --nvm <repo>/build-bench/src/tools/nvm/Release/nvm.exe,
--npkg <repo>/examples/mnist_mlp/main.npkg (rebuild the package with
`ncc build -p mnist_mlp.nproj -o main.npkg` first — a stale package
fails with a module-version error). Reports the median/min/max of both
legs and the ratio.

Measurement discipline: run on a quiet machine — a concurrent test
suite inflates both legs and makes the ratio meaningless (a controlled
A/B with the e2e suite running inflated the cpu leg from 1.5 s to
2.9 s).
"""

import argparse
import os
import pathlib
import statistics
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_NVM = REPO / "build-bench" / "src" / "tools" / "nvm" / "Release" / "nvm.exe"
DEFAULT_NPKG = REPO / "examples" / "mnist_mlp" / "main.npkg"


def bench(nvm, npkg, backend, runs):
    env = dict(os.environ)
    if backend:
        env["NLANG_TENSOR_BACKEND"] = backend
    walls = []
    for _ in range(runs):
        t = time.perf_counter()
        r = subprocess.run([str(nvm), str(npkg)], capture_output=True,
                           cwd=str(npkg.parent), env=env)
        walls.append(time.perf_counter() - t)
        if r.returncode != 0:
            sys.exit(f"nvm failed (rc={r.returncode}): {r.stderr[-300:]!r}")
    return statistics.median(walls), min(walls), max(walls)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nvm", type=pathlib.Path, default=DEFAULT_NVM)
    parser.add_argument("--npkg", type=pathlib.Path, default=DEFAULT_NPKG)
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()

    cpu = bench(args.nvm, args.npkg, None, args.runs)
    print(f"cpu:  median {cpu[0]*1000:7.1f} ms  "
          f"(min {cpu[1]*1000:.1f}, max {cpu[2]*1000:.1f})")
    gpu = bench(args.nvm, args.npkg, "cuda", args.runs)
    print(f"cuda: median {gpu[0]*1000:7.1f} ms  "
          f"(min {gpu[1]*1000:.1f}, max {gpu[2]*1000:.1f})")
    print(f"ratio cuda/cpu = {gpu[0]/cpu[0]:.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
