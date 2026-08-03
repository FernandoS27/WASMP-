// golden_m2.cpp — M2 module features: multi-value block types, multi-value
// (tuple) function results, and the local-names section. Emits modules for the
// external `wasm-tools validate` leg and checks determinism.

#define _CRT_SECURE_NO_WARNINGS  // std::getenv/std::fopen on MSVC UCRT

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <wasmp/wasmp.hpp>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d  CHECK(%s)\n", __FILE__,      \
                         __LINE__, #cond);                                 \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using namespace wasmp;

static bool BytesEq(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

static void WriteFile(const std::string& path, const std::vector<u8>& bytes) {
    if (const char* dir = std::getenv("WASMP_GOLDEN_DIR")) {
        std::string full = std::string(dir) + "/" + path;
        if (FILE* f = std::fopen(full.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
}

// Multi-value block: a (param i32 i32)(result i32) block that adds its inputs.
// Also exercises the manual End() style rather than RAII.
static std::vector<u8> BuildMultiValueBlock() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32, i32)>();
    TypeIdx blockty = mod.FuncType({ValType::I32, ValType::I32}, {ValType::I32});
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("mv", f);

    FunctionEmitter<policy::Trust> fn(mod, sig);
    fn.LocalGet(fn.Param(0));
    fn.LocalGet(fn.Param(1));
    auto blk = fn.Block(blockty);   // block type via TypeIdx (multi-value)
    fn.I32Add();
    blk.End();                      // manual close
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// Multi-value result: split an i64 into (low32, high32) as a tuple result.
static std::vector<u8> BuildTupleResult() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<std::tuple<i32, i32>(i64)>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("split", f);

    FunctionEmitter<policy::Trust> fn(mod, sig);
    LocalIdx x = fn.Param(0);
    fn.LocalGet(x);
    fn.I32WrapI64();                // low 32
    fn.LocalGet(x);
    fn.I64Const(32);
    fn.I64ShrU();
    fn.I32WrapI64();                // high 32
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// Local names section (subsection 2), plus module/function names.
static std::vector<u8> BuildLocalNames() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32)>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("scale", f);
    mod.SetModuleName("m2");
    mod.SetName(f, "scale");

    FunctionEmitter<policy::Trust> fn(mod, sig);
    LocalIdx factor = fn.Param(0);
    LocalIdx acc = fn.DeclareLocal(ValType::I32);
    mod.SetLocalName(f, factor, "factor");
    mod.SetLocalName(f, acc, "acc");

    fn.LocalGet(factor);
    fn.LocalGet(factor);
    fn.I32Mul();
    fn.LocalSet(acc);
    fn.LocalGet(acc);
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

static void TestBuilds() {
    struct Case { const char* file; std::vector<u8> (*build)(); };
    const Case cases[] = {
        {"mv_block.wasm", BuildMultiValueBlock},
        {"tuple_result.wasm", BuildTupleResult},
        {"local_names.wasm", BuildLocalNames},
    };
    for (const auto& c : cases) {
        auto bytes = c.build();
        CHECK(bytes.size() >= 8);
        CHECK(bytes[0] == 0x00 && bytes[1] == 0x61 && bytes[2] == 0x73 && bytes[3] == 0x6D);
        CHECK(BytesEq(bytes, c.build()));  // determinism
        WriteFile(c.file, bytes);
    }
}

int main() {
    TestBuilds();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
