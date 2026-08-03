// wasmp — the umbrella header (Facade).
// Include this and nothing else to get the full public API for building
// WebAssembly modules at runtime:
//
//     #include <wasmp/wasmp.hpp>
//
//     wasmp::Module mod;
//     wasmp::TypeIdx binop = mod.FuncTypeOf<f32(f32, f32)>();
//     wasmp::FunctionEmitter fn(mod, binop);
//     wasmp::FuncIdx square = mod.DeclareFunction(binop);
//     fn.LocalGet(fn.Param(0));
//     fn.LocalGet(fn.Param(0));
//     fn.F32Mul();
//     mod.Export("square", square);
//     mod.DefineFunction(square, fn.Finish());
//     std::vector<u8> wasm = mod.Assemble();
//
// The optional runtime adapters (wasmp/interop/*.hpp) are NOT included here so
// the core stays dependency-free; include them explicitly when needed.
//
// See DESIGN.md and IMPLEMENTATION_PLAN.md.
#ifndef WASMP_WASMP_HPP
#define WASMP_WASMP_HPP

#define WASMP_VERSION_MAJOR 0
#define WASMP_VERSION_MINOR 1
#define WASMP_VERSION_PATCH 0

#include "code_buffer.hpp"
#include "common.hpp"
#include "function_emitter.hpp"
#include "host_traits.hpp"
#include "leb128.hpp"
#include "module.hpp"

namespace wasmp {

// The NDEBUG-selected default emitter. `wasmp::Function fn(mod, sig);` gives a
// validating emitter in debug and a zero-overhead one in release.
using Function = FunctionEmitter<DefaultPolicy>;

}  // namespace wasmp

#endif  // WASMP_WASMP_HPP
