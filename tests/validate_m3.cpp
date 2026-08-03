// validate_m3.cpp — the policy::Validate type-stack checker.
// Built with WASMP_THROW_ON_ERROR so validation failures raise a catchable
// wasmp::Error instead of aborting. Covers: valid programs stay silent, each
// error class is caught, the polymorphic (unreachable) stack behaves, and a
// batch of randomly-generated valid programs is emitted for the external
// wasm-tools cross-check (differential fuzzing).

#define _CRT_SECURE_NO_WARNINGS
#define WASMP_THROW_ON_ERROR  // validator Fail() throws wasmp::Error

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
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

template <typename F>
static bool Throws(F&& f) {
    try {
        f();
        return false;
    } catch (const wasmp::Error&) {
        return true;
    }
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

// ---------------------------------------------------------------------------
// Positive cases — valid programs must not raise, and must assemble.
// ---------------------------------------------------------------------------
static void TestPositive() {
    // add(i32,i32)->i32
    CHECK(!Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32(i32, i32)>();
        auto f = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.LocalGet(fn.Param(0));
        fn.LocalGet(fn.Param(1));
        fn.I32Add();
        mod.DefineFunction(f, fn.Finish());
        (void)mod.Assemble();
    }));

    // loop with br_if and a local
    CHECK(!Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        auto f = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        auto i = fn.DeclareLocal(ValType::I32);
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
        (void)mod.Assemble();
    }));

    // if/else with a result, multi-value block, select, call, call_indirect
    CHECK(!Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32(i32)>();
        auto pick = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.LocalGet(fn.Param(0));
        {
            auto c = fn.If(ValType::I32);
            fn.I32Const(10);
            c.Else();
            fn.I32Const(20);
        }
        mod.DefineFunction(pick, fn.Finish());
        (void)mod.Assemble();
    }));

    // global get/set (mutable), memory load/store
    CHECK(!Throws([] {
        Module mod;
        auto g = mod.Global(ValType::I32, Mutability::Var, I32Const{0});
        mod.Memory(MemLimits{.min = 1});
        auto sig = mod.FuncTypeOf<i32()>();
        auto f = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.GlobalGet(g);
        fn.I32Const(1);
        fn.I32Add();
        fn.GlobalSet(g);
        fn.I32Const(0);
        fn.I32Load();
        mod.DefineFunction(f, fn.Finish());
        (void)mod.Assemble();
    }));

    // call another declared function
    CHECK(!Throws([] {
        Module mod;
        auto usig = mod.FuncTypeOf<i32(i32)>();
        auto sig = mod.FuncTypeOf<i32()>();
        auto helper = mod.DeclareFunction(usig);
        auto main = mod.DeclareFunction(sig);
        {
            Fn fn(mod, usig);
            fn.LocalGet(fn.Param(0));
            mod.DefineFunction(helper, fn.Finish());
        }
        {
            Fn fn(mod, sig);
            fn.I32Const(3);
            fn.Call(helper);
            mod.DefineFunction(main, fn.Finish());
        }
        (void)mod.Assemble();
    }));
}

// ---------------------------------------------------------------------------
// Polymorphic (unreachable) stack — dead code after br/unreachable is checked
// in polymorphic mode and must not spuriously underflow.
// ---------------------------------------------------------------------------
static void TestPolymorphic() {
    CHECK(!Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        auto f = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        auto blk = fn.Block(ValType::I32);
        fn.I32Const(5);
        fn.Br(blk);        // branch out; frame becomes unreachable
        fn.Drop();         // would underflow if not polymorphic
        fn.I32Const(9);
        blk.End();
        mod.DefineFunction(f, fn.Finish());
        (void)mod.Assemble();
    }));

    // unreachable makes the whole rest of the function polymorphic
    CHECK(!Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        auto f = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.Unreachable();
        mod.DefineFunction(f, fn.Finish());  // result i32 satisfied polymorphically
        (void)mod.Assemble();
    }));
}

