// examples/dynamic_link.cpp — the dynamic-linking convention: import the
// memory and a `__heap_base` global from the host, and place a data segment at
// a runtime offset relative to that global (rather than an absolute address).
// This is the normal shape for runtime-generated modules that share a linear
// memory with other modules.
//
//   clang++ -std=c++20 -Iinclude examples/dynamic_link.cpp -o dl && ./dl
//   wasm-tools validate dynamic_link.wasm

#define _CRT_SECURE_NO_WARNINGS

#include <cstdint>
#include <cstdio>
#include <vector>

#include <wasmp/wasmp.hpp>

int main() {
    using namespace wasmp;
    Module mod;

    // Imports must precede same-space definitions.
    GlobalIdx heap = mod.ImportGlobal("env", "__heap_base", ValType::I32, Mutability::Const);
    MemIdx mem = mod.ImportMemory("env", "memory", MemLimits{.min = 1});

    // Place "hello" at __heap_base (offset is a global.get const-expr, not an
    // absolute address).
    const u8 msg[] = {'h', 'e', 'l', 'l', 'o'};
    mod.ActiveData(mem, GlobalGet{heap}, msg);

    // Export a function returning the first byte of that data.
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx first = mod.DeclareFunction(sig);
    mod.Export("first_byte", first);
    mod.SetName(first, "first_byte");

    Function fn(mod, sig);
    fn.GlobalGet(heap);
    fn.I32Load8U();
    mod.DefineFunction(first, fn.Finish());

    std::vector<u8> wasm = mod.Assemble();
    if (FILE* f = std::fopen("dynamic_link.wasm", "wb")) {
        std::fwrite(wasm.data(), 1, wasm.size(), f);
        std::fclose(f);
    }
    std::printf("wrote dynamic_link.wasm (%zu bytes)\n", wasm.size());
    return 0;
}
