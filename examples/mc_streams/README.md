# mc_streams — Monte Carlo π over device streams

The v0.14.9 stream-orchestration example: four device streams estimate π
by Monte Carlo, each drawing points from its own disjoint PRNG window
and computing its inside mask on-device; the main stream joins the four
and merges the partial counts.

## The shape

- `tensor.streamCreate()` mints a stream handle; every op enqueued
  between `tensor.streamPush(s)` and `tensor.streamPop()` lands on that
  stream, so the four chunks' kernels run concurrently on the device.
- The PRNG windows are disjoint by construction: each chunk's
  `tensor.uniform` snapshot is fixed at enqueue time (host-thread
  order), so no chunk sees another's draws and the K-stream union
  reproduces the single-stream sequence.
- `tensor.seed(7)` pins the global engine (seed also resets the
  window cursor), so the printed estimate is identical on every
  run; without a seed the engine starts from an arbitrary seed
  per run and the estimate lands anywhere within its statistical
  error.
- `tensor.streamJoin(s)` orders the handoff: the main stream's
  subsequent reads see the chunk's completed writes.
- On the `cpu` and `mirror` backends every stream call is a no-op that
  keeps the program identical — same draws, same estimate, one backend
  up to the wall clock.

## Build and run

From this directory (the DLLs beside the sources are the runtime
modules copied from a Release build; see the top-level README):

    ncc build -p mc_streams.nproj -o mc_streams.npkg
    nvm mc_streams.npkg

Expected output on the CUDA backend (RTX-class GPU, ~2× faster than the
single-stream spelling — the four chunk kernels overlap):

    pi estimate: 3.14xx  (inside 3141xxx of 4000000)

The estimate is reproducible: fixed point count, `tensor.seed(7)`
pinning the engine, enqueue-order-fixed windows.

## The PyTorch twin

`tests/bench/mc_streams_torch_bench.py` times the same workload
(4 × 1,000,000 points, four `torch.cuda.Stream` contexts, `torch.rand`
draws) and the NLang `nvm` run of this example, whole-program wall
clock, warmup included — the same methodology as the mnist benches.
Manual tool, intentionally not wired into ctest (timing asserts are
flaky in CI); needs `torch` with CUDA (`pip install torch --index-url
https://download.pytorch.org/whl/cu126`).

Measured on the reference machine (RTX 3080 Laptop, torch 2.14
cu126, 5 runs after warmup): NLang whole-program median 212 ms,
torch in-process median 4 ms. The ~50× gap is architectural, not a
kernel deficit: torch generates its randoms on the device, while
NLang draws every uniform host-side from its single deterministic
engine (the price of the reproducibility contract — the same seed
replays identical draws on every backend) and uploads the points.
The mask and partial-sum kernels still run on the device and the
four streams overlap them; a host-draw of 8 M words plus the
transfers dominates the wall clock.
