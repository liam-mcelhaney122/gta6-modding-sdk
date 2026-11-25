cmake_minimum_required(VERSION 3.24)

# The hand-written scripts (README "Building", sdk/luau/build.cmd, sdk/runtime/loader/build.cmd)
# build optimised code with the static CRT; default to the same unless the caller chooses.
if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (the scripts build optimised code)")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "CRT (scripts use /MT)")

project(gta6mod_sdk LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(G6_WITH_LUAU "Build the Luau host (fetches Luau from GitHub at configure time)" OFF)
set(G6_LUAU_TAG "0.650" CACHE STRING "Luau git tag to fetch (verify it exists before configuring)")

enable_testing()

# Our own code builds with /W4 /WX, like the scripts.
function(g6_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

# ---------------------------------------------------------------- mock runtime
add_library(gta6mod_mock SHARED runtime/src/mock_backend.c)
target_include_directories(gta6mod_mock PUBLIC include)
target_compile_definitions(gta6mod_mock PRIVATE G6_BUILDING_RUNTIME=1)
g6_strict(gta6mod_mock)

# smoke test of the generated C bindings against the mock DLL
add_executable(g6_natives_c_check tests/c/natives_check.c)
set_target_properties(g6_natives_c_check PROPERTIES OUTPUT_NAME natives_check)
target_link_libraries(g6_natives_c_check PRIVATE gta6mod_mock)
g6_strict(g6_natives_c_check)
add_test(NAME natives_check COMMAND g6_natives_c_check)

# compile-only check of the generated C++ headers (patterns, layouts, invoker), like `clang-cl /c`
add_library(g6_headers_check OBJECT tests/c/headers_check.cpp)
target_include_directories(g6_headers_check PRIVATE include)
g6_strict(g6_headers_check)

# ---------------------------------------------------------------- Luau host
if(G6_WITH_LUAU)
  include(FetchContent)
  # Reuse the checkout sdk/luau/build.cmd makes (same tag) instead of downloading a second copy.
  # An explicit -DFETCHCONTENT_SOURCE_DIR_LUAU=<dir> still wins.
  set(_g6_luau_local "${CMAKE_CURRENT_SOURCE_DIR}/build/_deps/luau-${G6_LUAU_TAG}")
  if(NOT FETCHCONTENT_SOURCE_DIR_LUAU AND EXISTS "${_g6_luau_local}/VM/include/lua.h")
    set(FETCHCONTENT_SOURCE_DIR_LUAU "${_g6_luau_local}")
  endif()
  if(FETCHCONTENT_SOURCE_DIR_LUAU)
    message(STATUS "Luau ${G6_LUAU_TAG}: using local source ${FETCHCONTENT_SOURCE_DIR_LUAU}")
  endif()
  FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG        ${G6_LUAU_TAG}
    GIT_SHALLOW    TRUE)
  set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  # Luau 0.650 declares cmake_minimum_required(VERSION 3.0): CMake 4 rejects it without this, and
  # 3.10 (not 3.5) also avoids the "compatibility with CMake < 3.10" deprecation warning.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  FetchContent_MakeAvailable(luau)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  # Build only the Luau libraries our targets link (Ast, Compiler, VM and their deps).
  set_property(DIRECTORY "${luau_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
  # Upstream warning level, as build.cmd compiles Luau (/W3, not escalated).
  if(MSVC)
    foreach(_t Luau.Ast Luau.Compiler Luau.VM Luau.CLI.lib)
      if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE /W3)
      endif()
    endforeach()
  endif()

  add_library(g6_luau STATIC luau/host/g6_luau.cpp)
  set_target_properties(g6_luau PROPERTIES OUTPUT_NAME g6_luau_host)  # g6_luau.lib is the plugin's import lib
  target_include_directories(g6_luau PUBLIC include luau/host)
  target_compile_definitions(g6_luau PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(g6_luau PUBLIC Luau.VM Luau.Compiler)
  g6_strict(g6_luau)

  # plugin DLL loaded by the in-game runtime (talks only through g6_host_api)
  add_library(g6_luau_plugin SHARED luau/host/g6_luau_plugin.cpp)
  set_target_properties(g6_luau_plugin PROPERTIES OUTPUT_NAME g6_luau)
  target_compile_definitions(g6_luau_plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(g6_luau_plugin PRIVATE g6_luau)
  g6_strict(g6_luau_plugin)

  # offline runner, linked against gta6mod_mock.dll like build.cmd
  add_executable(g6_luau_runner luau/host/g6_luau_runner.cpp)
  target_compile_definitions(g6_luau_runner PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(g6_luau_runner PRIVATE g6_luau gta6mod_mock)
  g6_strict(g6_luau_runner)

  # loads g6_luau.dll through g6_plugin_query/load/unload against the mock host
  add_executable(g6_luau_plugin_check luau/tests/plugin_check.cpp)
  set_target_properties(g6_luau_plugin_check PROPERTIES OUTPUT_NAME plugin_check)
  target_compile_definitions(g6_luau_plugin_check PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(g6_luau_plugin_check PRIVATE gta6mod_mock)
  add_dependencies(g6_luau_plugin_check g6_luau_plugin)
  g6_strict(g6_luau_plugin_check)

  # the same runs as `sdk\luau\build.cmd test`
  set(_g6_luau_lib "${CMAKE_CURRENT_SOURCE_DIR}/luau/lib")
  add_test(NAME luau_spawn_car
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/spawn_car/init.luau"
                   5000 --press 51@1008)
  file(GLOB _g6_luau_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/luau/tests/*.luau")
  foreach(_test ${_g6_luau_tests})
    get_filename_component(_name "${_test}" NAME_WE)
    add_test(NAME luau_${_name}
             COMMAND g6_luau_runner "${_g6_luau_lib}" "${_test}" 3000 --press 51@480 --expect-clean
                     --expect-log "PASS ${_name}")
  endforeach()
  add_test(NAME luau_plugin_check
           COMMAND g6_luau_plugin_check $<TARGET_FILE:g6_luau_plugin> "${_g6_luau_lib}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples")
endif()

# ---------------------------------------------------------------- in-game runtime (optional)
# sdk/runtime/loader: gta6mod_runtime.dll for GTA VI 1.0.2.1 under a VIctory-style host.
# Mirrors sdk/runtime/loader/build.cmd (clang-cl, C++20, /W4 /WX, static CRT, PDB).
option(G6_WITH_RUNTIME_LOADER "Build sdk/runtime/loader (gta6mod_runtime.dll, Windows x64 only)" OFF)
if(G6_WITH_RUNTIME_LOADER)
  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "G6_WITH_RUNTIME_LOADER needs a Windows x64 toolchain")
  endif()
  set(G6_LOADER_SRC
    runtime/loader/src/runtime.cpp runtime/loader/src/hook.cpp runtime/loader/src/x64_decode.cpp
    runtime/loader/src/pattern_scan.cpp runtime/loader/src/identity.cpp runtime/loader/src/natives.cpp
    runtime/loader/src/scheduler.cpp runtime/loader/src/log.cpp runtime/loader/src/game_clock.cpp
    runtime/loader/src/vfs.cpp runtime/loader/src/guard.cpp)
  add_library(gta6mod_runtime SHARED ${G6_LOADER_SRC})
  set_target_properties(gta6mod_runtime PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(gta6mod_runtime PRIVATE include runtime/loader/src)
  target_compile_definitions(gta6mod_runtime PRIVATE G6_BUILDING_RUNTIME=1 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  g6_strict(gta6mod_runtime)
  if(MSVC)
    target_compile_options(gta6mod_runtime PRIVATE /EHsc /GS /Zi)
    target_link_options(gta6mod_runtime PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  endif()
  target_link_libraries(gta6mod_runtime PRIVATE bcrypt iphlpapi kernel32)
  # build.cmd copies the default ini next to the DLL once and never overwrites a user's copy.
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/gta6mod_runtime.ini")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/runtime/loader/gta6mod_runtime.ini"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
  endif()

  add_executable(g6_loader_selftest runtime/loader/tests/loader_selftest.cpp)
  set_target_properties(g6_loader_selftest PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                        OUTPUT_NAME loader_selftest)
  target_include_directories(g6_loader_selftest PRIVATE include runtime/loader/src)
  target_compile_definitions(g6_loader_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  g6_strict(g6_loader_selftest)
  if(MSVC)
    target_compile_options(g6_loader_selftest PRIVATE /EHsc $<$<CONFIG:Release>:/O1>)
  endif()
  add_dependencies(g6_loader_selftest gta6mod_runtime)
  # Set G6_DUMP (folder holding game_scarlett_master.exe) in the environment before ctest to
  # include the read-only dump checks; without it they print "dump: skipped".
  add_test(NAME loader_selftest COMMAND g6_loader_selftest $<TARGET_FILE:gta6mod_runtime>)
endif()
