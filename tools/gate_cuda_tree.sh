#!/usr/bin/env bash
# gate_cuda_tree.sh — the conditional cuda tree of the three-tree gate
# (spec D7, plan T4.3).
#
# The device double-run leg runs only when a working CUDA device
# backend is present on this machine; otherwise it reports SKIP and
# the other legs' verdicts stand. That keeps the gate honest on
# machines without a GPU while pinning the double-run where the
# device is.
#
# Usage: tools/gate_cuda_tree.sh <build-tree> [logfile-stem]
#   <build-tree>     the Release tree configured with NLANG_CUDA_ROOT
#                    (build-bench on the reference machine)
#   [logfile-stem]  capture prefix (default: temp/gate_cuda_tree)
#
# 1. Probe: compile and run a tiny program under
#    NLANG_TENSOR_BACKEND=cuda. The explicit-cuda fail-fast (the
#    process exits with code 3 before the program starts) means the
#    plugin or the GPU is absent — the leg SKIPs with the reason.
# 2. On presence: run the e2e double-run over the explicit list
#    tools/e2e_cuda_double_run.txt with NLANG_E2E_TENSOR_BACKEND=cuda
#    (tensor subset full + full-set sample, the D7 two-layer
#    strategy; the list is validated against manifest.txt by the
#    runner, so a stale name fails the leg instead of silently
#    shrinking the net).
#
# Exit: 0 = PASS or SKIP, non-zero = FAIL. Read the verdict line, not
# the tails.

set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
tree="${1:?usage: gate_cuda_tree.sh <build-tree> [logfile-stem]}"
stem="${2:-temp/gate_cuda_tree}"

cd "$root"
tree_abs="$(cd "$tree" 2>/dev/null && pwd)" || {
    echo "GATE cuda_tree: FAIL (build tree not found: $tree)"
    exit 1
}
ncc="$tree_abs/src/tools/ncc/Release/ncc.exe"
nvm="$tree_abs/src/tools/nvm/Release/nvm.exe"
ndb="$tree_abs/src/tools/ndb/Release/ndb.exe"
for tool in "$ncc" "$nvm" "$ndb"; do
    if [ ! -f "$tool" ]; then
        echo "GATE cuda_tree: FAIL (missing $tool)"
        exit 1
    fi
done

# --- 1. device presence probe -------------------------------------
mkdir -p temp
probe_src="$root/temp/_gate_cuda_probe.n"
cat > "$probe_src" << 'EOF'
import io;
import tensor;

int main() {
    float[] a = float[4];
    if (tensor.backend() != "cuda") return 1;
    if (tensor.sum(a) != 0) return 1;
    return 0;
}
EOF
probe_ncu="$root/temp/_gate_cuda_probe.ncu"
rm -f "$probe_ncu"
( cd temp && "$ncc" build _gate_cuda_probe.n >/dev/null 2>&1 )
if [ ! -f "$probe_ncu" ]; then
    echo "GATE cuda_tree: FAIL (probe did not compile)"
    exit 1
fi
probe_out="$( cd temp && NLANG_TENSOR_BACKEND=cuda "$nvm" _gate_cuda_probe.ncu 2>&1 )"
probe_rc=$?
if [ "$probe_rc" -ne 0 ]; then
    echo "GATE cuda_tree: SKIP (no working cuda device backend: probe rc=$probe_rc: $probe_out)"
    exit 0
fi

# --- 2. the double-run ---------------------------------------------
NLANG_E2E_TENSOR_BACKEND=cuda conda run -n py313 \
    python tests/e2e/run_e2e_tests.py "$ncc" "$nvm" "$ndb" \
    tools/e2e_cuda_double_run.txt >"$stem.out" 2>"$stem.err"
rc=$?

echo "----- cuda_tree: stdout tail ($stem.out) -----"
tail -n 15 "$stem.out"
echo "----- cuda_tree: stderr tail ($stem.err) -----"
tail -n 15 "$stem.err"

if [ "$rc" -eq 0 ]; then
    echo "GATE cuda_tree: PASS"
else
    echo "GATE cuda_tree: FAIL rc=$rc"
fi
exit "$rc"
