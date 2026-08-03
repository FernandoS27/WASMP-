# wasm_host_test.cmake — prove wasmp works INSIDE a WebAssembly host.
# Compiles the demo to wasm32-wasi, runs it under Node's WASI runtime (it emits
# .wasm modules from within the sandbox), then validates those with wasm-tools.
# Invoked via `cmake -P` from CTest; gated on the wasi-sdk + node being present.
#   -DWASI_CXX -DNODE -DWASM_TOOLS -DRUNNER -DSRC -DINCLUDE -DWORKDIR
file(MAKE_DIRECTORY "${WORKDIR}/wasmout")
set(_wasm "${WORKDIR}/wasm_host_demo.wasm")

# The wasi-sdk libc++ is built without exceptions; wasmp is -fno-exceptions
# clean and falls back to its abort error policy.
execute_process(
    COMMAND "${WASI_CXX}" -std=c++20 -O2 -fno-exceptions -fno-rtti
            "-I${INCLUDE}" "${SRC}" -o "${_wasm}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "wasm32-wasi compile of the demo failed")
endif()

execute_process(
    COMMAND "${NODE}" --experimental-wasi-unstable-preview1 "${RUNNER}"
            "${_wasm}" "${WORKDIR}/wasmout"
    RESULT_VARIABLE _rr)
if(NOT _rr EQUAL 0)
    message(FATAL_ERROR "running the sandboxed wasmp under WASI failed")
endif()

file(GLOB _gen "${WORKDIR}/wasmout/*.wasm")
if(NOT _gen)
    message(FATAL_ERROR "the sandboxed wasmp emitted no modules")
endif()
foreach(_g ${_gen})
    execute_process(COMMAND "${WASM_TOOLS}" validate --features=all "${_g}"
                    RESULT_VARIABLE _v)
    if(NOT _v EQUAL 0)
        message(FATAL_ERROR "sandbox-generated module failed validation: ${_g}")
    endif()
    message(STATUS "wasm-host: generated + validated ${_g}")
endforeach()
