#!/usr/bin/env bash
# debug_assert_smoke.sh — assertion-build smoke gate.
#
# Why this exists: a Debug-only assert violation once shipped silently
# because every routine gate ran Release builds (NDEBUG strips asserts,
# so 137 ctest + full e2e stayed green while every Debug compile
# aborted). This gate exercises the assertion-enabled tool chain:
#
#   1. build ncc/nvm in Debug (assertions on),
#   2. pack the whole standard library with Debug ncc — the stdlib
#      sources are the richest single compile in the tree (every
#      generic template mint, including the fixed-key self-references
#      that hosted the original bug),
#   3. compile and run a minimal program end to end,
#   4. fail on any NLANG_ASSERT stack dump in the captured stderr.
#
# The Debug stdlib pack writes into temp/ so the shared build-tree
# stdlib.npkg (which Release e2e gates consume) is never overwritten
# with a Debug-packed copy.
#
# Usage (from the repository root, via tools/gate.sh):
#   tools/gate.sh debug-assert-smoke temp/gate_debug_assert_smoke \
#       tools/debug_assert_smoke.sh
# Any non-zero exit code is a failure; the verdict comes from
# tools/gate.sh.

set -u

cd "$(dirname "$0")/.."

workdir=temp/debug_assert_smoke

fail() {
    echo "debug_assert_smoke: FAIL: $1" >&2
    exit 1
}

# Worktrees build under different directory names (master: build-ide,
# dev worktree: build-dev); NLANG_BUILD_DIR overrides, else the first
# existing candidate wins.
build_dir=${NLANG_BUILD_DIR:-}
if [ -z "$build_dir" ]; then
    for cand in build-ide build-dev; do
        [ -d "$cand" ] && { build_dir=$cand; break; }
    done
fi
[ -n "$build_dir" ] || fail "no build directory found (set NLANG_BUILD_DIR)"
ncc_exe=$build_dir/src/tools/ncc/Debug/ncc.exe
nvm_exe=$build_dir/src/tools/nvm/Debug/nvm.exe

echo "[1/5] building Debug ncc/nvm (assertions enabled)"
cmake --build "$build_dir" --config Debug --target ncc nvm \
    >"$workdir.build.log" 2>&1 \
    || { tail -n 15 "$workdir.build.log"; fail "Debug build failed"; }

[ -f "$ncc_exe" ] || fail "Debug ncc not found at $ncc_exe"
[ -f "$nvm_exe" ] || fail "Debug nvm not found at $nvm_exe"

mkdir -p "$workdir"

echo "[2/5] packing the standard library with Debug ncc"
"$ncc_exe" build -p "$build_dir/stdlib/stdlib.nproj" \
    -o "$workdir/stdlib.npkg" \
    >"$workdir/pack.out" 2>"$workdir/pack.err" \
    || { tail -n 15 "$workdir/pack.err"; fail "Debug stdlib pack failed"; }
grep -q "stack dump:" "$workdir/pack.err" \
    && fail "stack dump during Debug stdlib pack"

echo "[3/5] compiling a minimal program with Debug ncc"
cat >"$workdir/minimal.n" <<'EOF'
int main() { return 7; }
EOF
"$ncc_exe" build "$workdir/minimal.n" -o "$workdir/minimal.ncu" \
    >"$workdir/compile.out" 2>"$workdir/compile.err" \
    || { tail -n 15 "$workdir/compile.err"; fail "Debug compile failed"; }
grep -q "stack dump:" "$workdir/compile.err" \
    && fail "stack dump during Debug compile"

echo "[4/5] running it on Debug nvm"
"$nvm_exe" "$workdir/minimal.ncu" \
    >"$workdir/run.out" 2>"$workdir/run.err"
run_rc=$?
[ "$run_rc" -eq 7 ] || fail "Debug nvm exited $run_rc, expected 7"
grep -q "stack dump:" "$workdir/run.err" \
    && fail "stack dump during Debug run"

echo "[5/5] all steps clean"
exit 0
