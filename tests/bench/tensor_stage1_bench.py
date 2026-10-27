#!/usr/bin/env python3
"""tensor stage-1 benchmark: interpreter loops vs native kernels.

Compares hand-written NLang loops against the tensor package's native
kernels on the same data (elementwise add over 256K doubles, dot over
64K doubles), reporting per-variant medians and the speedup. Manual
tool — intentionally NOT wired into ctest (timing asserts are flaky in
CI). Usage:

    python tensor_stage1_bench.py [build_dir]

build_dir defaults to the repo's build-dev (ncc/nvm resolved under
src/tools/<tool>/Release). The .n sources use @N@/@ROUNDS@ placeholder
substitution, not str.format — they are full of braces.
"""
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]

ELEMENTWISE_N = 262144   # 256K doubles
DOT_N = 65536            # 64K doubles
ROUNDS_ELEMENTWISE = 64
ROUNDS_DOT = 256

#Checksum prints keep both variants honest and give the harness a
#same-result sanity check alongside the timing.
SRC_INTERP = """
import io;
import tensor;
int main() {
    double[] a = tensor.arange(0.0, @N@.0, 1.0);
    double[] b = tensor.full(@N@, 1.0);
    double[] c = double[@N@];
    for (int r = 0; r < @ROUNDS@; r = r + 1) {
        for (int i = 0; i < @N@; i = i + 1) {
            c[i] = a[i] + b[i];
        }
    }
    io.print(tensor.sum(c));
    return 0;
}
"""

SRC_KERNEL = """
import io;
import tensor;
int main() {
    double[] a = tensor.arange(0.0, @N@.0, 1.0);
    double[] b = tensor.full(@N@, 1.0);
    double[] c = double[@N@];
    for (int r = 0; r < @ROUNDS@; r = r + 1) {
        c = tensor.add(a, b);
    }
    io.print(tensor.sum(c));
    return 0;
}
"""

SRC_DOT_INTERP = """
import io;
import tensor;
int main() {
    double[] a = tensor.arange(0.0, @N@.0, 1.0);
    double[] b = tensor.full(@N@, 1.0);
    double acc = 0.0;
    for (int r = 0; r < @ROUNDS@; r = r + 1) {
        double s = 0.0;
        for (int i = 0; i < @N@; i = i + 1) {
            s = s + a[i] * b[i];
        }
        acc = acc + s;
    }
    io.print(acc);
    return 0;
}
"""

SRC_DOT_KERNEL = """
import io;
import tensor;
int main() {
    double[] a = tensor.arange(0.0, @N@.0, 1.0);
    double[] b = tensor.full(@N@, 1.0);
    double acc = 0.0;
    for (int r = 0; r < @ROUNDS@; r = r + 1) {
        acc = acc + tensor.dot(a, b);
    }
    io.print(acc);
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
    output = None
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
            "interp", fill(SRC_INTERP, {"N": ELEMENTWISE_N, "ROUNDS": ROUNDS_ELEMENTWISE}),
            ncc, nvm, work)
        t_kernel, out_k = run_variant(
            "kernel", fill(SRC_KERNEL, {"N": ELEMENTWISE_N, "ROUNDS": ROUNDS_ELEMENTWISE}),
            ncc, nvm, work)
        t_dot_i, out_di = run_variant(
            "dot_interp", fill(SRC_DOT_INTERP, {"N": DOT_N, "ROUNDS": ROUNDS_DOT}),
            ncc, nvm, work)
        t_dot_k, out_dk = run_variant(
            "dot_kernel", fill(SRC_DOT_KERNEL, {"N": DOT_N, "ROUNDS": ROUNDS_DOT}),
            ncc, nvm, work)

    elem_note = ("match" if out_i == out_k
                 else "MISMATCH: " + out_i + " vs " + out_k)
    dot_note = ("match" if out_di == out_dk
                else "MISMATCH: " + out_di + " vs " + out_dk)
    print(f"elementwise add  N={ELEMENTWISE_N} x{ROUNDS_ELEMENTWISE}: "
          f"interp {t_interp*1000:.1f} ms, kernel {t_kernel*1000:.1f} ms "
          f"-> speedup {t_interp/t_kernel:.1f}x   (checksums {elem_note})")
    print(f"dot             N={DOT_N} x{ROUNDS_DOT}: "
          f"interp {t_dot_i*1000:.1f} ms, kernel {t_dot_k*1000:.1f} ms "
          f"-> speedup {t_dot_i/t_dot_k:.1f}x   (checksums {dot_note})")
    print("stage-1 checkpoint thresholds: elementwise >= 30x, dot >= 20x")


if __name__ == "__main__":
    main()
