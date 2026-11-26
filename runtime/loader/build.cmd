@echo off
rem Build gta6mod_runtime.dll, the offline self-test, g6inject.exe (build_inject.cmd) and the g6probe
rem plugin (build_probe.cmd) with clang-cl (x64, C++20, /W4 /WX).
rem Usage: build.cmd [test]      "test" also runs build\loader_selftest.exe, build\inject_test\inject_test.exe
rem                              and the g6probe unit and mock-world tests
setlocal
set "HERE=%~dp0"
set "LLVM=C:\Program Files\LLVM\bin"
if not exist "%LLVM%\clang-cl.exe" (
  where clang-cl >nul 2>nul || (echo clang-cl not found & exit /b 1)
  set "CXX=clang-cl"
) else (
  set "CXX=%LLVM%\clang-cl.exe"
)
set "OUT=%HERE%build"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

set FLAGS=/nologo /std:c++20 /O2 /W4 /WX /EHsc /MT /GS /Zi /DG6_BUILDING_RUNTIME=1 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE
set INCS=/I"%HERE%..\..\include" /I"%HERE%src"

"%CXX%" %FLAGS% %INCS% /LD "%HERE%src\runtime.cpp" "%HERE%src\hook.cpp" "%HERE%src\x64_decode.cpp" "%HERE%src\pattern_scan.cpp" "%HERE%src\identity.cpp" "%HERE%src\natives.cpp" "%HERE%src\scheduler.cpp" "%HERE%src\log.cpp" "%HERE%src\game_clock.cpp" "%HERE%src\vfs.cpp" "%HERE%src\guard.cpp" /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" /Fe"%OUT%\gta6mod_runtime.dll" -fuse-ld=lld /link /DEBUG /OPT:REF /OPT:ICF bcrypt.lib iphlpapi.lib kernel32.lib
if errorlevel 1 (echo BUILD FAILED: gta6mod_runtime.dll & exit /b 1)

"%CXX%" /nologo /std:c++20 /O1 /W4 /WX /EHsc /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX %INCS% "%HERE%tests\loader_selftest.cpp" /Fo"%OUT%\obj\\" /Fe"%OUT%\loader_selftest.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: loader_selftest.exe & exit /b 1)

if not exist "%OUT%\gta6mod_runtime.ini" copy /y "%HERE%gta6mod_runtime.ini" "%OUT%\gta6mod_runtime.ini" >nul
echo built: %OUT%\gta6mod_runtime.dll
call "%HERE%build_inject.cmd"
if errorlevel 1 exit /b 1
call "%HERE%build_probe.cmd"
if errorlevel 1 exit /b 1
rem "if errorlevel" is evaluated when the line runs; %errorlevel% inside ( ) would be expanded too early.
if /i "%~1"=="test" (
  "%OUT%\loader_selftest.exe" "%OUT%\gta6mod_runtime.dll"
  if errorlevel 1 exit /b 1
  "%OUT%\inject_test\inject_test.exe" "%OUT%\g6inject.exe"
  if errorlevel 1 exit /b 1
  "%OUT%\probe_test\g6probe_test.exe"
  if errorlevel 1 exit /b 1
  "%OUT%\probe_test\probe_mock_test.exe" "%OUT%\probe_test\gta6mod_mock.dll" "%OUT%\probe\g6probe.dll" "%OUT%\probe_test\fake_title.dll" "%OUT%\probe_test\work"
  if errorlevel 1 exit /b 1
)
exit /b 0
