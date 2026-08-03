# Wasmp!

A header-only **C++20 library for building WebAssembly modules at runtime**. You
call C++ methods, bytes come out, and the result is immediately consumable by a
WASM runtime (wasmtime, WAMR, V8, wasm3, …) whose exports you then call from C or
C++ host code.

It plays the role [sirit](https://github.com/FernandoS27/sirit) plays for
SPIR-V and [xbyak](https://github.com/herumi/xbyak) /
[oaknut](https://github.com/merryhime/oaknut) play for x86 / AArch64 — a fast,
single-pass code generator designed for JIT-style pipelines where modules are
produced dynamically.

## Quick start

```cpp
#include <wasmp/wasmp.hpp>

wasmp::Module mod;

// Declare the signature once; the type entry and the host call path derive from it.
wasmp::TypeIdx sig = mod.FuncTypeOf<int32_t(int32_t, int32_t)>();
wasmp::FuncIdx add = mod.DeclareFunction(sig);
mod.Export("add", add);

wasmp::FunctionEmitter fn(mod, sig);   // validating in debug, zero-overhead in release
fn.LocalGet(fn.Param(0));
fn.LocalGet(fn.Param(1));
fn.I32Add();
mod.DefineFunction(add, fn.Finish());

std::vector<uint8_t> wasm = mod.Assemble();   // a ready-to-load .wasm module
```

See [`examples/`](examples/) for the full `add`, a runtime-JIT polynomial
evaluator executed via wasmtime (`jit_expr`), a dynamic-linking module
(`dynamic_link`), a `v128`-in/`v128`-out SIMD module called from the host
(`simd_host`), and wasmp itself compiled to WebAssembly and run inside a
sandbox (`wasm_host_demo`).

## Features

- **Complete instruction set** — the full WASM core plus SIMD (all 236 ops),
  threads/atomics (all 67 ops), bulk memory, reference types + typed `select`,
  tail calls, multi-memory, and memory64. ~500 instructions, all driven from a
  single opcode table.
- **Fast, single-pass emission** — one bounds check plus a few byte stores per
  instruction; each function body is emitted into its own buffer, so the module
  is sized once and written in a single pass. Deterministic, byte-identical
  output for identical input (content-addressable for module caches).
- **Zero-overhead correctness net** — `FunctionEmitter<Policy>` selects a
  validation policy at compile time. `policy::Validate` (the debug default)
  runs the full WASM type-checker — operand types, the polymorphic
  "unreachable" stack, index ranges, control-frame balance, and **feature-flag
  gating** — reporting at the offending call site. `policy::Trust` (the release
  default) compiles every check away and adds zero state.
- **Host interop, declared once** — `FuncTypeOf<int32_t(float,float)>()` maps a
  C++ signature to a WASM type; the optional `interop/` adapters derive the
  WAMR signature string (`"(ff)i"`) and, for wasmtime, a typed call wrapper
  (`TypedCall<Sig>`, host → wasm) *and* a native-callback importer
  (`MakeHostFunc<Sig>`, wasm → host) from the *same* signature — so a generated
  module can import native C++ functions and call back into them, with the
  signature never transcribed by hand.
- **Header-only, dependency-free, C++20.** No exceptions required (configurable
  error policy), no RTTI, no virtual dispatch on any emission path.

## Using it

Drop [`include/`](include/) into your project and `#include <wasmp/wasmp.hpp>`,
or via CMake:

```cmake
add_subdirectory(wasmp)
target_link_libraries(your_target PRIVATE wasmp::wasmp)
```

Requires a C++20 compiler (tested on Clang and MSVC). On MSVC the conforming
preprocessor is required — the CMake target sets `/Zc:preprocessor`
automatically; add it yourself if you consume the headers without CMake.

## Building the tests

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Two external tools unlock extra test legs when present (auto-detected, skipped
otherwise):

- **[wasm-tools](https://github.com/bytecodealliance/wasm-tools)**
  (`cargo install wasm-tools`) — every emitted module is cross-checked with
  `wasm-tools validate`.
- **wasmtime C API** — set `-DWASMTIME_DIR=<extracted c-api dir>` to build the
  execution tests that instantiate emitted modules and call their exports.

## Running inside a WASM host

wasmp is self-hosting: it compiles to WebAssembly and runs inside a WASM
runtime, so sandboxed code (a plugin, an edge function, a browser app) can
generate `.wasm` modules dynamically. Because the wasi-sdk's libc++ is built
without exceptions, build with `-fno-exceptions` — wasmp is exception-free and
falls back to its abort error policy:

```sh
# compile wasmp (via examples/wasm_host_demo.cpp) to wasm32-wasi
wasm32-wasip1-clang++ -std=c++20 -O2 -fno-exceptions -fno-rtti \
    -Iinclude examples/wasm_host_demo.cpp -o demo.wasm

# run it under a WASI runtime — it emits modules into ./out from the sandbox
node --experimental-wasi-unstable-preview1 examples/run_wasi.mjs demo.wasm out
wasm-tools validate out/add.wasm     # the sandbox-generated module is valid
```

This is exercised by the CMake `wasm_host` test when configured with
`-DWASI_SDK_DIR=<extracted wasi-sdk>` (node + wasm-tools also required): it
compiles wasmp to wasm, runs it under Node's WASI, and validates every module
the sandboxed builder produced. The debug type validator runs inside the
sandbox too.

The SIMD and atomics opcode tables are generated authoritatively from wasm-tools
by [`tools/gen_simd.py`](tools/gen_simd.py) and
[`tools/gen_atomics.py`](tools/gen_atomics.py).
