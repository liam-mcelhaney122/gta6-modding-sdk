@echo off
rem Build gta6mod_runtime.dll, the offline self-test, g6inject.exe (build_inject.cmd), the g6probe
rem plugin (build_probe.cmd), the UI overlay g6ui.dll + g6trainer plugin (build_ui.cmd) and the g6_python plugin
rem (sdk\python\hostuild.cmd) with clang-cl (x64, C++20, /W4 /WX).
rem Usage: build.cmd [test]      "test" also runs build\loader_selftest.exe, build\inject_test\inject_test.exe
rem                              the g6probe unit and mock-world tests, the UI tests (ui_test.exe on the default
rem                              adapter and on WARP, trainer_mock_test.exe) and g6_python_test.exe (skipped without
rem                              CPython 3.12)
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
set INCS=/I"%HERE%..\..\include" /I"%HERE%src" /I"%HERE%ui" /I"%HERE%..\src"

"%CXX%" %FLAGS% %INCS% /LD "%HERE%src\runtime.cpp" "%HERE%src\hook.cpp" "%HERE%src\x64_decode.cpp" "%HERE%src\pattern_scan.cpp" "%HERE%src\identity.cpp" "%HERE%src\natives.cpp" "%HERE%src\script_objects.cpp" "%HERE%src\scheduler.cpp" "%HERE%src\log.cpp" "%HERE%src\game_clock.cpp" "%HERE%src\vfs.cpp" "%HERE%src\guard.cpp" "%HERE%src\binder.cpp" "%HERE%..\src\ui_retained.cpp" /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" /Fe"%OUT%\gta6mod_runtime.dll" -fuse-ld=lld /link /DEBUG /OPT:REF /OPT:ICF bcrypt.lib iphlpapi.lib kernel32.lib
if errorlevel 1 (echo BUILD FAILED: gta6mod_runtime.dll & exit /b 1)

"%CXX%" /nologo /std:c++20 /O1 /W4 /WX /EHsc /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX %INCS% "%HERE%tests\loader_selftest.cpp" /Fo"%OUT%\obj\\" /Fe"%OUT%\loader_selftest.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: loader_selftest.exe & exit /b 1)

if not exist "%OUT%\gta6mod_runtime.ini" copy /y "%HERE%gta6mod_runtime.ini" "%OUT%\gta6mod_runtime.ini" >nul
echo built: %OUT%\gta6mod_runtime.dll
call "%HERE%build_inject.cmd"
if errorlevel 1 exit /b 1
call "%HERE%build_probe.cmd"
if errorlevel 1 exit /b 1
call "%HERE%build_ui.cmd"
if errorlevel 1 exit /b 1
call "%HERE%..\..\python\host\build.cmd"
if errorlevel 1 exit /b 1
rem The test steps are top-level lines on purpose: run through cmd /c, a batch that leaves by "exit /b 1" from a
rem ( ) block with more commands after that line exits with code 0 although ERRORLEVEL is 1 (seen 2026-11-25).
if /i not "%~1"=="test" exit /b 0
"%OUT%\loader_selftest.exe" "%OUT%\gta6mod_runtime.dll"
if errorlevel 1 (echo TEST FAILED: loader_selftest.exe & exit /b 1)
"%OUT%\inject_test\inject_test.exe" "%OUT%\g6inject.exe"
if errorlevel 1 (echo TEST FAILED: inject_test.exe & exit /b 1)
"%OUT%\probe_test\g6probe_test.exe"
if errorlevel 1 (echo TEST FAILED: g6probe_test.exe & exit /b 1)
"%OUT%\probe_test\probe_mock_test.exe" "%OUT%\probe_test\gta6mod_mock.dll" "%OUT%\probe\g6probe.dll" "%OUT%\probe_test\fake_title.dll" "%OUT%\probe_test\work"
if errorlevel 1 (echo TEST FAILED: probe_mock_test.exe & exit /b 1)
"%OUT%\ui_test\ui_test.exe" "%OUT%\g6ui.dll" "%OUT%\ui_test\ui_test_plugin.dll"
if errorlevel 1 (echo TEST FAILED: ui_test.exe & exit /b 1)
"%OUT%\ui_test\ui_test.exe" "%OUT%\g6ui.dll" "%OUT%\ui_test\ui_test_plugin.dll" --warp
if errorlevel 1 (echo TEST FAILED: ui_test.exe --warp & exit /b 1)
"%OUT%\ui_test\trainer_mock_test.exe" "%OUT%\ui_test\gta6mod_mock.dll" "%OUT%\trainer\g6trainer.dll"
if errorlevel 1 (echo TEST FAILED: trainer_mock_test.exe & exit /b 1)
if not defined G6_PYTHON_HOME set "G6_PYTHON_HOME=%LOCALAPPDATA%\Programs\Python\Python312"
if not exist "%OUT%\python_test\g6_python_test.exe" goto :python_skipped
"%OUT%\python_test\g6_python_test.exe" "%OUT%\python\g6_python.dll" "%HERE%..\.." "%G6_PYTHON_HOME%" "%OUT%\python_test\work"
if errorlevel 1 (echo TEST FAILED: g6_python_test.exe & exit /b 1)
exit /b 0
:python_skipped
echo SKIPPED: g6_python_test.exe (g6_python was not built: no CPython 3.12, see sdk\python\host\build.cmd)
exit /b 0
