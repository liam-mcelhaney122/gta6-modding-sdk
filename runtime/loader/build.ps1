# PowerShell front end for build.cmd (same flags, same outputs in .\build).
#   .\build.ps1            build gta6mod_runtime.dll + loader_selftest.exe + g6inject.exe (and its test binaries)
#   .\build.ps1 -Test      build and run the offline self-test, then the injector test
#   .\build.ps1 -Test -Dump "C:\path\to\GTA6.Xbox.v1.0.2.1"   also check the exe file (read-only)
param([switch]$Test, [string]$Dump = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
& cmd.exe /c "`"$here\build.cmd`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Test) {
    if ($Dump) { $env:G6_DUMP = $Dump }
    & "$here\build\loader_selftest.exe" "$here\build\gta6mod_runtime.dll"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$here\build\inject_test\inject_test.exe" "$here\build\g6inject.exe"
    exit $LASTEXITCODE
}
