@echo off
rem Build the UI overlay g6ui.dll, its offline test and the C++ trainer example plugin with clang-cl
rem (x64, C++20, /W4 /WX, static CRT).
rem Usage: build_ui.cmd [test]      "test" also runs build\ui_test\ui_test.exe (default adapter, then WARP)
rem Outputs:
rem   build\g6ui.dll                         loaded by gta6mod_runtime.dll when [ui] overlay=1 (keep it next to it)
rem   build\trainer\g6trainer.dll            example plugin (copy into <runtime folder>\plugins\)
rem   build\ui_test\ui_test.exe, ui_test_plugin.dll
rem Dear ImGui (sdk\third_party\imgui, v1.91.9b) is compiled with the same /W4 /WX; the one exception is
rem -Wno-uninitialized-const-pointer for the vendored files (clang 20+ flags imgui_widgets.cpp:5002, a
rem zero-length stb_textedit_replace of an uninitialised char that is never read). Our files get no exception.
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
set "UOUT=%OUT%\ui_test"
set "TROUT=%OUT%\trainer"
set "IMGUI=%HERE%..\..\third_party\imgui"
for %%D in ("%OUT%" "%OUT%\obj" "%OUT%\obj\ui" "%OUT%\obj\imgui" "%UOUT%" "%TROUT%") do if not exist "%%~D" mkdir "%%~D"

set FLAGS=/nologo /std:c++20 /O2 /W4 /WX /EHsc /MT /GS /Zi /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /FI"gta6mod/imgui_config.h"
set INCS=/I"%HERE%..\..\include" /I"%IMGUI%" /I"%HERE%src" /I"%HERE%ui"

"%CXX%" %FLAGS% -Wno-uninitialized-const-pointer %INCS% /c "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\backends\imgui_impl_dx12.cpp" "%IMGUI%\backends\imgui_impl_win32.cpp" /Fo"%OUT%\obj\imgui\\" /Fd"%OUT%\obj\imgui\\"
if errorlevel 1 (echo BUILD FAILED: Dear ImGui & exit /b 1)
set "IMCORE="%OUT%\obj\imgui\imgui.obj" "%OUT%\obj\imgui\imgui_draw.obj" "%OUT%\obj\imgui\imgui_tables.obj" "%OUT%\obj\imgui\imgui_widgets.obj""

"%CXX%" %FLAGS% %INCS% /LD "%HERE%ui\g6ui.cpp" "%HERE%src\hook.cpp" "%HERE%src\x64_decode.cpp" %IMCORE% "%OUT%\obj\imgui\imgui_impl_dx12.obj" "%OUT%\obj\imgui\imgui_impl_win32.obj" /Fo"%OUT%\obj\ui\\" /Fd"%OUT%\obj\ui\\" /Fe"%OUT%\g6ui.dll" -fuse-ld=lld /link /DEBUG /OPT:REF /OPT:ICF d3d12.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib dwmapi.lib kernel32.lib
if errorlevel 1 (echo BUILD FAILED: g6ui.dll & exit /b 1)

"%CXX%" %FLAGS% %INCS% "%HERE%tests\ui\ui_test.cpp" /Fo"%OUT%\obj\ui\\" /Fd"%OUT%\obj\ui\\" /Fe"%UOUT%\ui_test.exe" -fuse-ld=lld /link d3d12.lib dxgi.lib user32.lib
if errorlevel 1 (echo BUILD FAILED: ui_test.exe & exit /b 1)
"%CXX%" %FLAGS% %INCS% /LD "%HERE%tests\ui\ui_test_plugin.cpp" %IMCORE% /Fo"%OUT%\obj\ui\\" /Fd"%OUT%\obj\ui\\" /Fe"%UOUT%\ui_test_plugin.dll" -fuse-ld=lld /link user32.lib
if errorlevel 1 (echo BUILD FAILED: ui_test_plugin.dll & exit /b 1)

"%CXX%" %FLAGS% %INCS% /LD "%HERE%plugins\g6trainer\g6trainer.cpp" /Fo"%OUT%\obj\ui\\" /Fd"%OUT%\obj\ui\\" /Fe"%TROUT%\g6trainer.dll" -fuse-ld=lld /link /DEBUG kernel32.lib
if errorlevel 1 (echo BUILD FAILED: g6trainer.dll & exit /b 1)
"%CXX%" %FLAGS% %INCS% "%HERE%tests\ui\trainer_mock_test.cpp" /Fo"%OUT%\obj\ui\\" /Fd"%OUT%\obj\ui\\" /Fe"%UOUT%\trainer_mock_test.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: trainer_mock_test.exe & exit /b 1)
"%CXX%" /nologo /O2 /W4 /WX /std:c11 /MT /I"%HERE%..\..\include" /LD "%HERE%..\src\mock_backend.c" /Fo"%OUT%\obj\ui\\" /Fe"%UOUT%\gta6mod_mock.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: ui_test\gta6mod_mock.dll & exit /b 1)

echo built: %OUT%\g6ui.dll %TROUT%\g6trainer.dll
rem Top-level test steps: an "exit /b 1" from a ( ) block with more commands after it exits 0 under cmd /c.
if /i not "%~1"=="test" exit /b 0
"%UOUT%\ui_test.exe" "%OUT%\g6ui.dll" "%UOUT%\ui_test_plugin.dll"
if errorlevel 1 (echo TEST FAILED: ui_test.exe & exit /b 1)
"%UOUT%\ui_test.exe" "%OUT%\g6ui.dll" "%UOUT%\ui_test_plugin.dll" --warp
if errorlevel 1 (echo TEST FAILED: ui_test.exe --warp & exit /b 1)
"%UOUT%\trainer_mock_test.exe" "%UOUT%\gta6mod_mock.dll" "%TROUT%\g6trainer.dll"
if errorlevel 1 (echo TEST FAILED: trainer_mock_test.exe & exit /b 1)
exit /b 0
