// golden_m4.cpp — the M4 proposals: bulk memory, tail calls, multi-memory,
// memory64 (load/store), SIMD, and atomics. Each module is built under the
// validating policy (so the M4 validator paths are exercised), then written
// out for the external `wasm-tools validate --features=all` leg.

#define _CRT_SECURE_NO_WARNINGS

#include <array>
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
using Fn = FunctionEmitter<policy::Validate>;

static bool BytesEq(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
static void WriteFile(const std::string& name, const std::vector<u8>& bytes) {
    if (const char* dir = std::getenv("WASMP_GOLDEN_DIR")) {
        std::string full = std::string(dir) + "/" + name;
        if (FILE* f = std::fopen(full.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
}

// Bulk memory: memory.init/copy/fill, data.drop, table.init/copy/grow/size/fill.
static std::vector<u8> BuildBulk() {
    Module mod;
    mod.Memory(MemLimits{.min = 1});
    TableIdx tab = mod.Table(RefType::FuncRef, TableLimits{.min = 8});
    const u8 blob[] = {1, 2, 3};
    DataIdx d = mod.PassiveData(blob);
    TypeIdx sig = mod.FuncTypeOf<void()>();
    FuncIdx run = mod.DeclareFunction(sig);
    ElemIdx e = mod.PassiveElem(RefType::FuncRef, {run});
    mod.Export("run", run);

    Fn fn(mod, sig);
    fn.I32Const(0); fn.I32Const(0); fn.I32Const(3); fn.MemoryInit(d);
    fn.DataDrop(d);
    fn.I32Const(0); fn.I32Const(8); fn.I32Const(3); fn.MemoryCopy();
    fn.I32Const(0); fn.I32Const(0); fn.I32Const(4); fn.MemoryFill();
    fn.I32Const(0); fn.I32Const(0); fn.I32Const(1); fn.TableInit(e, tab);
    fn.ElemDrop(e);
    fn.I32Const(0); fn.I32Const(0); fn.I32Const(1); fn.TableCopy(tab, tab);
    fn.RefNull(RefType::FuncRef); fn.I32Const(1); fn.TableGrow(tab); fn.Drop();
    fn.TableSize(tab); fn.Drop();
    fn.I32Const(0); fn.RefNull(RefType::FuncRef); fn.I32Const(1); fn.TableFill(tab);
    mod.DefineFunction(run, fn.Finish());
    return mod.Assemble();
}

// Tail calls: return_call and return_call_indirect.
static std::vector<u8> BuildTailCall() {
    Features feat = Features::Core();
    feat.tail_call = true;
    Module mod(feat);
    TypeIdx sig = mod.FuncTypeOf<i32(i32)>();
    TableIdx tab = mod.Table(RefType::FuncRef, TableLimits{.min = 1});
    FuncIdx helper = mod.DeclareFunction(sig);
    FuncIdx direct = mod.DeclareFunction(sig);
    FuncIdx indirect = mod.DeclareFunction(sig);
    mod.Export("direct", direct);
    mod.Export("indirect", indirect);
    { Fn fn(mod, sig); fn.LocalGet(fn.Param(0)); mod.DefineFunction(helper, fn.Finish()); }
    { Fn fn(mod, sig); fn.LocalGet(fn.Param(0)); fn.ReturnCall(helper);
      mod.DefineFunction(direct, fn.Finish()); }
    { Fn fn(mod, sig); fn.LocalGet(fn.Param(0)); fn.I32Const(0);
      fn.ReturnCallIndirect(sig, tab); mod.DefineFunction(indirect, fn.Finish()); }
    (void)tab;
    return mod.Assemble();
}

// Multi-memory: two memories, load from memory 1.
static std::vector<u8> BuildMultiMemory() {
    Features feat = Features::Core();
    feat.multi_memory = true;
    Module mod(feat);
    mod.Memory(MemLimits{.min = 1});
    MemIdx m1 = mod.Memory(MemLimits{.min = 1});
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("load1", f);
    Fn fn(mod, sig);
    fn.I32Const(0);
    fn.I32Load(MemArg{.memory = m1});  // read from the second memory
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// Memory64: i64-addressed load/store (validator expects i64 addresses here).
static std::vector<u8> BuildMemory64() {
    Features feat = Features::Core();
    feat.memory64 = true;
    Module mod(feat);
    mod.Memory(MemLimits{.min = 1});
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("m64", f);
    Fn fn(mod, sig);
    fn.I64Const(8); fn.I32Const(42); fn.I32Store();  // addr is i64
    fn.I64Const(8); fn.I32Load();                    // addr is i64 -> i32
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// SIMD: const/splat/shuffle/extract, and v128 load/store + load/store lane.
static std::vector<u8> BuildSimd() {
    Features feat = Features::Core();
    feat.simd = true;
    Module mod(feat);
    mod.Memory(MemLimits{.min = 1});
    std::array<u8, 16> c{};
    c.fill(7);
    std::array<u8, 16> shuf{};
    for (u8 i = 0; i < 16; ++i) shuf[i] = i;

    // Phase 1: declare both functions before any body is attached (§2.1).
    TypeIdx isig = mod.FuncTypeOf<i32()>();
    TypeIdx vsig = mod.FuncTypeOf<void()>();
    FuncIdx lanes = mod.DeclareFunction(isig);
    FuncIdx mem = mod.DeclareFunction(vsig);
    mod.Export("lanes", lanes);
    mod.Export("simdmem", mem);
    {
        Fn fn(mod, isig);
        fn.I32Const(3); fn.I8x16Splat();  // [v128]
        fn.V128Const(c);                  // [v128, v128]
        fn.I8x16Shuffle(shuf);            // [v128]
        fn.V128Const(c);                  // [v128, v128]
        fn.I32x4Add();                    // [v128]
        fn.I32x4ExtractLane(0);           // [i32]
        mod.DefineFunction(lanes, fn.Finish());
    }
    {
        Fn fn(mod, vsig);
        fn.I32Const(16); fn.I32Const(0); fn.V128Load(); fn.V128Store();
        fn.I32Const(0); fn.V128Const(c); fn.V128Load8Lane(MemArg{}, 0); fn.Drop();
        fn.I32Const(0); fn.V128Const(c); fn.V128Store8Lane(MemArg{}, 0);
        mod.DefineFunction(mem, fn.Finish());
    }
    return mod.Assemble();
}

// Atomics: shared memory, atomic load/store/rmw/cmpxchg/notify, fence.
static std::vector<u8> BuildAtomics() {
    Features feat = Features::Core();
    feat.atomics = true;
    Module mod(feat);
    mod.Memory(MemLimits{.min = 1, .max = 1, .shared = true});
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("atomics", f);
    Fn fn(mod, sig);
    fn.I32Const(0); fn.I32Const(5); fn.I32AtomicStore();
    fn.I32Const(0); fn.I32Const(1); fn.I32AtomicRmwAdd(); fn.Drop();
    fn.I32Const(0); fn.I32Const(1); fn.I32Const(2); fn.I32AtomicRmwCmpxchg(); fn.Drop();
    fn.I32Const(0); fn.I32Const(1); fn.MemoryAtomicNotify(); fn.Drop();
    fn.AtomicFence();
    fn.I32Const(0); fn.I32AtomicLoad();
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// Typed select (reference types): choose between two funcrefs, then drop it.
static std::vector<u8> BuildSelectT() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("pick", f);  // exported → ref.func target is "declared"
    Fn fn(mod, sig);
    fn.RefNull(RefType::FuncRef);
    fn.RefFunc(f);
    fn.I32Const(1);
    fn.SelectT(ValType::FuncRef);  // -> funcref
    fn.Drop();
    fn.I32Const(0);
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

static void TestBuilds() {
    struct Case { const char* file; std::vector<u8> (*build)(); };
    const Case cases[] = {
        {"bulk.wasm", BuildBulk},
        {"tailcall.wasm", BuildTailCall},
        {"multimem.wasm", BuildMultiMemory},
        {"memory64.wasm", BuildMemory64},
        {"simd.wasm", BuildSimd},
        {"atomics.wasm", BuildAtomics},
        {"select_t.wasm", BuildSelectT},
    };
    for (const auto& c : cases) {
        auto bytes = c.build();
        CHECK(bytes.size() >= 8);
        CHECK(bytes[0] == 0x00 && bytes[1] == 0x61);
        CHECK(BytesEq(bytes, c.build()));  // determinism
        WriteFile(c.file, bytes);
    }
}

int main() {
    TestBuilds();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
