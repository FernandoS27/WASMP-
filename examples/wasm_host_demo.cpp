// examples/wasm_host_demo.cpp — wasmp running INSIDE a WebAssembly host.
//
// Compiled to wasm32-wasi, this program uses wasmp (with the debug validator
// active) to generate several .wasm modules from within the sandbox, and writes
// them out through WASI. This is the "self-hosting" use case: a plugin, edge
// function, or browser app dynamically producing WebAssembly.
//
//   wasm32-wasi-clang++ -std=c++20 -O2 -Iinclude examples/wasm_host_demo.cpp \
//       -o wasm_host_demo.wasm
//   node run_wasi.mjs           # runs it under a WASI runtime
//   wasm-tools validate out/*.wasm

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <wasmp/wasmp.hpp>

using namespace wasmp;

static void WriteOut(const char* name, const std::vector<u8>& bytes) {
    const std::string path = std::string("/out/") + name;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    std::printf("  generated %-14s %zu bytes\n", name, bytes.size());
}

// add(i32, i32) -> i32
static std::vector<u8> BuildAdd() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32, i32)>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("add", f);
    Function fn(mod, sig);  // validating in this build (no NDEBUG)
    fn.LocalGet(fn.Param(0));
    fn.LocalGet(fn.Param(1));
    fn.I32Add();
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// A loop that counts a local to 10 (control frames + br_if).
static std::vector<u8> BuildLoop() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("count", f);
    Function fn(mod, sig);
    LocalIdx i = fn.DeclareLocal(ValType::I32);
    {
        auto lp = fn.Loop();
        fn.LocalGet(i);
        fn.I32Const(1);
        fn.I32Add();
        fn.LocalSet(i);
        fn.LocalGet(i);
        fn.I32Const(10);
        fn.I32LtS();
        fn.BrIf(lp);
    }
    fn.LocalGet(i);
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// A SIMD function (exercises the feature-gated path + v128).
static std::vector<u8> BuildSimd() {
    Features feat = Features::Core();
    feat.simd = true;
    Module mod(feat);
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("simd_sum", f);
    Function fn(mod, sig);
    fn.I32Const(100);
    fn.I32x4Splat();
    fn.I32Const(23);
    fn.I32x4Splat();
    fn.I32x4Add();
    fn.I32x4ExtractLane(2);
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

int main() {
    std::printf("wasmp is running inside a WebAssembly host.\n");
    std::printf("generating modules from within the sandbox:\n");
    WriteOut("add.wasm", BuildAdd());
    WriteOut("loop.wasm", BuildLoop());
    WriteOut("simd.wasm", BuildSimd());
    std::printf("done — %d modules emitted.\n", 3);
    return 0;
}