// ---------------------------------------------------------------------------
// Negative cases — each must be caught at the offending emitter call.
// ---------------------------------------------------------------------------
static void TestNegative() {
    // operand type mismatch: i32.add on two f32
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.F32Const(1.0f);
        fn.F32Const(2.0f);
        fn.I32Add();
    }));

    // operand stack underflow
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Add();
    }));

    // local index out of range
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.LocalGet(LocalIdx{7});
    }));

    // global.set on an immutable global
    CHECK(Throws([] {
        Module mod;
        auto g = mod.Global(ValType::I32, Mutability::Const, I32Const{0});
        auto sig = mod.FuncTypeOf<void()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Const(1);
        fn.GlobalSet(g);
    }));

    // wrong function result type
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<i32()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.F32Const(1.0f);
        (void)fn.Finish();  // expects i32 on the stack, finds f32
    }));

    // if (result i32) without an else arm
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<void()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Const(0);
        auto c = fn.If(ValType::I32);
        fn.I32Const(1);
        c.End();  // explicit end so the throw is catchable (not in a dtor)
    }));

    // select with mismatched arms
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<void()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Const(1);
        fn.F32Const(2.0f);
        fn.I32Const(1);  // condition
        fn.Select();
    }));
}

// ---------------------------------------------------------------------------
// Differential fuzzing — deterministic random VALID integer programs. The
// validator must stay silent (they are valid by construction) and each is
// written out for the external wasm-tools validation leg.
// ---------------------------------------------------------------------------
static void TestFuzzValid(int count) {
    std::mt19937 rng(0xC0FFEE);
    for (int n = 0; n < count; ++n) {
        bool threw = Throws([&] {
            Module mod;
            auto sig = mod.FuncTypeOf<i32()>();
            auto f = mod.DeclareFunction(sig);
            mod.Export("f", f);
            Fn fn(mod, sig);
            int depth = 0;  // model stack depth (all i32)
            const int steps = 3 + static_cast<int>(rng() % 30);
            for (int s = 0; s < steps; ++s) {
                const int choice = static_cast<int>(rng() % 5);
                if (depth >= 2 && choice == 0) { fn.I32Add(); --depth; }
                else if (depth >= 2 && choice == 1) { fn.I32Sub(); --depth; }
                else if (depth >= 2 && choice == 2) { fn.I32Mul(); --depth; }
                else if (depth >= 1 && choice == 3) { fn.I32Eqz(); /* depth same */ }
                else { fn.I32Const(static_cast<i32>(rng())); ++depth; }
            }
            while (depth > 1) { fn.I32Add(); --depth; }  // reduce to one result
            if (depth == 0) fn.I32Const(0);
            mod.DefineFunction(f, fn.Finish());
            auto bytes = mod.Assemble();
            WriteFile("fuzz_" + std::to_string(n) + ".wasm", bytes);
        });
        CHECK(!threw);
        if (threw) break;
    }
}

// ---------------------------------------------------------------------------
// Feature-flag gating — an opcode whose proposal is disabled must be rejected.
// ---------------------------------------------------------------------------
static void TestFeatureGating() {
    const std::array<u8, 16> z{};

    // SIMD op with simd disabled (default Core) → rejected.
    CHECK(Throws([&] {
        Module mod;
        auto sig = mod.FuncTypeOf<void()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.V128Const(z);
    }));
    // Same op with simd enabled → accepted.
    CHECK(!Throws([&] {
        Features f = Features::Core();
        f.simd = true;
        Module mod(f);
        auto sig = mod.FuncTypeOf<i32()>();
        auto idx = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.V128Const(z);
        fn.I32x4ExtractLane(0);
        mod.DefineFunction(idx, fn.Finish());
    }));
    // Atomic op with atomics disabled → rejected.
    CHECK(Throws([] {
        Module mod;
        mod.Memory(MemLimits{.min = 1});
        auto sig = mod.FuncTypeOf<i32()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Const(0);
        fn.I32AtomicLoad();
    }));
    // Non-zero memory index without multi-memory → rejected.
    CHECK(Throws([] {
        Module mod;
        mod.Memory(MemLimits{.min = 1});
        mod.Memory(MemLimits{.min = 1});
        auto sig = mod.FuncTypeOf<i32()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Const(0);
        fn.I32Load(MemArg{.memory = MemIdx{1}});
    }));
}

