#!/usr/bin/env python3
"""mnist_mlp wall-time comparison: NLang VM vs an equivalent numpy program.

Trains the same two-layer perceptron (784 -> 100 -> 10, batch 32, 40
epochs, lr 0.5) on the example's frozen MNIST subset with both stacks
and reports per-variant medians of the run wall time plus the final
test accuracy. The two implementations are algorithmically identical
but seed their own PRNG, so losses are comparable only in shape — the
shared acceptance signal is the accuracy band (~0.89 on this subset).

Manual tool — intentionally NOT wired into ctest (timing asserts are
flaky in CI). Usage:

    python mnist_vs_numpy_bench.py [build_dir] [options]

    --runs N          timed runs per variant (default 3, median reported)
    --skip-numpy      time the NLang variant only
    --skip-nlang      time the numpy variant only
    --numpy-python X  interpreter that has numpy (default: current one)

build_dir defaults to the repo's build-bench (ncc/nvm resolved under
src/tools/<tool>/Release). The NLang side compiles the example once
(the way ctest's mnist_mlp_compile does) and times only `nvm` runs;
the numpy side times the whole data-load + train + evaluate program.
"""

import argparse
import pathlib
import statistics
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
EXAMPLE = REPO / "examples" / "mnist_mlp"

#The numpy reference reads the example's frozen data files and mirrors
#layers.n op-for-op: forward (matmul + bias broadcast + relu), softmax
#(row-max shifted), mean cross entropy, the four gradients, plain SGD.
#Weight init uses the same Xavier-uniform recipe under numpy's own RNG.
NUMPY_REF = r'''
import pathlib
import time
import numpy as np

DATA = pathlib.Path(r"@DATA@")
pixels, classes = 784, 10
hidden, batch, epochs, lr = 100, 32, 40, 0.5

train_x = np.fromfile(DATA / "train_images.f32", dtype=np.float32)[:2000*pixels].reshape(2000, pixels)
train_y = np.fromfile(DATA / "train_labels.u8", dtype=np.uint8)[:2000].astype(np.int64)
test_x = np.fromfile(DATA / "test_images.f32", dtype=np.float32)[:1000*pixels].reshape(1000, pixels)
test_y = np.fromfile(DATA / "test_labels.u8", dtype=np.uint8)[:1000].astype(np.int64)

rng = np.random.default_rng(42)
w1 = rng.uniform(-1, 1, (pixels, hidden)) * np.sqrt(6.0 / (pixels + hidden))
b1 = np.zeros(hidden, dtype=np.float32)
w2 = rng.uniform(-1, 1, (hidden, classes)) * np.sqrt(6.0 / (hidden + classes))
b2 = np.zeros(classes, dtype=np.float32)
w1 = w1.astype(np.float32); w2 = w2.astype(np.float32)

def forward(x):
    z1 = x @ w1 + b1
    h = np.maximum(z1, 0.0)
    return h, h @ w2 + b2

def softmax_rows(logits):
    shifted = logits - logits.max(axis=1, keepdims=True)
    e = np.exp(shifted)
    return e / e.sum(axis=1, keepdims=True)

def mean_cross_entropy(logits, labels):
    row_max = logits.max(axis=1)
    log_sum = np.log(np.exp(logits - row_max[:, None]).sum(axis=1)) + row_max
    return float(np.mean(log_sum - logits[np.arange(len(labels)), labels]))

def backward(x, h, probs, labels):
    n = len(labels)
    one_hot = np.zeros((n, classes), dtype=np.float32)
    one_hot[np.arange(n), labels] = 1.0
    d_logits = (probs - one_hot) / n
    g_w2 = h.T @ d_logits
    g_b2 = d_logits.sum(axis=0)
    d_hidden = d_logits @ w2.T
    d_z1 = d_hidden * (h > 0)
    g_w1 = x.T @ d_z1
    g_b1 = d_z1.sum(axis=0)
    return g_w1, g_b1, g_w2, g_b2

start = time.perf_counter()
batches = len(train_x) // batch
first_loss = last_loss = 0.0
for epoch in range(epochs):
    epoch_loss = 0.0
    for b in range(batches):
        x = train_x[b*batch:(b+1)*batch]
        labels = train_y[b*batch:(b+1)*batch]
        h, logits = forward(x)
        probs = softmax_rows(logits)
        epoch_loss += mean_cross_entropy(logits, labels)
        g_w1, g_b1, g_w2, g_b2 = backward(x, h, probs, labels)
        w1 -= lr * g_w1; b1 -= lr * g_b1
        w2 -= lr * g_w2; b2 -= lr * g_b2
    epoch_loss /= batches
    if epoch == 0:
        first_loss = epoch_loss
    last_loss = epoch_loss
probs = softmax_rows(forward(test_x)[1])
acc = float(np.mean(probs.argmax(axis=1) == test_y))
elapsed = time.perf_counter() - start

print(f"first loss {first_loss:.6f}")
print(f"last loss {last_loss:.6f}")
print(f"test accuracy {acc:.6f}")
print(f"numpy elapsed {elapsed:.3f}")
assert last_loss < first_loss and acc >= 0.85, "numpy reference failed its gate"
'''


def time_nlang(build_dir: pathlib.Path, runs: int) -> tuple[float, str]:
    ncc = build_dir / "src/tools/ncc/Release/ncc.exe"
    nvm = build_dir / "src/tools/nvm/Release/nvm.exe"
    npkg = build_dir / "e2e_out/mnist_mlp.npkg"
    npkg.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(ncc), "build", "-p", str(EXAMPLE / "mnist_mlp.nproj"),
                    "-o", str(npkg)], check=True, capture_output=True)
    walls, last_line = [], ""
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run([str(nvm), str(npkg.resolve())], cwd=EXAMPLE,
                              check=True, capture_output=True, text=True)
        walls.append(time.perf_counter() - start)
        last_line = proc.stdout.strip().splitlines()[-1]
    return statistics.median(walls), last_line


def time_numpy(runs: int) -> tuple[float, str]:
    src = NUMPY_REF.replace("@DATA@", str(EXAMPLE / "data"))
    walls, lines = [], []
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run([sys.executable, "-c", src], check=True,
                              capture_output=True, text=True)
        walls.append(time.perf_counter() - start)
        lines = proc.stdout.strip().splitlines()
    return statistics.median(walls), " | ".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", nargs="?",
                        default=str(REPO / "build-bench"))
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--skip-numpy", action="store_true")
    parser.add_argument("--skip-nlang", action="store_true")
    args = parser.parse_args()
    build_dir = pathlib.Path(args.build_dir)

    nlang_seconds = numpy_seconds = None
    if not args.skip_nlang:
        nlang_seconds, accuracy_line = time_nlang(build_dir, args.runs)
        print(f"nlang  median {nlang_seconds:8.3f} s   ({accuracy_line})")
    if not args.skip_numpy:
        numpy_seconds, detail = time_numpy(args.runs)
        print(f"numpy  median {numpy_seconds:8.3f} s   ({detail})")
    if nlang_seconds and numpy_seconds:
        print(f"ratio  nlang/numpy = {nlang_seconds / numpy_seconds:.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
