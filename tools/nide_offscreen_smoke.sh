#!/usr/bin/env bash
# nide_offscreen_smoke.sh -- the N2 headless smoke for the IDE's Linux
# port: launch nide with the offscreen QPA, give the main window a few
# seconds to come up, then terminate it. Verdict: the process must
# start, stay alive, and exit WITHOUT a crash signal -- a SIGTERM'd
# (143) or clean (0) exit passes; 139 (SIGSEGV), 134 (SIGABRT) or an
# early exit fail. QtWebEngine flags stay unset here on purpose: the
# smoke exists to discover whether the embedded help stack needs them
# on a headless host, not to assume it does.
#
#   tools/nide_offscreen_smoke.sh <path-to-nide-binary>
set -u
NIDE_BIN=${1:?usage: nide_offscreen_smoke.sh <nide-binary>}
[ -x "$NIDE_BIN" ] || { echo "not executable: $NIDE_BIN"; exit 2; }

export QT_QPA_PLATFORM=offscreen
"$NIDE_BIN" &
PID=$!
sleep 8
if ! kill -0 "$PID" 2>/dev/null; then
    wait "$PID"; RC=$?
    echo "FAIL: nide exited early rc=$RC"
    exit 1
fi
kill -TERM "$PID" 2>/dev/null
wait "$PID"; RC=$?
#128+n for signal death: TERM (143) is the managed shutdown; SEGV
#(139) and ABRT (134) are the crashes this smoke guards against.
if [ "$RC" -eq 143 ] || [ "$RC" -eq 0 ]; then
    echo "PASS: started and terminated cleanly (rc=$RC)"
    exit 0
fi
echo "FAIL: abnormal exit rc=$RC"
exit 1