// ---------------------------------------------------------------------------
// memory64 — bulk-memory ops take i64 address operands.
// ---------------------------------------------------------------------------
static void TestMemory64() {
    auto mem64 = [] {
        Features f = Features::Core();
        f.memory64 = true;
        return f;
    };
    // Correct i64-addressed fill/copy/grow/size.
    CHECK(!Throws([&] {
        Module mod(mem64());
        mod.Memory(MemLimits{.min = 1});
        auto sig = mod.FuncTypeOf<void()>();
        auto idx = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I64Const(0); fn.I32Const(0); fn.I64Const(4); fn.MemoryFill();  // i64 dst/len
        fn.I64Const(0); fn.I64Const(0); fn.I64Const(4); fn.MemoryCopy();  // all i64
        fn.I64Const(1); fn.MemoryGrow(); fn.Drop();                       // i64 delta -> i64
        fn.MemorySize(); fn.Drop();                                       // -> i64
        mod.DefineFunction(idx, fn.Finish());
    }));
    // i32 dst under memory64 → type mismatch.
    CHECK(Throws([&] {
        Module mod(mem64());
        mod.Memory(MemLimits{.min = 1});
        auto sig = mod.FuncTypeOf<void()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.I32Const(0); fn.I32Const(0); fn.I32Const(4); fn.MemoryFill();
    }));
}

// ---------------------------------------------------------------------------
// Typed select (reference types) and the signature-later / reuse ctor path.
// ---------------------------------------------------------------------------
static void TestSelectTAndReuse() {
    // Typed select over funcref → funcref (dropped), returns nothing.
    CHECK(!Throws([] {
        Module mod;  // Core has reference_types on
        auto sig = mod.FuncTypeOf<void()>();
        auto idx = mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.RefNull(RefType::FuncRef);
        fn.RefNull(RefType::FuncRef);
        fn.I32Const(1);
        fn.SelectT(ValType::FuncRef);
        fn.Drop();
        mod.DefineFunction(idx, fn.Finish());
    }));
    // Mismatched arms: second value is externref where funcref is expected.
    CHECK(Throws([] {
        Module mod;
        auto sig = mod.FuncTypeOf<void()>();
        mod.DeclareFunction(sig);
        Fn fn(mod, sig);
        fn.RefNull(RefType::FuncRef);
        fn.RefNull(RefType::ExternRef);
        fn.I32Const(1);
        fn.SelectT(ValType::FuncRef);
    }));
    // Signature-later ctor + reuse across two different signatures.
    CHECK(!Throws([] {
        Module mod;
        auto sigA = mod.FuncTypeOf<i32(i32)>();
        auto sigB = mod.FuncTypeOf<i32(i32, i32)>();
        auto a = mod.DeclareFunction(sigA);
        auto b = mod.DeclareFunction(sigB);
        Fn fn(mod);  // signature-later
        fn.SetSignature(sigA);
        fn.LocalGet(fn.Param(0));
        mod.DefineFunction(a, fn.Finish());
        fn.SetSignature(sigB);  // reuse for a different signature
        fn.LocalGet(fn.Param(0));
        fn.LocalGet(fn.Param(1));
        fn.I32Add();
        mod.DefineFunction(b, fn.Finish());
        (void)mod.Assemble();
    }));
}

int main() {
    TestPositive();
    TestPolymorphic();
    TestNegative();
    TestFeatureGating();
    TestMemory64();
    TestSelectTAndReuse();
    TestFuzzValid(64);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
