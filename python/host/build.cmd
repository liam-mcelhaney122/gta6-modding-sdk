@echo off
rem Build g6_python.dll (the in-game Python host plugin) and its offline test with clang-cl (x64, C++20, /W4 /WX).
rem Usage: build.cmd [test]      "test" also runs g6_python_test.exe (every scenario in its own process)
rem Outputs (next to the runtime's own build, sdk\runtime\loader\build\):
rem   python\g6_python.dll, python\g6_python.ini     the plugin (copy both into <runtime folder>\plugins\)
rem   python_test\                                   g6_python_test.exe, gta6mod_mock.dll, scenario folders under work\
rem Needs the CPython 3.12 headers and libs\python312.lib: %G6_PYTHON_HOME%, else
rem %LOCALAPPDATA%\Programs\Python\Python312. Without them it prints SKIPPED and exits 0.
rem The test runs the plugin against that same Python (its python312.dll, Lib and DLLs).
setlocal
set "HERE=%~dp0"
set "SDK=%HERE%..\.."
set "LLVM=C:\Program Files\LLVM\bin"
if not exist "%LLVM%\clang-cl.exe" (
  where clang-cl >nul 2>nul || (echo clang-cl not found & exit /b 1)
  set "CXX=clang-cl"
) else (
  set "CXX=%LLVM%\clang-cl.exe"
)
if not defined G6_PYTHON_HOME set "G6_PYTHON_HOME=%LOCALAPPDATA%\Programs\Python\Python312"
if not exist "%G6_PYTHON_HOME%\include\Python.h" goto :nopython
if not exist "%G6_PYTHON_HOME%\libs\python312.lib" goto :nopython
if not exist "%G6_PYTHON_HOME%\python312.dll" goto :nopython

set "OUT=%SDK%\runtime\loader\build"
set "POUT=%OUT%\python"
set "TOUT=%OUT%\python_test"
for %%D in ("%OUT%" "%OUT%\obj" "%OUT%\obj\python" "%POUT%" "%TOUT%") do if not exist "%%~D" mkdir "%%~D"

rem Python's headers are system headers (/imsvc): their warnings are not ours. python312.dll is delay-loaded, so
rem g6_python.dll loads (and does nothing) on a machine without Python, and LoadLibraryExW picks [python] home.
set FLAGS=/nologo /std:c++20 /O2 /W4 /WX /EHsc /MT /GS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /DNDEBUG
set OBJ=/Fo"%OUT%\obj\python\\" /Fd"%OUT%\obj\python\\"

"%CXX%" %FLAGS% /I"%SDK%\include" /imsvc "%G6_PYTHON_HOME%\include" /Zi /LD "%HERE%g6_python.cpp" %OBJ% /Fe"%POUT%\g6_python.dll" -fuse-ld=lld /link /DEBUG /OPT:REF /OPT:ICF /LIBPATH:"%G6_PYTHON_HOME%\libs" python312.lib delayimp.lib /DELAYLOAD:python312.dll kernel32.lib
if errorlevel 1 (echo BUILD FAILED: g6_python.dll & exit /b 1)
if not exist "%POUT%\g6_python.ini" copy /y "%HERE%g6_python.ini" "%POUT%\g6_python.ini" >nul

"%CXX%" /nologo /O2 /W4 /WX /std:c11 /MT /I"%SDK%\include" /LD "%SDK%\runtime\src\mock_backend.c" %OBJ% /Fe"%TOUT%\gta6mod_mock.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: python_test\gta6mod_mock.dll & exit /b 1)
"%CXX%" %FLAGS% /I"%SDK%\include" /I"%SDK%\runtime\src" "%HERE%tests\g6_python_test.cpp" "%SDK%\runtime\src\mock_world.cpp" "%SDK%\runtime\src\ui_retained.cpp" "%TOUT%\gta6mod_mock.lib" %OBJ% /Fe"%TOUT%\g6_python_test.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: g6_python_test.exe & exit /b 1)

echo built: %POUT%\g6_python.dll (Python 3.12 from %G6_PYTHON_HOME%)
rem Top-level test steps: an "exit /b 1" from a ( ) block with more commands after it exits 0 under cmd /c.
if /i not "%~1"=="test" exit /b 0
"%TOUT%\g6_python_test.exe" "%POUT%\g6_python.dll" "%SDK%" "%G6_PYTHON_HOME%" "%TOUT%\work"
if errorlevel 1 (echo TEST FAILED: g6_python_test.exe & exit /b 1)
exit /b 0

:nopython
echo SKIPPED: g6_python (no CPython 3.12 with include\Python.h, libs\python312.lib and python312.dll at "%G6_PYTHON_HOME%"; set G6_PYTHON_HOME)
exit /b 0
