# PowerShell front end for build.cmd (same flags, same outputs in .\build).
#   .uild.ps1            build gta6mod_runtime.dll + loader_selftest.exe + g6inject.exe + g6probe.dll + g6ui.dll
#                          + g6trainer.dll + g6_python.dll (and the test binaries)
#   .uild.ps1 -Test      build and run the offline self-test, the injector test, the g6probe tests, the UI tests and
#                          g6_python_test (g6_python needs CPython 3.12: $env:G6_PYTHON_HOME, else
#                          %LOCALAPPDATA%\Programs\Python\Python312; without it the plugin and its test are SKIPPED
#                          and the output says so)
#   .\build.ps1 -Test -Dump "C:\path\to\GTA6.Xbox.v1.0.2.1"   also check the exe file (read-only)
#   .\build.ps1 -Low ...   run this script and every compiler and test it starts at idle priority (= start /low)
#
# Exit code: 0 only if every build step and every test passed; otherwise the failing step's exit code
# (1 if that code would read as 0 to a caller that keeps only the low byte, such as Git Bash).
# Use -Low instead of `cmd /c "start /low /wait /b powershell -File build.ps1 ..."`: cmd /c whose only
# command is `start` exits 0 whatever the started process returned, so a failure would be lost there.
param([switch]$Test, [string]$Dump = "", [switch]$Low)
# Native tools write diagnostics to stderr. With "Stop", PowerShell 5.1 turns the first stderr line into a
# terminating NativeCommandError whenever the caller redirects stderr, so failures are decided only by
# exit codes below.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Low) {
    # Child processes inherit the idle (and below-normal) priority class from their parent.
    try { (Get-Process -Id $PID).PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Idle }
    catch { Write-Host "build.ps1: could not lower the priority ($($_.Exception.Message)); continuing at normal priority" }
}

function Stop-OnFailure([string]$Step, $Code) {
    if ($null -eq $Code) { $Code = 1 }
    if ($Code -eq 0) { return }
    $out = if (($Code -band 0xFF) -eq 0) { 1 } else { $Code }
    Write-Host ("build.ps1: FAILED: {0} (exit code {1} = 0x{2:X8}); build.ps1 exits {3}" -f $Step, $Code, $Code, $out)
    exit $out
}

function Invoke-Step([string]$Step, [string]$Exe, [string[]]$Arguments) {
    if (-not (Test-Path -LiteralPath $Exe)) { Stop-OnFailure "$Step ($Exe not found)" 1 }
    $global:LASTEXITCODE = $null
    & $Exe @Arguments
    Stop-OnFailure $Step $LASTEXITCODE
}

$global:LASTEXITCODE = $null
& cmd.exe /c "`"$here\build.cmd`""
Stop-OnFailure "build.cmd" $LASTEXITCODE

if ($Test) {
    if ($Dump) { $env:G6_DUMP = $Dump }
    $b = "$here\build"
    Invoke-Step "loader_selftest" "$b\loader_selftest.exe" @("$b\gta6mod_runtime.dll")
    Invoke-Step "inject_test" "$b\inject_test\inject_test.exe" @("$b\g6inject.exe")
    Invoke-Step "g6probe_test" "$b\probe_test\g6probe_test.exe" @()
    Invoke-Step "probe_mock_test" "$b\probe_test\probe_mock_test.exe" @("$b\probe_test\gta6mod_mock.dll", "$b\probe\g6probe.dll",
        "$b\probe_test\fake_title.dll", "$b\probe_test\work")
    Invoke-Step "ui_test (default adapter)" "$b\ui_test\ui_test.exe" @("$b\g6ui.dll", "$b\ui_test\ui_test_plugin.dll")
    Invoke-Step "ui_test (WARP)" "$b\ui_test\ui_test.exe" @("$b\g6ui.dll", "$b\ui_test\ui_test_plugin.dll", "--warp")
    Invoke-Step "trainer_mock_test" "$b\ui_test\trainer_mock_test.exe" @("$b\ui_test\gta6mod_mock.dll", "$b\trainer\g6trainer.dll")
    $pyHome = if ($env:G6_PYTHON_HOME) { $env:G6_PYTHON_HOME } else { "$env:LOCALAPPDATA\Programs\Python\Python312" }
    if (Test-Path -LiteralPath "$b\python_test\g6_python_test.exe") {
        Invoke-Step "g6_python_test" "$b\python_test\g6_python_test.exe" @("$b\python\g6_python.dll",
            (Resolve-Path -LiteralPath "$here\..\..").Path, $pyHome, "$b\python_test\work")
    } else {
        Write-Host "build.ps1: SKIPPED: g6_python_test (g6_python was not built: no CPython 3.12 at $pyHome)"
    }
    Write-Host "build.ps1: build and all tests passed"
}
exit 0
