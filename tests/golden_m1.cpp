// golden_m1.cpp — M1 module-assembly tests.
// Exact-byte golden for the canonical "add" module, determinism, and
// structural builds (control flow, memory/data, globals) that are byte-dumped
// to disk for the optional external `wasm-tools validate` CI leg.

#define _CRT_SECURE_NO_WARNINGS  // test uses std::getenv/std::fopen on MSVC UCRT

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

static void DumpHex(const char* label, const std::vector<u8>& b) {
    std::fprintf(stderr, "%s (%zu bytes):\n", label, b.size());
    for (size_t i = 0; i < b.size(); ++i)
        std::fprintf(stderr, "%02X%s", b[i], (i % 16 == 15) ? "\n" : " ");
    std::fprintf(stderr, "\n");
}

static bool BytesEq(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// Build: (func (export "add") (param i32 i32) (result i32)
//           local.get 0  local.get 1  i32.add)
static std::vector<u8> BuildAdd() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32, i32)>();
    FuncIdx add = mod.DeclareFunction(sig);
    mod.Export("add", add);

    FunctionEmitter<policy::Trust> fn(mod, sig);
    fn.LocalGet(fn.Param(0));
    fn.LocalGet(fn.Param(1));
    fn.I32Add();
    mod.DefineFunction(add, fn.Finish());
    return mod.Assemble();
}

static void TestAddGolden() {
    const std::vector<u8> expected = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,  // magic + version
        0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,  // type
        0x03, 0x02, 0x01, 0x00,                          // func
        0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,  // export "add"
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B,  // code
    };
    auto got = BuildAdd();
    if (!BytesEq(got, expected)) {
        DumpHex("expected", expected);
        DumpHex("got", got);
    }
    CHECK(BytesEq(got, expected));
}

static void TestDeterminism() {
    // Same API calls must yield byte-identical modules (content-addressable).
    CHECK(BytesEq(BuildAdd(), BuildAdd()));
}

static void TestTypeInterning() {
    Module mod;
    TypeIdx a = mod.FuncType({ValType::F32, ValType::F32}, {ValType::F32});
    TypeIdx b = mod.FuncTypeOf<f32(f32, f32)>();
    TypeIdx c = mod.FuncType({ValType::I32}, {});
    CHECK(Raw(a) == Raw(b));   // deduplicated
    CHECK(Raw(c) != Raw(a));   // distinct signature
    CHECK(Raw(c) == 1);        // second distinct type -> index 1 (deterministic)
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

// Control flow: a loop that counts a local up to 10.
static std::vector<u8> BuildLoop() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx count = mod.DeclareFunction(sig);
    mod.Export("count", count);

    FunctionEmitter<policy::Trust> fn(mod, sig);
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
    }  // lp.End() via RAII
    fn.LocalGet(i);
    mod.DefineFunction(count, fn.Finish());
    return mod.Assemble();
}

// if/else producing a value, multi-value-free (single result).
static std::vector<u8> BuildIfElse() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32)>();
    FuncIdx pick = mod.DeclareFunction(sig);
    mod.Export("pick", pick);

    FunctionEmitter<policy::Trust> fn(mod, sig);
    fn.LocalGet(fn.Param(0));
    {
        auto c = fn.If(ValType::I32);
        fn.I32Const(10);
        c.Else();
        fn.I32Const(20);
    }
    mod.DefineFunction(pick, fn.Finish());
    return mod.Assemble();
}

// Memory + active data + a load.
static std::vector<u8> BuildMemoryData() {
    Module mod;
    MemIdx mem = mod.Memory(MemLimits{.min = 1});
    mod.Export("mem", mem);
    const u8 hi[] = {'h', 'i'};
    mod.ActiveData(mem, I32Const{0}, hi);

    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx load = mod.DeclareFunction(sig);
    mod.Export("load", load);
    FunctionEmitter<policy::Trust> fn(mod, sig);
    fn.I32Const(0);
    fn.I32Load();  // natural alignment default
    mod.DefineFunction(load, fn.Finish());
    return mod.Assemble();
}

// Mutable global, get/set, and a name section.
static std::vector<u8> BuildGlobalNamed() {
    Module mod;
    GlobalIdx g = mod.Global(ValType::I32, Mutability::Var, I32Const{7});
    mod.Export("g", g);

    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx bump = mod.DeclareFunction(sig);
    mod.Export("bump", bump);
    mod.SetName(bump, "bump");
    mod.SetModuleName("golden");

    FunctionEmitter<policy::Trust> fn(mod, sig);
    fn.GlobalGet(g);
    fn.I32Const(1);
    fn.I32Add();
    fn.GlobalSet(g);
    fn.GlobalGet(g);
    mod.DefineFunction(bump, fn.Finish());
    return mod.Assemble();
}

// Bulk memory + non-trapping conversion: exercises the prefixed-opcode (0xFC)
// emit path, the MEMINIT immediate shape, passive data, and the DataCount
// section — none of which the other cases touch.
static std::vector<u8> BuildBulk() {
    Module mod;
    mod.Memory(MemLimits{.min = 1});
    const u8 abc[] = {'a', 'b', 'c'};
    DataIdx d = mod.PassiveData(abc);

    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("f", f);

    FunctionEmitter<policy::Trust> fn(mod, sig);
    fn.I32Const(0);   // dest
    fn.I32Const(0);   // src
    fn.I32Const(3);   // len
    fn.MemoryInit(d);
    fn.DataDrop(d);
    fn.F32Const(1.5f);
    fn.I32TruncSatF32S();  // 0xFC 0x00 — prefixed opcode
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

static void TestStructuralBuilds() {
    struct Case { const char* file; std::vector<u8> (*build)(); };
    const Case cases[] = {
        {"add.wasm", BuildAdd},
        {"loop.wasm", BuildLoop},
        {"ifelse.wasm", BuildIfElse},
        {"memory_data.wasm", BuildMemoryData},
        {"global_named.wasm", BuildGlobalNamed},
        {"bulk.wasm", BuildBulk},
    };
    for (const auto& c : cases) {
        auto bytes = c.build();
        // Every module starts with the magic + version and is non-trivial.
        CHECK(bytes.size() >= 8);
        CHECK(bytes[0] == 0x00 && bytes[1] == 0x61 && bytes[2] == 0x73 && bytes[3] == 0x6D);
        CHECK(bytes[4] == 0x01 && bytes[5] == 0x00 && bytes[6] == 0x00 && bytes[7] == 0x00);
        // Determinism per case.
        CHECK(BytesEq(bytes, c.build()));
        WriteFile(c.file, bytes);  // for external validation when enabled
    }
}

static void TestTryAssembleError() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<void()>();
    mod.DeclareFunction(sig);  // declared, never defined
    auto r = mod.TryAssemble();
    CHECK(!r);  // should report the missing body, not crash
    CHECK(r.error.has_value());
}

int main() {
    TestAddGolden();
    TestDeterminism();
    TestTypeInterning();
    TestStructuralBuilds();
    TestTryAssembleError();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
