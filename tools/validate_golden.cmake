# validate_golden.cmake — CI leg: emit the golden modules, then run every one
# through `wasm-tools validate`. Invoked via `cmake -P` from CTest.
#   -DGOLDEN_EXE=<path to golden_m1>  -DWASM_TOOLS=<path>  -DOUT_DIR=<dir>
file(MAKE_DIRECTORY "${OUT_DIR}")
file(GLOB _old "${OUT_DIR}/*.wasm")
if(_old)
    file(REMOVE ${_old})
endif()

set(ENV{WASMP_GOLDEN_DIR} "${OUT_DIR}")
execute_process(COMMAND "${GOLDEN_EXE}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "golden_m1 self-checks failed (rc=${_rc})")
endif()

file(GLOB _wasms "${OUT_DIR}/*.wasm")
if(NOT _wasms)
    message(FATAL_ERROR "no .wasm files were emitted to ${OUT_DIR}")
endif()

set(_failed 0)
foreach(_w ${_wasms})
    execute_process(COMMAND "${WASM_TOOLS}" validate --features=all "${_w}" RESULT_VARIABLE _vr)
    if(_vr EQUAL 0)
        message(STATUS "validate PASS ${_w}")
    else()
        message(WARNING "validate FAIL ${_w}")
        set(_failed 1)
    endif()
endforeach()

if(_failed)
    message(FATAL_ERROR "one or more emitted modules failed wasm-tools validation")
endif()
