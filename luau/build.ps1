# PowerShell front end for build.cmd (same flags, same outputs in sdk\build\luau).
#   .\build.ps1            build g6_luau.dll, g6_luau_runner.exe, gta6mod_mock.dll, plugin_check.exe
#   .\build.ps1 -Test      build, then run the spawn_car example, sdk\luau\tests and plugin_check
#   .\build.ps1 -Rebuild   also recompile the Luau library (sdk\build\luau\luau-<tag>.lib)
param([switch]$Test, [switch]$Rebuild)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmdArgs = @()
if ($Rebuild) { $cmdArgs += "rebuild" }
if ($Test) { $cmdArgs += "test" }
# Merge stderr inside cmd: Windows PowerShell 5.1 turns native stderr lines (upstream Luau warnings)
# into terminating errors under ErrorActionPreference=Stop. The exit code stays authoritative.
& cmd.exe /c "`"$here\build.cmd`" $($cmdArgs -join ' ') 2>&1"
exit $LASTEXITCODE
