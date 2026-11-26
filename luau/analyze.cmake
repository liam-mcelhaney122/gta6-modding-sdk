# Type-check the Luau SDK code with luau-analyze (ctest luau_analyze; see sdk/CMakeLists.txt).
#   cmake -DLUAU_ANALYZE=<luau-analyze> -DSDK_DIR=<sdk> -DSTAGE_DIR=<scratch dir> -P analyze.cmake
#
# luau-analyze (Luau 0.650 CLI) resolves require("x") to "<cwd>/x.luau", so the @gta6/* modules are
# staged under STAGE_DIR/@gta6: the libraries from sdk/luau/lib and the type stubs of the host's C
# modules (@gta6/native, @gta6/script) from sdk/luau/types. Each file keeps its own --!strict/--!nonstrict
# mode. Any reported line fails the check (lint warnings included) unless it matches _known below;
# staged paths are mapped back to the sources in the report.
#
# natives.luau: 0.650 cannot export a module type of more than 10000 type nodes (LuauTarjanChildLimit,
# an int flag the CLI cannot set); the full module (~6800 functions) is far over it, so its interface
# would degrade to an error type and every require of it would go unchecked. The full file is still
# checked on its own (full/natives.luau: every wrapper body and annotation), and dependents get a staged
# copy without the RAW namespace (unnamed N_<hash> natives, never referenced by names.luau, examples or
# tests), which fits the limit. If the named natives alone outgrow it, the copy reports the same error
# and the check fails rather than silently losing the types.
foreach(_v LUAU_ANALYZE SDK_DIR STAGE_DIR)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "analyze.cmake: -D${_v}=... is required")
  endif()
endforeach()

# Diagnostics that are limitations of the 0.650 analyzer, not defects in the code (regex per line).
set(_known
  # The full natives module over the export limit (see above); only this file, only at (1,1).
  "^full/natives\\.luau\\(1,1\\): TypeError: Internal error: Code is too complex to typecheck! Consider adding type annotations around this area$"
  # 0.650 declares `vector` as an empty class (EmbeddedBuiltinDefinitions.cpp: "declare class vector end"),
  # while the 0.650 VM reads .x/.y/.z (any case) from a vector (lvmexecute.cpp, LOP_GETTABLEKS).
  ": TypeError: Key '[xyzXYZ]' not found in class 'vector'$")

set(_luau "${SDK_DIR}/luau")
file(REMOVE_RECURSE "${STAGE_DIR}/@gta6" "${STAGE_DIR}/full")
file(MAKE_DIRECTORY "${STAGE_DIR}/@gta6" "${STAGE_DIR}/full")

file(GLOB _libs "${_luau}/lib/*.luau")
file(GLOB _stubs "${_luau}/types/*.luau")
set(_inputs)
set(_map)  # staged name;source path pairs
foreach(_f ${_libs} ${_stubs})
  get_filename_component(_n "${_f}" NAME)
  if(EXISTS "${STAGE_DIR}/@gta6/${_n}")
    message(FATAL_ERROR "analyze.cmake: ${_n} is both a library and a host type stub")
  endif()
  if(_n STREQUAL "natives.luau")
    file(COPY "${_f}" DESTINATION "${STAGE_DIR}/full")
    list(APPEND _inputs "full/natives.luau")
    list(APPEND _map "full/natives.luau" "${_f}")
    file(READ "${_f}" _src)
    set(_begin_mark "\ndo\n\tlocal RAW = {}\n")
    set(_end_mark "\n\tN.RAW = RAW\nend\n")
    string(FIND "${_src}" "${_begin_mark}" _b)
    string(FIND "${_src}" "${_end_mark}" _e)
    if(_b LESS 0 OR _e LESS _b)
      message(FATAL_ERROR "analyze.cmake: RAW namespace block not found in ${_f} (generator layout changed?)")
    endif()
    string(LENGTH "${_end_mark}" _el)
    math(EXPR _tail "${_e} + ${_el}")
    string(SUBSTRING "${_src}" 0 ${_b} _head)
    string(SUBSTRING "${_src}" ${_tail} -1 _rest)
    file(WRITE "${STAGE_DIR}/@gta6/natives.luau" "${_head}\n${_rest}")
    list(APPEND _map "@gta6/natives.luau" "${_f} [staged without RAW]")
  else()
    file(COPY "${_f}" DESTINATION "${STAGE_DIR}/@gta6")
    list(APPEND _inputs "@gta6/${_n}")
    list(APPEND _map "@gta6/${_n}" "${_f}")
  endif()
endforeach()
if(NOT EXISTS "${STAGE_DIR}/@gta6/natives.luau")
  message(FATAL_ERROR "analyze.cmake: ${_luau}/lib/natives.luau is missing")
endif()

file(GLOB _mods "${_luau}/examples/*/*.luau")
file(GLOB _tests "${_luau}/tests/*.luau" "${_luau}/tests/expect_errors/*.luau" "${_luau}/tests/scenarios/*.luau")
list(APPEND _inputs ${_mods} ${_tests})
list(LENGTH _inputs _count)

execute_process(
  COMMAND "${LUAU_ANALYZE}" ${_inputs}
  WORKING_DIRECTORY "${STAGE_DIR}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)

# One entry per line; ';' would split CMake list items, so it travels as a placeholder.
string(REPLACE ";" "<g6semi>" _lines "${_out}${_err}")
string(REPLACE "\r" "" _lines "${_lines}")
string(REPLACE "\n" ";" _lines "${_lines}")
set(_report "")
set(_suppressed 0)
foreach(_line IN LISTS _lines)
  if(_line STREQUAL "")
    continue()
  endif()
  set(_is_known FALSE)
  foreach(_re IN LISTS _known)
    if(_line MATCHES "${_re}")
      set(_is_known TRUE)
    endif()
  endforeach()
  if(_is_known)
    math(EXPR _suppressed "${_suppressed} + 1")
  else()
    string(APPEND _report "${_line}\n")
  endif()
endforeach()
string(REPLACE "<g6semi>" ";" _report "${_report}")
while(_map)
  list(POP_FRONT _map _staged _src)
  string(REPLACE "${_staged}(" "${_src}(" _report "${_report}")
endwhile()

# The exit code only counts files with errors; with every line accounted for, a nonzero exit is fine.
if(NOT _report STREQUAL "")
  message(FATAL_ERROR "luau-analyze: exit ${_rc} over ${_count} files\n${_report}")
endif()
if(NOT _rc EQUAL 0 AND _suppressed EQUAL 0)
  message(FATAL_ERROR "luau-analyze: exit ${_rc} over ${_count} files with no diagnostics")
endif()
message(STATUS "luau-analyze: ${_count} files clean (${_suppressed} known Luau 0.650 analyzer limitations "
               "suppressed, listed in ${CMAKE_CURRENT_LIST_FILE})")
