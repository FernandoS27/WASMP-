# wasmp — header-only C++20 runtime WebAssembly module builder.
# See DESIGN.md and IMPLEMENTATION_PLAN.md.
cmake_minimum_required(VERSION 3.16)
project(wasmp LANGUAGES CXX VERSION 0.1.0)

# --- The library: header-only INTERFACE target ----------------------------
add_library(wasmp INTERFACE)
add_library(wasmp::wasmp ALIAS wasmp)
target_include_directories(wasmp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(wasmp INTERFACE cxx_std_20)
# The table-driven X-macro needs a conforming preprocessor; MSVC's is opt-in.
target_compile_options(wasmp INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# --- Tests / examples are opt-in (off when consumed as a subproject) -------
option(WASMP_BUILD_TESTS "Build wasmp tests" ${PROJECT_IS_TOP_LEVEL})

if(WASMP_BUILD_TESTS)
    enable_testing()

    # Scratch space for the wasm-tools validation legs. It must NOT be directly
    # in the binary dir: a leg's output dir would collide with the same-named
    # test executable (build/golden_m2 the dir vs build/golden_m2 the exe) on
    # every platform where executables have no .exe suffix.
    set(WASMP_VALIDATE_DIR ${CMAKE_CURRENT_BINARY_DIR}/validate)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(WASMP_WARNINGS -Wall -Wextra -Wpedantic -Werror)
    elseif(MSVC)
        set(WASMP_WARNINGS /W4 /WX)
    endif()

    # Facade contract gate: must parse/typecheck the whole public surface.
    add_executable(facade_smoke tests/facade_smoke.cpp)
    target_link_libraries(facade_smoke PRIVATE wasmp)
    target_compile_options(facade_smoke PRIVATE ${WASMP_WARNINGS})
    add_test(NAME facade_smoke COMMAND facade_smoke)

    # M0 mechanism-layer unit tests.
    add_executable(unit_m0 tests/unit_m0.cpp)
    target_link_libraries(unit_m0 PRIVATE wasmp)
    target_compile_options(unit_m0 PRIVATE ${WASMP_WARNINGS})
    add_test(NAME unit_m0 COMMAND unit_m0)

    # M1 module-assembly golden + determinism tests.
    add_executable(golden_m1 tests/golden_m1.cpp)
    target_link_libraries(golden_m1 PRIVATE wasmp)
    target_compile_options(golden_m1 PRIVATE ${WASMP_WARNINGS})
    add_test(NAME golden_m1 COMMAND golden_m1)

    # Optional external-validation leg: emit the golden modules and run each
    # through `wasm-tools validate`. Gated on the tool being installed.
    find_program(WASM_TOOLS wasm-tools)
    if(WASM_TOOLS)
        add_test(NAME validate_golden COMMAND ${CMAKE_COMMAND}
            -DGOLDEN_EXE=$<TARGET_FILE:golden_m1>
            -DWASM_TOOLS=${WASM_TOOLS}
            -DOUT_DIR=${WASMP_VALIDATE_DIR}/golden_m1
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_golden.cmake)
    else()
        message(STATUS "wasm-tools not found; external validation test disabled")
    endif()

    # M2 interop compile-time checks + module features (multi-value, names).
    add_executable(interop_m2 tests/interop_m2.cpp)
    target_link_libraries(interop_m2 PRIVATE wasmp)
    target_compile_options(interop_m2 PRIVATE ${WASMP_WARNINGS})
    add_test(NAME interop_m2 COMMAND interop_m2)

    add_executable(golden_m2 tests/golden_m2.cpp)
    target_link_libraries(golden_m2 PRIVATE wasmp)
    target_compile_options(golden_m2 PRIVATE ${WASMP_WARNINGS})
    add_test(NAME golden_m2 COMMAND golden_m2)

    if(WASM_TOOLS)
        add_test(NAME validate_golden_m2 COMMAND ${CMAKE_COMMAND}
            -DGOLDEN_EXE=$<TARGET_FILE:golden_m2>
            -DWASM_TOOLS=${WASM_TOOLS}
            -DOUT_DIR=${WASMP_VALIDATE_DIR}/golden_m2
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_golden.cmake)
    endif()

    # M3 validation policy: positive/negative/polymorphic cases + fuzzing.
    add_executable(validate_m3 tests/validate_m3.cpp)
    target_link_libraries(validate_m3 PRIVATE wasmp)
    target_compile_options(validate_m3 PRIVATE ${WASMP_WARNINGS})
    add_test(NAME validate_m3 COMMAND validate_m3)

    find_package(Threads REQUIRED)
    add_executable(thread_stress tests/thread_stress.cpp)
    target_link_libraries(thread_stress PRIVATE wasmp Threads::Threads)
    target_compile_options(thread_stress PRIVATE ${WASMP_WARNINGS})
    add_test(NAME thread_stress COMMAND thread_stress)

    # Differential leg: the validator accepted these random programs — confirm
    # wasm-tools accepts them too.
    if(WASM_TOOLS)
        add_test(NAME validate_fuzz COMMAND ${CMAKE_COMMAND}
            -DGOLDEN_EXE=$<TARGET_FILE:validate_m3>
            -DWASM_TOOLS=${WASM_TOOLS}
            -DOUT_DIR=${WASMP_VALIDATE_DIR}/fuzz
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_golden.cmake)
    endif()

    # M4 proposals: bulk memory, tail calls, multi-memory, memory64, SIMD, atomics.
    add_executable(golden_m4 tests/golden_m4.cpp)
    target_link_libraries(golden_m4 PRIVATE wasmp)
    target_compile_options(golden_m4 PRIVATE ${WASMP_WARNINGS})
    add_test(NAME golden_m4 COMMAND golden_m4)
    if(WASM_TOOLS)
        add_test(NAME validate_golden_m4 COMMAND ${CMAKE_COMMAND}
            -DGOLDEN_EXE=$<TARGET_FILE:golden_m4>
            -DWASM_TOOLS=${WASM_TOOLS}
            -DOUT_DIR=${WASMP_VALIDATE_DIR}/golden_m4
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_golden.cmake)
    endif()

    # Full SIMD table (236 ops, generated by tools/gen_simd.py): emit every op
    # through the library and validate the whole module externally.
    add_executable(simd_full tests/simd_full.cpp)
    target_link_libraries(simd_full PRIVATE wasmp)
    target_compile_options(simd_full PRIVATE ${WASMP_WARNINGS})
    add_test(NAME simd_full COMMAND simd_full)
    if(WASM_TOOLS)
        add_test(NAME validate_simd_full COMMAND ${CMAKE_COMMAND}
            -DGOLDEN_EXE=$<TARGET_FILE:simd_full>
            -DWASM_TOOLS=${WASM_TOOLS}
            -DOUT_DIR=${WASMP_VALIDATE_DIR}/simd_full
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_golden.cmake)
    endif()

    # Full atomics table (67 ops, generated by tools/gen_atomics.py).
    add_executable(atomics_full tests/atomics_full.cpp)
    target_link_libraries(atomics_full PRIVATE wasmp)
    target_compile_options(atomics_full PRIVATE ${WASMP_WARNINGS})
    add_test(NAME atomics_full COMMAND atomics_full)
    if(WASM_TOOLS)
        add_test(NAME validate_atomics_full COMMAND ${CMAKE_COMMAND}
            -DGOLDEN_EXE=$<TARGET_FILE:atomics_full>
            -DWASM_TOOLS=${WASM_TOOLS}
            -DOUT_DIR=${WASMP_VALIDATE_DIR}/atomics_full
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_golden.cmake)
    endif()

    # wasm-host leg: compile wasmp to wasm32-wasi, run it under Node's WASI
    # runtime (it generates modules from inside the sandbox), and validate the
    # output. Gated on the wasi-sdk + node + wasm-tools all being available.
    find_program(NODE_EXE node)
    find_file(WASI_CLANGXX NAMES wasm32-wasip1-clang++.exe wasm32-wasip1-clang++
              HINTS "${WASI_SDK_DIR}/bin" "$ENV{WASI_SDK_DIR}/bin")
    if(WASI_CLANGXX AND NODE_EXE AND WASM_TOOLS)
        add_test(NAME wasm_host COMMAND ${CMAKE_COMMAND}
            -DWASI_CXX=${WASI_CLANGXX}
            -DNODE=${NODE_EXE}
            -DWASM_TOOLS=${WASM_TOOLS}
            -DRUNNER=${CMAKE_CURRENT_SOURCE_DIR}/examples/run_wasi.mjs
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/examples/wasm_host_demo.cpp
            -DINCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/include
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/wasm_host
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/wasm_host_test.cmake)
    else()
        message(STATUS "wasm-host test disabled (needs -DWASI_SDK_DIR=<dir>, node, wasm-tools)")
    endif()

    # Examples double as documentation and smoke coverage.
    add_executable(example_add examples/add.cpp)
    target_link_libraries(example_add PRIVATE wasmp)
    target_compile_options(example_add PRIVATE ${WASMP_WARNINGS})
    add_test(NAME example_add COMMAND example_add)

    add_executable(example_dynamic_link examples/dynamic_link.cpp)
    target_link_libraries(example_dynamic_link PRIVATE wasmp)
    target_compile_options(example_dynamic_link PRIVATE ${WASMP_WARNINGS})
    add_test(NAME example_dynamic_link COMMAND example_dynamic_link)

    # wasmtime execution leg — gated on the C API being available. Point
    # WASMTIME_DIR at an extracted wasmtime-*-c-api directory to enable.
    find_path(WASMTIME_INCLUDE_DIR wasmtime.h HINTS "${WASMTIME_DIR}/include"
              "$ENV{WASMTIME_DIR}/include")
    find_file(WASMTIME_IMPORT_LIB NAMES wasmtime.dll.lib libwasmtime.dll.a wasmtime.lib
              HINTS "${WASMTIME_DIR}/lib" "$ENV{WASMTIME_DIR}/lib")
    if(WASMTIME_INCLUDE_DIR AND WASMTIME_IMPORT_LIB)
        find_file(WASMTIME_DLL wasmtime.dll HINTS "${WASMTIME_DIR}/lib" "$ENV{WASMTIME_DIR}/lib")

        add_executable(exec_wasmtime tests/exec_wasmtime.cpp)
        target_include_directories(exec_wasmtime PRIVATE ${WASMTIME_INCLUDE_DIR})
        target_link_libraries(exec_wasmtime PRIVATE wasmp ${WASMTIME_IMPORT_LIB})
        add_test(NAME exec_wasmtime COMMAND exec_wasmtime)

        add_executable(example_jit_expr examples/jit_expr.cpp)
        target_include_directories(example_jit_expr PRIVATE ${WASMTIME_INCLUDE_DIR})
        target_link_libraries(example_jit_expr PRIVATE wasmp ${WASMTIME_IMPORT_LIB})
        add_test(NAME example_jit_expr COMMAND example_jit_expr)

        # Passing SIMD (v128) values from the host into a JITed module.
        add_executable(example_simd_host examples/simd_host.cpp)
        target_include_directories(example_simd_host PRIVATE ${WASMTIME_INCLUDE_DIR})
        target_link_libraries(example_simd_host PRIVATE wasmp ${WASMTIME_IMPORT_LIB})
        add_test(NAME example_simd_host COMMAND example_simd_host)

        if(WASMTIME_DLL)  # Windows: place the runtime DLL next to the exes.
            foreach(_t exec_wasmtime example_jit_expr example_simd_host)
                add_custom_command(TARGET ${_t} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${WASMTIME_DLL} $<TARGET_FILE_DIR:${_t}>)
            endforeach()
        endif()
    else()
        message(STATUS "wasmtime C API not found; execution tests disabled "
                       "(set -DWASMTIME_DIR=<extracted c-api dir> to enable)")
    endif()
endif()
