#!/usr/bin/env python3
"""mnist_mlp wall-time: numpy (CPU) vs CuPy (GPU), same workload.

Times the two-layer perceptron training (784 -> 100 -> 10, batch 32, 40
epochs, lr 0.5) on the example's frozen MNIST subset with both array
stacks. numpy runs on the CPU (OpenBLAS, multithreaded); cupy runs the
same ops on the GPU (RTX 3080 Laptop). The question this answers: on this
small-matrix workload, does the GPU (like NLang's CUDA backend) get
dominated by per-kernel launch overhead and run slower than the CPU?

Manual tool, intentionally NOT wired into ctest (timing asserts are flaky
in CI). The GPU leg needs cupy-cuda12x (`pip install cupy-cuda12x`); the
CPU leg runs without it. Usage:

    python mnist_numpy_cpu_gpu_bench.py [--runs N] [--warmup N]

The frozen subset is loaded once from disk (not timed). A warmup run
initializes the CUDA context and loads the kernels before the timed runs,
so the GPU numbers are steady-state. The timed region is the full train
(Xavier init + 40-epoch forward/backward/SGD + evaluate); the per-run
GPU data upload (train_x, ~6 MB) is included, matching the whole-program
methodology of the NLang `nvm` timing.
"""

import argparse
import pathlib
import statistics
import sys
import time

import numpy as np

REPO = pathlib.Path(__file__).resolve().parents[2]
DATA = REPO / "examples" / "mnist_mlp" / "data"

pixels, classes = 784, 10
hidden, batch, epochs, lr = 100, 32, 40, 0.5


def load():
    """Load the example's frozen subset once (CPU, not timed)."""
    train_x = np.fromfile(DATA / "train_images.f32", dtype=np.float32)
    train_x = train_x[:2000 * pixels].reshape(2000, pixels)
    train_y = np.fromfile(DATA / "train_labels.u8", dtype=np.uint8)[:2000].astype(np.int64)
    test_x = np.fromfile(DATA / "test_images.f32", dtype=np.float32)
    test_x = test_x[:1000 * pixels].reshape(1000, pixels)
    test_y = np.fromfile(DATA / "test_labels.u8", dtype=np.uint8)[:1000].astype(np.int64)
    return train_x, train_y, test_x, test_y


def train(mod, train_x, train_y, test_x, test_y):
    """One full training run on the array module mod (np or cp).

    mod is numpy (CPU) or cupy (GPU); the ops are identical for both.
    Returns the final test accuracy.
    """
    rng = mod.random.default_rng(42)
    w1 = rng.uniform(-1, 1, (pixels, hidden)).astype(mod.float32) * mod.sqrt(6.0 / (pixels + hidden))
    b1 = mod.zeros(hidden, dtype=mod.float32)
    w2 = rng.uniform(-1, 1, (hidden, classes)).astype(mod.float32) * mod.sqrt(6.0 / (hidden + classes))
    b2 = mod.zeros(classes, dtype=mod.float32)
    #asanyarray: numpy is a no-op on an ndarray; on cupy it is the H2D upload
    x = mod.asanyarray(train_x)
    y = mod.asanyarray(train_y)
    tx = mod.asanyarray(test_x)
    ty = mod.asanyarray(test_y)

    def forward(a):
        z1 = a @ w1 + b1
        h = mod.maximum(z1, 0.0)
        return h, h @ w2 + b2

    def softmax_rows(logits):
        shifted = logits - logits.max(axis=1, keepdims=True)
        e = mod.exp(shifted)
        return e / e.sum(axis=1, keepdims=True)

    def mean_cross_entropy(logits, labels):
        row_max = logits.max(axis=1)
        log_sum = mod.log(mod.exp(logits - row_max[:, None]).sum(axis=1)) + row_max
        return float(mod.mean(log_sum - logits[mod.arange(len(labels)), labels]))

    def backward(a, h, probs, labels):
        n = len(labels)
        one_hot = mod.zeros((n, classes), dtype=mod.float32)
        one_hot[mod.arange(n), labels] = 1.0
        d_logits = (probs - one_hot) / n
        g_w2 = h.T @ d_logits
        g_b2 = d_logits.sum(axis=0)
        d_hidden = d_logits @ w2.T
        d_z1 = d_hidden * (h > 0)
        g_w1 = a.T @ d_z1
        g_b1 = d_z1.sum(axis=0)
        return g_w1, g_b1, g_w2, g_b2

    batches = len(x) // batch
    first_loss = last_loss = 0.0
    for epoch in range(epochs):
        epoch_loss = 0.0
        for b in range(batches):
            a = x[b * batch:(b + 1) * batch]
            labels = y[b * batch:(b + 1) * batch]
            h, logits = forward(a)
            probs = softmax_rows(logits)
            epoch_loss += mean_cross_entropy(logits, labels)
            g_w1, g_b1, g_w2, g_b2 = backward(a, h, probs, labels)
            w1 -= lr * g_w1; b1 -= lr * g_b1
            w2 -= lr * g_w2; b2 -= lr * g_b2
        epoch_loss /= batches
        if epoch == 0:
            first_loss = epoch_loss
        last_loss = epoch_loss
    probs = softmax_rows(forward(tx)[1])
    acc = float(mod.mean(probs.argmax(axis=1) == ty))
    assert last_loss < first_loss and acc >= 0.85, "workload failed its gate"
    return acc


def bench(mod, runs, warmup):
    data = load()
    for _ in range(warmup):
        train(mod, *data)
    walls = []
    acc = None
    for _ in range(runs):
        t = time.perf_counter()
        acc = train(mod, *data)
        walls.append(time.perf_counter() - t)
    return statistics.median(walls), min(walls), max(walls), acc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=1)
    args = parser.parse_args()

    cpu = bench(np, args.runs, args.warmup)
    print(f"numpy (CPU)  median {cpu[0]*1000:8.1f} ms  (min {cpu[1]*1000:.1f}, max {cpu[2]*1000:.1f})  acc {cpu[3]:.3f}")

    try:
        import cupy as cp
    except ImportError:
        print("cupy  (GPU)    not installed -- `pip install cupy-cuda12x`")
        return 0
    gpu = bench(cp, args.runs, args.warmup)
    print(f"cupy  (GPU)  median {gpu[0]*1000:8.1f} ms  (min {gpu[1]*1000:.1f}, max {gpu[2]*1000:.1f})  acc {gpu[3]:.3f}")
    print(f"ratio GPU/CPU  = {gpu[0]/cpu[0]:.2f}x  (GPU is {gpu[0]/cpu[0]*100:.1f}% of CPU time)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
