@echo off
rem Build the g6probe plugin (x64, C++20, /W4 /WX) and its offline tests with clang-cl.
rem Usage: build_probe.cmd [test]      "test" also runs the unit test and the mock-world test
rem Outputs:
rem   build\probe\g6probe.dll, build\probe\g6probe.ini   the plugin (copy both into <runtime folder>\plugins\)
rem   build\probe_test\                                  g6probe_test.exe, probe_mock_test.exe, gta6mod_mock.dll,
rem                                                      fake_title.dll; scenario folders under work\
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
set "POUT=%OUT%\probe"
set "TOUT=%OUT%\probe_test"
for %%D in ("%OUT%" "%OUT%\obj" "%OUT%\obj\probe" "%POUT%" "%TOUT%") do if not exist "%%~D" mkdir "%%~D"

set FLAGS=/nologo /std:c++20 /O2 /W4 /WX /EHsc /MT /GS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE
set INCS=/I"%HERE%..\..\include" /I"%HERE%plugins\g6probe"
set OBJ=/Fo"%OUT%\obj\probe\\" /Fd"%OUT%\obj\probe\\"

"%CXX%" %FLAGS% %INCS% /Zi /LD "%HERE%plugins\g6probe\g6probe.cpp" %OBJ% /Fe"%POUT%\g6probe.dll" -fuse-ld=lld /link /DEBUG /OPT:REF /OPT:ICF kernel32.lib user32.lib
if errorlevel 1 (echo BUILD FAILED: g6probe.dll & exit /b 1)
if not exist "%POUT%\g6probe.ini" copy /y "%HERE%plugins\g6probe\g6probe.ini" "%POUT%\g6probe.ini" >nul

"%CXX%" %FLAGS% %INCS% /I"%HERE%src" "%HERE%tests\probe\g6probe_test.cpp" %OBJ% /Fe"%TOUT%\g6probe_test.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: g6probe_test.exe & exit /b 1)
"%CXX%" /nologo /O2 /W4 /WX /std:c11 /MT /I"%HERE%..\..\include" /LD "%HERE%..\src\mock_backend.c" %OBJ% /Fe"%TOUT%\gta6mod_mock.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: gta6mod_mock.dll & exit /b 1)
"%CXX%" %FLAGS% /LD "%HERE%tests\probe\fake_title.cpp" %OBJ% /Fe"%TOUT%\fake_title.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: fake_title.dll & exit /b 1)
"%CXX%" %FLAGS% /I"%HERE%..\..\include" "%HERE%tests\probe\probe_mock_test.cpp" %OBJ% /Fe"%TOUT%\probe_mock_test.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: probe_mock_test.exe & exit /b 1)

echo built: %POUT%\g6probe.dll
rem Top-level test steps: an "exit /b 1" from a ( ) block with more commands after it exits 0 under cmd /c.
if /i not "%~1"=="test" exit /b 0
"%TOUT%\g6probe_test.exe"
if errorlevel 1 (echo TEST FAILED: g6probe_test.exe & exit /b 1)
"%TOUT%\probe_mock_test.exe" "%TOUT%\gta6mod_mock.dll" "%POUT%\g6probe.dll" "%TOUT%\fake_title.dll" "%TOUT%\work"
if errorlevel 1 (echo TEST FAILED: probe_mock_test.exe & exit /b 1)
exit /b 0
