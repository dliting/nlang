<#
.SYNOPSIS
    Builds the three pure-C++ control-group binaries of the experiments in
    docs/dev/net_http_perf_design.md §3.3 (E0's two registration shapes and
    E1's timer-resolution check).

.DESCRIPTION
    E0 asks whether the flat ~15 ms per request can be produced by our own
    native code with the virtual machine taken out altogether. The answer has
    to come from the SAME files the server uses — the IO poller facade, the
    vendored wepoll, and the socket layer — so this script compiles four
    translation units and nothing else:

        tools/http_perf/control_server.cpp          the driver, three shapes
        src/vm/IoPollerWindows.cpp                  epoll-compatible facade
        src/3rdparty/wepoll/wepoll.c                IOCP-backed poller
        src/native/net/net_platform_windows.cpp     Winsock socket layer

    No VM library, no global interpreter lock, no carriers, no script. The
    same file combination already compiles without the VM in
    tests/CMakeLists.txt's test_net_platform target, which is what makes this
    command line trustworthy rather than a guess at the build system.

    One binary per shape, so the hot path carries no branch on the mode:
    control_round.exe re-syncs the interest set at the poller's round boundary
    (today's SyncPollerInterest), control_immediate.exe registers at the park
    site (ruling D1's shape), and control_round-tb.exe is today's shape plus
    one timeBeginPeriod(1) at startup — E1's mechanism check taken in the
    cheapest place first, since a 1 ms resolution request should pull the
    quantum from 15.6 ms down to the 10 ms slice if the clock-tick rounding is
    really what sets it. Only control_server.cpp differs between them, through
    CONTROL_REGISTER_IMMEDIATE and CONTROL_TIMEBEGIN.

    /O2 /MT with NDEBUG: the baseline rows these numbers get compared against
    were measured against a Release nvm, and a statically linked CRT means the
    probe runs without depending on a redistributable being on PATH.

    Objects go under the output directory (temp/, per the project's layout
    rules); so do the two exes, which is where probe_control_ab.sh expects
    them.

.EXAMPLE
    powershell -NoProfile -File tools/http_perf/build_control.ps1

.EXAMPLE
    powershell -NoProfile -File tools/http_perf/build_control.ps1 -OutDir D:\tmp\e0
#>
param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # .../tools/http_perf -> repo root
if (-not $OutDir) { $OutDir = Join-Path $root 'temp\http_perf\control' }

#Locate VS 2022 (the toolchain the project builds with) through vswhere.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    $vswhere = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
}
if (-not (Test-Path $vswhere)) { Write-Error "vswhere not found near '$vswhere'"; exit 1 }
$vsPath = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if (-not $vsPath) { Write-Error 'no VS 2022 installation with the x64 tools'; exit 1 }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Write-Error "missing '$vcvars'"; exit 1 }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$defines = @(
    "-I$(Join-Path $root 'src\vm')",
    "-I$(Join-Path $root 'src\native')",
    "-I$(Join-Path $root 'src\3rdparty\wepoll')"
)
$sources = @(
    (Join-Path $root 'tools\http_perf\control_server.cpp'),
    (Join-Path $root 'src\vm\IoPollerWindows.cpp'),
    (Join-Path $root 'src\native\net\net_platform_windows.cpp'),
    (Join-Path $root 'src\3rdparty\wepoll\wepoll.c')
)
$libs = @('ws2_32.lib', 'iphlpapi.lib', 'winmm.lib')

#cl is driven through a generated .bat because vcvars64.bat sets the
#environment only inside its own cmd session. Objects land in the current
#directory, hence the cd per shape — which also keeps the shapes' objects
#apart without fighting cl's trailing-backslash quoting in /Fo.
#Shape → its compile definition. The table is also the list that names the
#binaries, which is what probe_control_ab.sh walks, so one place holds the
#shape names and control_server.cpp carries no runtime branch on them.
$shapes = [ordered]@{
    'round'     = @()                                     #today's registration shape
    'immediate' = @('/DCONTROL_REGISTER_IMMEDIATE')      #ruling D1's shape
    'round-tb'  = @('/DCONTROL_TIMEBEGIN')               #E1's mechanism check
}

foreach ($mode in $shapes.Keys) {
    $objDir = Join-Path $OutDir "obj_$mode"
    $exe = Join-Path $OutDir "control_$mode.exe"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    if (Test-Path $exe) { Remove-Item $exe }
    $flags = @('/nologo', '/std:c++17', '/EHsc', '/O2', '/MT', '/W4', '/utf-8', '/DNDEBUG')
    $flags += $shapes[$mode]
    #Compile with /c, then link: objects are named after their sources and all
    #land in $objDir (the current directory), so the link line is a fixed list
    #and /Fe and /Fo quoting never come up.
    $clLine = (@('cl') + $flags + $defines + @('/c') + $sources) -join ' '
    $objs = @('control_server.obj', 'IoPollerWindows.obj', 'net_platform_windows.obj', 'wepoll.obj')
    $linkLine = (@('link /nologo') + @("/OUT:`"$exe`"") + $objs + $libs + @('|| exit /b 1')) -join ' '
    $bat = @(
        '@echo off',
        'setlocal',
        "call `"$vcvars`" >nul || exit /b 1",
        "cd /d `"$objDir`" || exit /b 1",
        $clLine,
        'if errorlevel 1 exit /b 1',
        $linkLine
    )
    $batPath = Join-Path $OutDir "build_$mode.bat"
    Set-Content -Path $batPath -Value $bat -Encoding ascii
    Write-Host "building control_$mode.exe"
    & cmd.exe /c "`"$batPath`""
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Write-Error "control_$mode build failed (rc=$LASTEXITCODE); command line in '$batPath'"
        exit 1
    }
    Write-Host "  ok: $exe"
}
'built:'
Get-ChildItem (Join-Path $OutDir 'control_*.exe') | ForEach-Object { '  ' + $_.FullName + '  ' + $_.Length + ' bytes' }
