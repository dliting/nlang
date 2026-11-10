#!/usr/bin/env bash
# Build check for the net package's POSIX path. Compiles the module's Linux
# translation-unit list (mirroring the target_sources list in
# src/native/CMakeLists.txt) with -Wall -Wextra, then cross-checks the objects'
# symbols, so net_platform.h cannot drift from its POSIX implementation
# unnoticed the way it did until v0.13.6 (five signatures/definitions out of
# sync, plus a Windows-only header reaching a portable translation unit).
#
# Run it on Linux, or from Windows through WSL (the host has no Linux CI):
#   wsl.exe -d Ubuntu-22.04 -- bash <repo>/tools/posix_net_build_check.sh
#
# Compiling and linking clean is not the same as running correct: the net e2e
# suite still has to run on Linux for that (docs/roadmap.md, v0.13.6 item 6).
set -u
cd "$(dirname "$0")/.." || exit 9
out=temp/posix_net_check
mkdir -p "$out"
: >"$out/rc.txt"
files="src/native/net/net_native.cpp src/native/net/net_jobs.cpp src/native/net/net_options.cpp src/native/net/net_slots.cpp src/native/net/net_state.cpp src/native/net/net_tcp.cpp src/native/net/net_tcp_connect.cpp src/native/net/net_udp.cpp src/native/net/net_icmp.cpp src/native/net/net_platform_posix.cpp src/native/net/net_platform_icmp_posix.cpp src/runtime/StackDump.cpp"
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
fail=0
for f in $files; do
    name=$(basename "$f")
    g++ -std=c++17 -fPIC -Wall -Wextra -Iinclude -Isrc/native/net \
        -c "$f" -o "$obj/$name.o" >"$out/$name.log" 2>&1
    rc=$?
    printf '%-46s rc=%d\n' "$f" "$rc" >>"$out/rc.txt"
    [ "$rc" -eq 0 ] || fail=1
done
# Symbol cross-check. A full link is not possible here (the VM side is not
# built, and a shared library tolerates undefined symbols anyway), so the
# objects' own symbol tables are compared: an nlang::Net* symbol some
# object calls must be defined by some object. The VM's own symbols
# (NativeHost, IoPoller, ...) stay undefined by design and are filtered
# out. The comparison is signature-complete (name AND parameter list):
# a definition beside a changed parameter list is a DIFFERENT function,
# and the earlier name-only comparison minted exactly the drift class it
# was built to catch — a one-parameter NetConnectResult definition
# satisfied two-parameter calls while dlopen(RTLD_NOW) still failed the
# real symbol at module load (external review C1). The [abi:cxx11] tag
# nm -C prints for every std::string-returning function is stripped
# first, or those entries slip through the filter unnoticed.
tags='s/\[abi:[a-z0-9_]*\]//g'
sig='s/.*(nlang::Net[A-Za-z0-9_]*\(.*\)).*/\1/p'
nm -C --undefined-only "$obj"/*.o 2>/dev/null \
    | sed -nE "$tags; $sig" | sort -u > "$out/undef.txt"
nm -C --defined-only "$obj"/*.o 2>/dev/null \
    | sed -nE "$tags; $sig" | sort -u > "$out/defs.txt"
comm -23 "$out/undef.txt" "$out/defs.txt" > "$out/missing.txt"
if [ -s "$out/missing.txt" ]; then
    fail=1
    printf 'missing definitions: %s\n' "$(tr '\n' ' ' < "$out/missing.txt")" \
        >> "$out/rc.txt"
fi
# Tripwire for the capture itself: a sed whose pattern silently matches
# nothing would make the cross-check blind again — the exact failure
# mode the old name-only comm had (and the -E here is not decoration:
# in a BRE the same pattern captures parameter-list fragments instead
# of whole signatures, which is how the first rewrite of this check
# still passed while NetConnectResult(int, int*) was undefined). Every
# nlang::Net* name visible in the raw undefined entries must reappear
# in some captured signature; a missing one is a capture blind spot.
nm -C --undefined-only "$obj"/*.o 2>/dev/null | sed "$tags" \
    | grep -oE 'nlang::Net[A-Za-z0-9_]+\(' | sed 's/(//' | sort -u \
    > "$out/rawnames.txt"
cut -d'(' -f1 "$out/undef.txt" | sort -u > "$out/capnames.txt"
if ! cmp -s "$out/rawnames.txt" "$out/capnames.txt"; then
    fail=1
    printf 'capture blind spot (raw undefined name vs captures):\n' \
        >> "$out/rc.txt"
    comm -3 "$out/rawnames.txt" "$out/capnames.txt" >> "$out/rc.txt"
fi
if [ "$fail" -eq 0 ]; then
    echo "GATE posix_net_build: PASS"
else
    echo "GATE posix_net_build: FAIL (see $out/rc.txt and $out/*.log)"
fi
exit "$fail"
