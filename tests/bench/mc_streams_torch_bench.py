#!/usr/bin/env python3
"""mc_streams wall-time: NLang (CUDA backend) vs PyTorch, same workload.

Times the Monte Carlo pi estimation of examples/mc_streams (four device
streams x 1,000,000 points: uniform draws, inside mask, partial sums,
stream joins) on both stacks. The PyTorch leg mirrors the example's
shape with torch.cuda.Stream contexts and torch.rand draws; the NLang
leg times the whole `nvm mc_streams.npkg` run (process start and package
load included), the same whole-program methodology as the mnist benches.

Manual tool, intentionally NOT wired into ctest (timing asserts are
flaky in CI). Needs torch with CUDA
(`pip install torch --index-url https://download.pytorch.org/whl/cu126`);
the script reports which legs ran and skips cleanly otherwise. Usage:

    python mc_streams_torch_bench.py [--runs N] [--warmup N]
        [--streams K] [--points N]
"""

import argparse
import os
import pathlib
import statistics
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
EXAMPLE = REPO / "examples" / "mc_streams"
PKG = EXAMPLE / "mc_streams.npkg"


def nlang_walls(runs, warmup):
    """Time whole `nvm` runs of the example (CUDA backend)."""
    nvm = _find_nvm()
    if nvm is None or not PKG.exists():
        print(f"nlang (GPU)   skipped (nvm or {PKG.name} not found;"
              " build the example first)")
        return None
    env = dict(os.environ, NLANG_TENSOR_BACKEND="cuda")
    cmd = [str(nvm), str(PKG)]
    for _ in range(warmup):
        subprocess.run(cmd, cwd=EXAMPLE, env=env, check=True,
                       stdout=subprocess.DEVNULL)
    walls = []
    estimate = ""
    for _ in range(runs):
        t = time.perf_counter()
        out = subprocess.run(cmd, cwd=EXAMPLE, env=env, check=True,
                             capture_output=True, text=True)
        walls.append(time.perf_counter() - t)
        estimate = out.stdout.strip()
    print(f"nlang (GPU)   {estimate}")
    return walls


def _find_nvm():
    """Locate a Release nvm beside the repo's build trees."""
    for cand in (REPO / "build-bench" / "src" / "tools" / "nvm" / "Release"
                 / "nvm.exe",):
        if cand.exists():
            return cand
    return None


def torch_walls(streams, points, runs, warmup):
    """Time the same workload on torch.cuda streams; returns (walls, pi)."""
    import torch

    if not torch.cuda.is_available():
        print("torch (GPU)   skipped (torch has no CUDA device)")
        return None, None
    device = torch.device("cuda")
    gen = torch.Generator(device=device).manual_seed(42)

    def one_run():
        masks = []
        handles = [torch.cuda.Stream(device=device) for _ in range(streams)]
        per = points // streams
        for s in handles:
            with torch.cuda.stream(s):
                x = torch.rand(per, device=device, generator=gen)
                y = torch.rand(per, device=device, generator=gen)
                masks.append((x * x + y * y) <= 1.0)
        current = torch.cuda.current_stream(device=device)
        for s in handles:  # the join: main stream waits each chunk
            current.wait_stream(s)
        inside = sum(int(m.sum().item()) for m in masks)
        return 4.0 * inside / (streams * per)

    for _ in range(warmup):
        one_run()
    torch.cuda.synchronize()
    walls = []
    estimate = None
    for _ in range(runs):
        t = time.perf_counter()
        estimate = one_run()
        torch.cuda.synchronize()
        walls.append(time.perf_counter() - t)
    return walls, estimate


def report(name, walls):
    print(f"{name:12s} median {statistics.median(walls)*1000:8.1f} ms"
          f"  (min {min(walls)*1000:.1f}, max {max(walls)*1000:.1f})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--streams", type=int, default=4)
    parser.add_argument("--points", type=int, default=4000000)
    args = parser.parse_args()

    nlang = nlang_walls(args.runs, args.warmup)
    try:
        import torch
    except ImportError:
        torch = None
    if torch is None:
        print("torch (GPU)   skipped (torch not installed;"
              " see README in examples/mc_streams)")
    if nlang:
        report("nlang", nlang)
    if torch is not None:
        walls, estimate = torch_walls(args.streams, args.points,
                                      args.runs, args.warmup)
        if walls:
            report("torch", walls)
            print(f"torch estimate {estimate:.5f}"
                  " (statistical range check, not an exact match)")
    if not nlang:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
