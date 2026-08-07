#!/bin/bash
NCC="build/src/tools/ncc/Release/ncc.exe"
NVM="build/src/tools/nvm/Release/nvm.exe"
TESTDIR="tests/e2e"
PASS=0
FAIL=0
CFail=0

for f in "$TESTDIR"/*.n; do
    name=$(basename "$f" .n)
    nmod="$TESTDIR/${name}.nmod"

    # Compile
    $NCC build "$f" -o "$nmod" >/dev/null 2>&1
    if [ ! -f "$nmod" ]; then
        echo "COMPILE_FAIL: $name"
        CFail=$((CFail+1))
        FAIL=$((FAIL+1))
        continue
    fi

    # Run
    $NVM "$nmod" >/dev/null 2>&1
    actual=$?
    rm -f "$nmod"

    # Check expected exit code from comment, or accept any non-crash result
    expected=""
    comment=$(grep -m1 '^// exit:' "$f" 2>/dev/null)
    if [ -n "$comment" ]; then
        expected=$(echo "$comment" | sed 's/.*exit: *//')
    fi

    if [ -n "$expected" ]; then
        if [ "$actual" -eq "$expected" ]; then
            echo "PASS: $name (exit=$actual)"
            PASS=$((PASS+1))
        else
            echo "FAIL: $name (exit=$actual, expected=$expected)"
            FAIL=$((FAIL+1))
        fi
    else
        # No expected value specified - just verify it compiled and ran
        echo "OK: $name (exit=$actual)"
        PASS=$((PASS+1))
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed ($CFail compile failures)"
