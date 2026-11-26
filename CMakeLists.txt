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
option(G6_LUAU_ANALYZE "With G6_WITH_LUAU: build luau-analyze from the same Luau source and add the luau_analyze test" ON)

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

# generated control-action header: constants vs g6_joaat, the index table, C and C++ linkage
add_executable(g6_controls_check tests/c/controls_check.c tests/c/controls_check_cpp.cpp)
set_target_properties(g6_controls_check PROPERTIES OUTPUT_NAME controls_check)
target_include_directories(g6_controls_check PRIVATE include)
g6_strict(g6_controls_check)
add_test(NAME controls_check COMMAND g6_controls_check)

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
  # The CLI targets are only declared here; EXCLUDE_FROM_ALL below builds just luau-analyze (if wanted).
  set(LUAU_BUILD_CLI ${G6_LUAU_ANALYZE} CACHE BOOL "" FORCE)
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
  add_executable(g6_luau_runner luau/host/g6_luau_runner.cpp runtime/src/mock_world.cpp)
  target_include_directories(g6_luau_runner PRIVATE runtime/src)
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
                   5000 --press INPUT_CONTEXT@1008)
  # script-layer examples: scenario flags and expected log lines from sdk/docs/script_layer.md
  add_test(NAME luau_example_god_mode
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/god_mode/init.luau"
                   4000 --damage 50@496 --press INPUT_DETONATE@1008 --damage 50@2000 --press INPUT_DETONATE@3008 --damage 30@3504 --expect-clean --expect-log "god mode ON" --expect-log "god mode OFF")
  add_test(NAME luau_example_never_wanted
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/never_wanted/init.luau"
                   4000 --press INPUT_VEH_HEADLIGHT@496 --wanted 3@1504 --press INPUT_VEH_HEADLIGHT@3008 --expect-clean --expect-log "never wanted ON (max wanted level was 5)" --expect-log "cleared wanted level 3" --expect-log "never wanted OFF (max wanted level restored to 5)")
  add_test(NAME luau_example_waypoint_teleport
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/waypoint_teleport/init.luau"
                   3000 --press INPUT_VEH_DUCK@496 --waypoint 250,-400,0@1008 --press INPUT_VEH_DUCK@1504 --expect-clean --expect-log "no waypoint set" --expect-log "teleported to 250.0 -400.0 13.0")
  add_test(NAME luau_example_vehicle_spawner
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/vehicle_spawner/init.luau"
                   3000 --press INPUT_CONTEXT_SECONDARY@496 --damage 150@2000 --expect-clean --expect-log "spawned adder and seated the player" --expect-log "repaired vehicle (850 -> 1000)")
  add_test(NAME luau_example_time_control
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/time_control/init.luau"
                   3000 --press INPUT_MULTIPLAYER_INFO@496 --press INPUT_MULTIPLAYER_INFO@1008 --press INPUT_SPECIAL_ABILITY@1504 --press INPUT_MULTIPLAYER_INFO@1600+1200 --expect-clean --expect-log "time scale 0.50" --expect-log "time scale 0.20" --expect-log "clock 13:00" --expect-log "time scale reset to 1.00")
  add_test(NAME luau_example_trainer_menu
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/trainer_menu/init.luau"
                   3000 --damage 30@240 --ui-click "God mode@496" --damage 50@1008 --ui-click "Never wanted@1504" --wanted 3@2000 --ui-click Heal@2496 --expect-clean --expect-log "god mode ON" --expect-log "never wanted ON (max wanted level was 5)" --expect-log "cleared wanted level 3" --expect-log "healed" --expect-ui "text Player: -1000.0 -2000.0 15.0" --expect-ui "checkbox God mode = 1"
                   --expect-ui-log "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/trainer_menu.ui.txt")
  add_test(NAME luau_scenario_ui_widgets
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/tests/scenarios/ui_widgets.luau"
                   1500 --ui-click Heal@96 --ui-click "God mode@96" --ui-set Wanted=4@96 --ui-set Speed=2.5@96 --ui-set Name=Jason@96 --ui-set Weather=Fog@96 --ui-click Advanced@96 --ui-click Second@96 --ui-set Tint=0.1,0.2,0.3,0.4@96 --ui-menu 0@496 --ui-menu 1@800 --expect-clean --expect-ui "checkbox God mode = 1" --expect-ui "tab_item Second (selected)" --expect-log "PASS ui_widgets"
                   --expect-ui-log "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/ui_widgets.ui.txt")
  add_test(NAME luau_scenario_objective
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/tests/scenarios/objective.luau"
                   1500 --expect-clean --expect-log "busyspinner: Saving..." --expect-log "objective shown: Lose the cops (500 ms)" --expect-log "objective shown: Second (-1 ms)" --expect-log "objective cleared" --expect-log "PASS objective")
  add_test(NAME luau_scenario_hotkeys_hold
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/tests/scenarios/hotkeys_hold.luau"
                   2000 --press INPUT_CONTEXT@96 --press INPUT_CONTEXT@400+800 --press INPUT_SPRINT@1504+300 --press INPUT_CONTEXT@1520+100 --press INPUT_JUMP@0+600 --expect-clean --expect-log "PASS hotkeys_hold")
  # the host's stop event (G-086): toggles off, every script cancelled
  add_test(NAME luau_stop_event_god_mode
           COMMAND g6_luau_runner "${_g6_luau_lib}" "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples/god_mode/init.luau"
                   3008 --press INPUT_DETONATE@496 --unload-at 1504 --damage 50@2000 --expect-clean --expect-log "god mode ON" --expect-log "god mode OFF" --expect-live 0)
  file(GLOB _g6_luau_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/luau/tests/*.luau")
  foreach(_test ${_g6_luau_tests})
    get_filename_component(_name "${_test}" NAME_WE)
    add_test(NAME luau_${_name}
             COMMAND g6_luau_runner "${_g6_luau_lib}" "${_test}" 3000 --press 51@480 --expect-clean
                     --expect-log "PASS ${_name}")
  endforeach()
  # Tests that log an error on purpose: no --expect-clean.
  file(GLOB _g6_luau_err_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/luau/tests/expect_errors/*.luau")
  foreach(_test ${_g6_luau_err_tests})
    get_filename_component(_name "${_test}" NAME_WE)
    add_test(NAME luau_${_name}
             COMMAND g6_luau_runner "${_g6_luau_lib}" "${_test}" 500 --expect-log "PASS ${_name}")
  endforeach()
  add_test(NAME luau_plugin_check
           COMMAND g6_luau_plugin_check $<TARGET_FILE:g6_luau_plugin> "${_g6_luau_lib}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/luau/examples")

  # Type check of luau/lib, luau/examples and luau/tests against the host-module stubs in luau/types.
  if(G6_LUAU_ANALYZE)
    add_custom_target(g6_luau_analyze ALL)
    add_dependencies(g6_luau_analyze Luau.Analyze.CLI)
    add_test(NAME luau_analyze
             COMMAND ${CMAKE_COMMAND} -DLUAU_ANALYZE=$<TARGET_FILE:Luau.Analyze.CLI>
                     "-DSDK_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DSTAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/luau_analyze"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/luau/analyze.cmake")
  endif()
endif()

# ---------------------------------------------------------------- in-game Python host (optional)
# sdk/python/host: g6_python.dll, a runtime plugin embedding CPython 3.12 (sdk/docs/python_host.md), and
# g6_python_test, which runs it against gta6mod_mock and the shared mock world. Mirrors sdk/python/host/build.cmd.
option(G6_WITH_PYTHON_HOST "Build g6_python.dll and g6_python_test (Windows x64, CPython 3.12 headers and libs)" OFF)
if(G6_WITH_PYTHON_HOST)
  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "G6_WITH_PYTHON_HOST needs a Windows x64 toolchain")
  endif()
  set(G6_PYTHON_HOME "" CACHE PATH "CPython 3.12 home (include/Python.h, libs/python312.lib, python312.dll); empty = %LOCALAPPDATA%/Programs/Python/Python312")
  set(_g6_py_home "${G6_PYTHON_HOME}")
  if(NOT _g6_py_home)
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/Python/Python312" _g6_py_home)
  endif()
  foreach(_f include/Python.h libs/python312.lib python312.dll)
    if(NOT EXISTS "${_g6_py_home}/${_f}")
      message(FATAL_ERROR "G6_WITH_PYTHON_HOST: ${_g6_py_home}/${_f} not found; set G6_PYTHON_HOME to a CPython 3.12 install")
    endif()
  endforeach()
  message(STATUS "g6_python: CPython 3.12 from ${_g6_py_home}")

  add_library(g6_python SHARED python/host/g6_python.cpp)
  set_target_properties(g6_python PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python")
  target_include_directories(g6_python PRIVATE include)
  target_include_directories(g6_python SYSTEM PRIVATE "${_g6_py_home}/include")
  target_compile_definitions(g6_python PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE NDEBUG)
  g6_strict(g6_python)
  if(MSVC)
    target_compile_options(g6_python PRIVATE /EHsc /GS)
  endif()
  # python312.dll is delay-loaded: the plugin loads without Python and LoadLibraryExW's [python] home first.
  target_link_directories(g6_python PRIVATE "${_g6_py_home}/libs")
  target_link_libraries(g6_python PRIVATE python312 delayimp kernel32)
  target_link_options(g6_python PRIVATE /DELAYLOAD:python312.dll)
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/python/g6_python.ini")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/python/host/g6_python.ini" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/python")
  endif()

  set(_g6_py_test_dir "${CMAKE_CURRENT_BINARY_DIR}/python_test")
  add_executable(g6_python_test python/host/tests/g6_python_test.cpp runtime/src/mock_world.cpp runtime/src/ui_retained.cpp)
  set_target_properties(g6_python_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                        RUNTIME_OUTPUT_DIRECTORY "${_g6_py_test_dir}")
  target_include_directories(g6_python_test PRIVATE include runtime/src)
  target_compile_definitions(g6_python_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(g6_python_test PRIVATE gta6mod_mock)
  g6_strict(g6_python_test)
  if(MSVC)
    target_compile_options(g6_python_test PRIVATE /EHsc)
  endif()
  add_dependencies(g6_python_test g6_python)
  add_custom_command(TARGET g6_python_test POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gta6mod_mock> "${_g6_py_test_dir}")
  add_test(NAME g6_python_test
           COMMAND g6_python_test $<TARGET_FILE:g6_python> "${CMAKE_CURRENT_SOURCE_DIR}" "${_g6_py_home}"
                   "${_g6_py_test_dir}/work")
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
    runtime/loader/src/vfs.cpp runtime/loader/src/guard.cpp runtime/loader/src/binder.cpp
    runtime/src/ui_retained.cpp)
  add_library(gta6mod_runtime SHARED ${G6_LOADER_SRC})
  set_target_properties(gta6mod_runtime PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(gta6mod_runtime PRIVATE include runtime/loader/src runtime/loader/ui runtime/src)
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

  # Flags shared by build_inject.cmd and build_probe.cmd (C++20, /W4 /WX, /EHsc /GS, static CRT).
  function(g6_loader_tool target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    g6_strict(${target})
    if(MSVC)
      target_compile_options(${target} PRIVATE /EHsc /GS)
    endif()
  endfunction()

  # g6inject.exe (build_inject.cmd). Its test binaries go to inject_test/ beside it, where inject_test
  # also looks for ..\gta6mod_runtime.dll (late-load smoke test). The x86 binaries for the
  # architecture-mismatch cases are not built here (one toolchain, one architecture): those cases SKIP.
  add_executable(g6inject runtime/loader/tools/inject/g6inject.cpp)
  g6_loader_tool(g6inject)
  target_include_directories(g6inject PRIVATE runtime/loader/tools/inject)
  if(MSVC)
    target_compile_options(g6inject PRIVATE /Zi)
    target_link_options(g6inject PRIVATE /DEBUG)
  endif()
  target_link_libraries(g6inject PRIVATE kernel32)

  set(_g6_inject_dir "${CMAKE_CURRENT_BINARY_DIR}/inject_test")
  add_executable(g6inject_target runtime/loader/tests/inject/inject_target.cpp)
  add_library(g6inject_image SHARED runtime/loader/tests/inject/inject_image_dll.cpp)
  set_target_properties(g6inject_image PROPERTIES OUTPUT_NAME victory-image-0000000000000000)
  add_library(g6inject_marker SHARED runtime/loader/tests/inject/inject_marker_dll.cpp)
  add_executable(g6_inject_test runtime/loader/tests/inject/inject_test.cpp)
  set_target_properties(g6_inject_test PROPERTIES OUTPUT_NAME inject_test)
  target_include_directories(g6_inject_test PRIVATE runtime/loader/tools/inject)
  foreach(_t g6inject_target g6inject_image g6inject_marker g6_inject_test)
    g6_loader_tool(${_t})
    set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_g6_inject_dir}")
  endforeach()
  add_dependencies(g6_inject_test g6inject g6inject_target g6inject_image g6inject_marker gta6mod_runtime)
  add_test(NAME inject_test COMMAND g6_inject_test $<TARGET_FILE:g6inject>)

  # g6probe plugin (build_probe.cmd): probe/g6probe.dll plus a default g6probe.ini, copied once and
  # never over a user's copy; unit test and mock-world test in probe_test/.
  add_library(g6probe SHARED runtime/loader/plugins/g6probe/g6probe.cpp)
  g6_loader_tool(g6probe)
  set_target_properties(g6probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/probe")
  target_include_directories(g6probe PRIVATE include runtime/loader/plugins/g6probe)
  if(MSVC)
    target_compile_options(g6probe PRIVATE /Zi)
    target_link_options(g6probe PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  endif()
  target_link_libraries(g6probe PRIVATE kernel32 user32)
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/probe/g6probe.ini")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/runtime/loader/plugins/g6probe/g6probe.ini"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/probe")
  endif()

  set(_g6_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/probe_test")
  add_executable(g6probe_test runtime/loader/tests/probe/g6probe_test.cpp)
  target_include_directories(g6probe_test PRIVATE include runtime/loader/plugins/g6probe runtime/loader/src)
  add_library(g6probe_fake_title SHARED runtime/loader/tests/probe/fake_title.cpp)
  set_target_properties(g6probe_fake_title PROPERTIES OUTPUT_NAME fake_title)
  add_executable(g6probe_mock_test runtime/loader/tests/probe/probe_mock_test.cpp)
  set_target_properties(g6probe_mock_test PROPERTIES OUTPUT_NAME probe_mock_test)
  target_include_directories(g6probe_mock_test PRIVATE include)
  foreach(_t g6probe_test g6probe_fake_title g6probe_mock_test)
    g6_loader_tool(${_t})
    set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_g6_probe_dir}")
  endforeach()
  add_dependencies(g6probe_mock_test g6probe g6probe_fake_title gta6mod_mock)
  add_test(NAME g6probe_test COMMAND g6probe_test)

  # UI overlay (build_ui.cmd): g6ui.dll beside the runtime (loaded with [ui] overlay=1), its D3D12 test, and the
  # C++ trainer example plugin with its mock-world test. Dear ImGui (third_party/imgui, v1.91.9b) is built with
  # the same /W4 /WX and gta6mod/imgui_config.h as its user config (the scripts force-include the same header).
  set(_g6_imgui "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
  add_library(g6_imgui_core STATIC ${_g6_imgui}/imgui.cpp ${_g6_imgui}/imgui_draw.cpp ${_g6_imgui}/imgui_tables.cpp
              ${_g6_imgui}/imgui_widgets.cpp)
  target_include_directories(g6_imgui_core PUBLIC ${_g6_imgui} include)
  target_compile_definitions(g6_imgui_core PUBLIC "IMGUI_USER_CONFIG=\"gta6mod/imgui_config.h\"")
  add_library(g6_imgui_win_dx12 STATIC ${_g6_imgui}/backends/imgui_impl_dx12.cpp ${_g6_imgui}/backends/imgui_impl_win32.cpp)
  target_compile_definitions(g6_imgui_win_dx12 PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
  target_link_libraries(g6_imgui_win_dx12 PUBLIC g6_imgui_core)
  foreach(_t g6_imgui_core g6_imgui_win_dx12)
    g6_loader_tool(${_t})
  endforeach()

  add_library(g6ui SHARED runtime/loader/ui/g6ui.cpp runtime/loader/src/hook.cpp runtime/loader/src/x64_decode.cpp)
  g6_loader_tool(g6ui)
  target_include_directories(g6ui PRIVATE include runtime/loader/src runtime/loader/ui)
  target_link_libraries(g6ui PRIVATE g6_imgui_win_dx12 d3d12 dxgi d3dcompiler user32 gdi32 dwmapi kernel32)

  set(_g6_ui_dir "${CMAKE_CURRENT_BINARY_DIR}/ui_test")
  add_executable(g6ui_test runtime/loader/tests/ui/ui_test.cpp)
  set_target_properties(g6ui_test PROPERTIES OUTPUT_NAME ui_test)
  target_include_directories(g6ui_test PRIVATE include runtime/loader/ui)
  target_link_libraries(g6ui_test PRIVATE d3d12 dxgi user32)
  add_library(g6ui_test_plugin SHARED runtime/loader/tests/ui/ui_test_plugin.cpp)
  set_target_properties(g6ui_test_plugin PROPERTIES OUTPUT_NAME ui_test_plugin)
  target_link_libraries(g6ui_test_plugin PRIVATE g6_imgui_core user32)
  add_library(g6trainer SHARED runtime/loader/plugins/g6trainer/g6trainer.cpp)
  set_target_properties(g6trainer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/trainer")
  target_include_directories(g6trainer PRIVATE include)
  add_executable(g6trainer_mock_test runtime/loader/tests/ui/trainer_mock_test.cpp)
  set_target_properties(g6trainer_mock_test PROPERTIES OUTPUT_NAME trainer_mock_test)
  target_include_directories(g6trainer_mock_test PRIVATE include)
  foreach(_t g6ui_test g6ui_test_plugin g6trainer g6trainer_mock_test)
    g6_loader_tool(${_t})
  endforeach()
  foreach(_t g6ui_test g6ui_test_plugin g6trainer_mock_test)
    set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_g6_ui_dir}")
  endforeach()
  add_dependencies(g6ui_test g6ui g6ui_test_plugin)
  add_dependencies(g6trainer_mock_test g6trainer gta6mod_mock)
  add_test(NAME ui_test COMMAND g6ui_test $<TARGET_FILE:g6ui> $<TARGET_FILE:g6ui_test_plugin>)
  add_test(NAME ui_test_warp COMMAND g6ui_test $<TARGET_FILE:g6ui> $<TARGET_FILE:g6ui_test_plugin> --warp)
  add_test(NAME trainer_mock_test COMMAND g6trainer_mock_test $<TARGET_FILE:gta6mod_mock> $<TARGET_FILE:g6trainer>)
  add_test(NAME probe_mock_test
           COMMAND g6probe_mock_test $<TARGET_FILE:gta6mod_mock> $<TARGET_FILE:g6probe>
                   $<TARGET_FILE:g6probe_fake_title> "${_g6_probe_dir}/work")
endif()
