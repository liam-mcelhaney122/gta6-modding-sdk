@echo off
rem Build gta6mod_runtime.dll and the offline self-test with clang-cl (x64, C++20, /W4 /WX).
rem Usage: build.cmd [test]      "test" also runs build\loader_selftest.exe
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
if /i "%~1"=="test" (
  "%OUT%\loader_selftest.exe" "%OUT%\gta6mod_runtime.dll"
  exit /b %errorlevel%
)
exit /b 0
