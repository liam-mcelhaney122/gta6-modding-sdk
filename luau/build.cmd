@echo off
rem Build the Luau host with clang-cl (x64, C++17): no cmake needed.
rem   sdk\build\luau\g6_luau.dll          plugin DLL loaded by the runtime (talks only through g6_host_api)
rem   sdk\build\luau\g6_luau_runner.exe   offline runner, linked against gta6mod_mock.dll/.lib
rem   sdk\build\luau\gta6mod_mock.dll     mock runtime, rebuilt here from runtime\src\mock_backend.c
rem Luau itself is fetched (git, shallow) into sdk\build\_deps\luau-<tag> at the tag pinned by
rem G6_LUAU_TAG in sdk\CMakeLists.txt, and compiled once into sdk\build\luau\luau-<tag>.lib.
rem Usage: build.cmd [rebuild] [test]
rem   rebuild  recompile the Luau library too
rem   test     run the example mod, every sdk\luau\tests\*.luau (each must log "PASS <name>"), plugin_check
rem The tag below must equal G6_LUAU_TAG in sdk\CMakeLists.txt.
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "SDK=%HERE%.."
set "TAG=0.650"
set "LUAU=%SDK%\build\_deps\luau-%TAG%"
set "OUT=%SDK%\build\luau"
set "OBJ=%OUT%\obj"
set "LLVM=C:\Program Files\LLVM\bin"
if exist "%LLVM%\clang-cl.exe" (
  set "CXX=%LLVM%\clang-cl.exe"
  set "LIB_EXE=%LLVM%\llvm-lib.exe"
) else (
  where clang-cl >nul 2>nul || (echo clang-cl not found & exit /b 1)
  set "CXX=clang-cl"
  set "LIB_EXE=llvm-lib"
)
findstr /c:"set(G6_LUAU_TAG \"%TAG%\"" "%SDK%\CMakeLists.txt" >nul || (echo Luau tag %TAG% differs from G6_LUAU_TAG in sdk\CMakeLists.txt & exit /b 1)
set REBUILD=0
set TEST=0
for %%A in (%*) do (
  if /i "%%~A"=="rebuild" set REBUILD=1
  if /i "%%~A"=="test" set TEST=1
)

if not exist "%LUAU%\VM\include\lua.h" (
  where git >nul 2>nul || (echo Luau %TAG% missing at %LUAU% and git not found & exit /b 1)
  echo fetching Luau %TAG% into %LUAU%
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch %TAG% https://github.com/luau-lang/luau.git "%LUAU%"
  if errorlevel 1 (echo FETCH FAILED: luau %TAG% & exit /b 1)
)
if not exist "%OBJ%\luau" mkdir "%OBJ%\luau"
if not exist "%OBJ%\host" mkdir "%OBJ%\host"

rem Luau sources: upstream flags (C++17, /D_CRT_SECURE_NO_WARNINGS), warnings not escalated.
set "LUAULIB=%OUT%\luau-%TAG%.lib"
if "%REBUILD%"=="1" if exist "%LUAULIB%" del /q "%LUAULIB%"
if not exist "%LUAULIB%" (
  echo compiling Luau %TAG% ^(Ast, Compiler, VM^)
  set "LFLAGS=/nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I"%LUAU%\Common\include" /I"%LUAU%\Ast\include" /I"%LUAU%\Compiler\include" /I"%LUAU%\VM\include""
  del /q "%OBJ%\luau\*.obj" 2>nul
  for %%D in (Ast Compiler VM) do (
    for %%F in ("%LUAU%\%%D\src\*.cpp") do (
      "%CXX%" !LFLAGS! /c "%%~fF" /Fo"%OBJ%\luau\%%D_%%~nF.obj"
      if errorlevel 1 (echo BUILD FAILED: %%D\src\%%~nxF & exit /b 1)
    )
  )
  "%LIB_EXE%" /nologo /out:"%LUAULIB%" "%OBJ%\luau\*.obj"
  if errorlevel 1 (echo BUILD FAILED: luau-%TAG%.lib & exit /b 1)
)

rem Our files: /W4 /WX.
set FLAGS=/nologo /std:c++17 /O2 /W4 /WX /EHsc /MT /D_CRT_SECURE_NO_WARNINGS
set INCS=/I"%SDK%\include" /I"%HERE%host" /I"%LUAU%\VM\include" /I"%LUAU%\Compiler\include"

"%CXX%" /nologo /O2 /W4 /WX /std:c11 /MT /I"%SDK%\include" /LD "%SDK%\runtime\src\mock_backend.c" /Fo"%OBJ%\host\\" /Fe"%OUT%\gta6mod_mock.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: gta6mod_mock.dll & exit /b 1)

"%CXX%" %FLAGS% %INCS% /c "%HERE%host\g6_luau.cpp" "%HERE%host\g6_luau_plugin.cpp" "%HERE%host\g6_luau_runner.cpp" /Fo"%OBJ%\host\\"
if errorlevel 1 (echo BUILD FAILED: luau host & exit /b 1)

"%CXX%" /nologo /MT /LD "%OBJ%\host\g6_luau_plugin.obj" "%OBJ%\host\g6_luau.obj" "%LUAULIB%" /Fe"%OUT%\g6_luau.dll" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: g6_luau.dll & exit /b 1)

"%CXX%" /nologo /MT "%OBJ%\host\g6_luau_runner.obj" "%OBJ%\host\g6_luau.obj" "%LUAULIB%" "%OUT%\gta6mod_mock.lib" /Fe"%OUT%\g6_luau_runner.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: g6_luau_runner.exe & exit /b 1)

"%CXX%" %FLAGS% /I"%SDK%\include" "%HERE%tests\plugin_check.cpp" "%OUT%\gta6mod_mock.lib" /Fo"%OBJ%\host\\" /Fe"%OUT%\plugin_check.exe" -fuse-ld=lld
if errorlevel 1 (echo BUILD FAILED: plugin_check.exe & exit /b 1)

echo built: %OUT%\g6_luau.dll %OUT%\g6_luau_runner.exe
if "%TEST%"=="0" exit /b 0

echo --- example: spawn_car (5000 ms, control 51 pressed at 1008 ms)
"%OUT%\g6_luau_runner.exe" "%HERE%lib" "%HERE%examples\spawn_car\init.luau" 5000 --press 51@1008
if errorlevel 1 (echo FAILED: spawn_car example & exit /b 1)
echo --- tests
for %%T in ("%HERE%tests\*.luau") do (
  "%OUT%\g6_luau_runner.exe" "%HERE%lib" "%%~fT" 3000 --press 51@480 --expect-clean --expect-log "PASS %%~nT"
  if errorlevel 1 (echo FAILED: %%~nxT & exit /b 1)
)
echo --- plugin: g6_luau.dll through g6_plugin_query/load/unload
"%OUT%\plugin_check.exe" "%OUT%\g6_luau.dll" "%HERE%lib" "%HERE%examples"
if errorlevel 1 (echo FAILED: plugin_check & exit /b 1)
echo all Luau tests passed
exit /b 0
