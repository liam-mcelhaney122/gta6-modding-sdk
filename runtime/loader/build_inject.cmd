@echo off
rem Build g6inject.exe (x64, C++20, /W4 /WX) and its offline test binaries with clang-cl.
rem Usage: build_inject.cmd [test]      "test" also runs build\inject_test\inject_test.exe
rem Outputs: build\g6inject.exe; build\inject_test\ (stand-in target, image DLL, marker DLLs, test driver).
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
set "TOUT=%OUT%\inject_test"
for %%D in ("%OUT%" "%OUT%\obj" "%OUT%\obj\inject" "%OUT%\obj\inject32" "%TOUT%") do if not exist "%%~D" mkdir "%%~D"

set FLAGS=/nologo /std:c++20 /O2 /W4 /WX /EHsc /MT /GS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE
set INC=/I"%HERE%tools\inject"
set OBJ=/Fo"%OUT%\obj\inject\\" /Fd"%OUT%\obj\inject\\"
set OBJ32=/Fo"%OUT%\obj\inject32\\" /Fd"%OUT%\obj\inject32\\"

"%CXX%" %FLAGS% %INC% /Zi "%HERE%tools\inject\g6inject.cpp" %OBJ% /Fe"%OUT%\g6inject.exe" -fuse-ld=lld /link /DEBUG kernel32.lib
if errorlevel 1 (echo BUILD FAILED: g6inject.exe & exit /b 1)

"%CXX%" %FLAGS% "%HERE%tests\inject\inject_target.cpp" %OBJ% /Fe"%TOUT%\g6inject_target.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: g6inject_target.exe & exit /b 1)
"%CXX%" %FLAGS% /LD "%HERE%tests\inject\inject_image_dll.cpp" %OBJ% /Fe"%TOUT%\victory-image-0000000000000000.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: victory-image-0000000000000000.dll & exit /b 1)
"%CXX%" %FLAGS% /LD "%HERE%tests\inject\inject_marker_dll.cpp" %OBJ% /Fe"%TOUT%\g6inject_marker.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: g6inject_marker.dll & exit /b 1)
"%CXX%" %FLAGS% %INC% "%HERE%tests\inject\inject_test.cpp" %OBJ% /Fe"%TOUT%\inject_test.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: inject_test.exe & exit /b 1)

rem x86 binaries for the architecture-mismatch cases. Optional: without them those cases are skipped.
"%CXX%" --target=i686-pc-windows-msvc %FLAGS% "%HERE%tests\inject\inject_target.cpp" %OBJ32% /Fe"%TOUT%\g6inject_target32.exe" -fuse-ld=lld >nul 2>nul
if errorlevel 1 echo note: x86 g6inject_target32.exe not built; arch-mismatch cases will be skipped
"%CXX%" --target=i686-pc-windows-msvc %FLAGS% /LD "%HERE%tests\inject\inject_marker_dll.cpp" %OBJ32% /Fe"%TOUT%\g6inject_marker32.dll" -fuse-ld=lld >nul 2>nul
if errorlevel 1 echo note: x86 g6inject_marker32.dll not built; the x86 --dll case will be skipped

echo built: %OUT%\g6inject.exe
rem Top-level test step: an "exit /b 1" from a ( ) block with more commands after it exits 0 under cmd /c.
if /i not "%~1"=="test" exit /b 0
"%TOUT%\inject_test.exe" "%OUT%\g6inject.exe"
if errorlevel 1 (echo TEST FAILED: inject_test.exe & exit /b 1)
exit /b 0
