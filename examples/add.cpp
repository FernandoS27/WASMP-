// examples/add.cpp — the smallest end-to-end wasmp program.
// Builds a module exporting `add(i32, i32) -> i32`, assembles it to bytes, and
// writes add.wasm. Load it in any runtime (wasmtime, WAMR, a browser) and call
// the export; the M2 interop headers show the host-side call path.
//
//   clang++ -std=c++20 -Iinclude examples/add.cpp -o add && ./add
//   wasm-tools validate add.wasm && wasm-tools print add.wasm

#define _CRT_SECURE_NO_WARNINGS  // std::fopen on MSVC UCRT

#include <cstdio>
#include <cstdint>
#include <vector>

#include <wasmp/wasmp.hpp>

int main() {
    using namespace wasmp;

    Module mod;

    // Declare the signature once — the type entry and (via the interop headers)
    // the host call wrapper both derive from it.
    TypeIdx sig = mod.FuncTypeOf<i32(i32, i32)>();

    // Assign the function index up front, then attach the body.
    FuncIdx add = mod.DeclareFunction(sig);
    mod.Export("add", add);
    mod.SetName(add, "add");

    FunctionEmitter fn(mod, sig);   // wasmp::Function = policy chosen by NDEBUG
    fn.LocalGet(fn.Param(0));
    fn.LocalGet(fn.Param(1));
    fn.I32Add();
    mod.DefineFunction(add, fn.Finish());

    std::vector<u8> wasm = mod.Assemble();

    if (FILE* f = std::fopen("add.wasm", "wb")) {
        std::fwrite(wasm.data(), 1, wasm.size(), f);
        std::fclose(f);
    }
    std::printf("wrote add.wasm (%zu bytes)\n", wasm.size());
    return 0;
}
