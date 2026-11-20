<#
.SYNOPSIS
    Reports the wait granularity this process gets, before and while it holds a
    1 ms timer-resolution request, and keeps the request alive for a while.

.DESCRIPTION
    docs/dev/http_perf_baseline.md §5 leaves one measurement open: whether the flat
    ~15 ms per request belongs to the server's own work or to a timed wait being
    rounded up to the system clock tick. Windows idles that tick near 15.6 ms unless
    a process raises its resolution with timeBeginPeriod, so only a process that
    raises the request and then measures its own waits can answer the question.

    The request is per-process. On this box (Windows 11 26300, measured 2026-11-10)
    a process holding timeBeginPeriod(1) sees its own Thread.Sleep(1) come back in
    about 2 ms, while a second process that never asked still waits about 15.6 ms.
    GetSystemTimeAdjustment's increment reads 156250 (100 ns units, i.e. 15.625 ms)
    in both cases, so that field is not the instrument here: it reports how often the
    system clock is updated, not the resolution a process asked for.
    GetGlobalTimerResolution, the newer winmm reader, is not exported by this box's
    winmm.dll at all (calling it raises EntryPointNotFoundException).

    Two consequences for the baseline document. A holder started outside the measured
    server never changes the server's waits, so the earlier out-of-process comparison
    did not move the independent variable at all. Experiment 1 in §5 therefore has to
    call timeBeginPeriod(1) inside nvm itself; this script shows what such a call does
    to one process's waits, and doubles as a way to check that a hold is in effect.

    Thread.Sleep(1) is the measuring stick, not PowerShell's Start-Sleep: Start-Sleep
    stays near 15.6 ms even in a process that holds the request, which hides the
    effect entirely.

.EXAMPLE
    powershell -File tools/http_perf/probe_clock_hold.ps1 -Seconds 120
#>
param([int]$Seconds = 20, [int]$Rounds = 200)
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public class ClockProbe {
    [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint ms);
    [DllImport("kernel32.dll")] public static extern bool GetSystemTimeAdjustment(
        out uint adjustment, out uint increment, out bool disabled);
    public static long Increment100ns() {
        uint a, i; bool d;
        return GetSystemTimeAdjustment(out a, out i, out d) ? (long)i : -1;
    }
    // Smallest wait this process actually gets. Compiled rather than scripted, so
    // no interpreter's own sleep policy can colour the distribution.
    public static string Granularity(int rounds) {
        var deltas = new double[rounds];
        var sw = Stopwatch.StartNew();
        double last = 0;
        for (int i = 0; i < rounds; i++) {
            System.Threading.Thread.Sleep(1);
            double now = sw.Elapsed.TotalMilliseconds;
            deltas[i] = now - last;
            last = now;
        }
        Array.Sort(deltas);
        return string.Format("pid={0} sleep(1ms) min={1:F3} median={2:F3} max={3:F3}",
            System.Diagnostics.Process.GetCurrentProcess().Id,
            deltas[0], deltas[rounds / 2], deltas[rounds - 1]);
    }
}
'@
"granularity before:  $([ClockProbe]::Granularity($Rounds))"
"increment before:    $([ClockProbe]::Increment100ns()) 100 ns units (see .DESCRIPTION: not this process's resolution)"
"requesting 1 ms:     rc=$([ClockProbe]::timeBeginPeriod(1))"
"granularity holding: $([ClockProbe]::Granularity($Rounds))"
"increment holding:   $([ClockProbe]::Increment100ns()) 100 ns units"
"holding for $Seconds s; another process still sees the coarse tick, so a measurement run"
"alongside this one only changes if it asks for the resolution itself"
Start-Sleep -Seconds $Seconds
[void][ClockProbe]::timeEndPeriod(1)
"granularity after:   $([ClockProbe]::Granularity($Rounds))"
"released"
