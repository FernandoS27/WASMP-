// exec_wasmtime.cpp — the M2 end-to-end proof: build modules with wasmp,
// instantiate them in wasmtime, and call the exports through the TypedCall
// adapter using the SAME C++ signature the module was built from.
//
// Requires the wasmtime C API (headers + import lib + wasmtime.dll on PATH).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include <wasm.h>
#include <wasmtime.h>

#include <wasmp/wasmp.hpp>
#include <wasmp/interop/wasmtime.hpp>

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

// Minimal instantiation wrapper.
struct Instance {
    wasm_engine_t* engine = nullptr;
    wasmtime_store_t* store = nullptr;
    wasmtime_context_t* ctx = nullptr;
    wasmtime_module_t* module = nullptr;
    wasmtime_instance_t instance{};

    explicit Instance(const std::vector<u8>& wasm) {
        engine = wasm_engine_new();
        store = wasmtime_store_new(engine, nullptr, nullptr);
        ctx = wasmtime_store_context(store);
        wasmtime_error_t* err = wasmtime_module_new(engine, wasm.data(), wasm.size(), &module);
        if (err) Die("module_new", err, nullptr);
        wasm_trap_t* trap = nullptr;
        err = wasmtime_instance_new(ctx, module, nullptr, 0, &instance, &trap);
        if (err || trap) Die("instance_new", err, trap);
    }

    wasmtime_func_t Func(const char* name) {
        wasmtime_extern_t ext{};
        bool ok = wasmtime_instance_export_get(ctx, &instance, name, std::strlen(name), &ext);
        CHECK(ok && ext.kind == WASMTIME_EXTERN_FUNC);
        return ext.of.func;
    }

    static void Die(const char* what, wasmtime_error_t* err, wasm_trap_t* trap) {
        std::fprintf(stderr, "wasmtime %s failed\n", what);
        if (err) wasmtime_error_delete(err);
        if (trap) wasm_trap_delete(trap);
        ++g_failures;
    }
};

static std::vector<u8> BuildAdd() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32, i32)>();
    FuncIdx add = mod.DeclareFunction(sig);
    mod.Export("add", add);
    Function fn(mod, sig);
    fn.LocalGet(fn.Param(0));
    fn.LocalGet(fn.Param(1));
    fn.I32Add();
    mod.DefineFunction(add, fn.Finish());
    return mod.Assemble();
}

static std::vector<u8> BuildFMul() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<f32(f32, f32)>();
    FuncIdx m = mod.DeclareFunction(sig);
    mod.Export("fmul", m);
    Function fn(mod, sig);
    fn.LocalGet(fn.Param(0));
    fn.LocalGet(fn.Param(1));
    fn.F32Mul();
    mod.DefineFunction(m, fn.Finish());
    return mod.Assemble();
}

// (param i64) (result i32 i32): low and high 32 bits — a multi-value export.
static std::vector<u8> BuildSplit() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<std::tuple<i32, i32>(i64)>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("split", f);
    Function fn(mod, sig);
    LocalIdx x = fn.Param(0);
    fn.LocalGet(x);
    fn.I32WrapI64();
    fn.LocalGet(x);
    fn.I64Const(32);
    fn.I64ShrU();
    fn.I32WrapI64();
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

// M4 execution proof: (result i32) 100 splat + 23 splat, add, extract lane 2.
static std::vector<u8> BuildSimdSum() {
    Features feat = Features::Core();
    feat.simd = true;
    Module mod(feat);
    TypeIdx sig = mod.FuncTypeOf<i32()>();
    FuncIdx f = mod.DeclareFunction(sig);
    mod.Export("simd_sum", f);
    Function fn(mod, sig);
    fn.I32Const(100); fn.I32x4Splat();
    fn.I32Const(23); fn.I32x4Splat();
    fn.I32x4Add();
    fn.I32x4ExtractLane(2);
    mod.DefineFunction(f, fn.Finish());
    return mod.Assemble();
}

int main() {
    {
        Instance inst(BuildAdd());
        wasmtime_func_t f = inst.Func("add");
        auto r = wasmtime::TypedCall<i32(i32, i32)>(inst.ctx, &f, 2, 3);
        CHECK(r == 5);
        auto r2 = wasmtime::TypedCall<i32(i32, i32)>(inst.ctx, &f, -10, 7);
        CHECK(r2 == -3);
    }
    {
        Instance inst(BuildFMul());
        wasmtime_func_t f = inst.Func("fmul");
        f32 r = wasmtime::TypedCall<f32(f32, f32)>(inst.ctx, &f, 2.5f, 4.0f);
        CHECK(std::fabs(r - 10.0f) < 1e-6f);
    }
    {
        Instance inst(BuildSplit());
        wasmtime_func_t f = inst.Func("split");
        const i64 v = (static_cast<i64>(7) << 32) | 5;
        auto [lo, hi] = wasmtime::TypedCall<std::tuple<i32, i32>(i64)>(inst.ctx, &f, v);
        CHECK(lo == 5);
        CHECK(hi == 7);
    }
    {
        Instance inst(BuildSimdSum());
        wasmtime_func_t f = inst.Func("simd_sum");
        i32 r = wasmtime::TypedCall<i32()>(inst.ctx, &f);
        CHECK(r == 123);  // 100 + 23, per i32x4 lane
    }
    {
        // Native callback as an import: the JITed module calls back into C++.
        // Module: import env.cb(i32,i32)->i32; export run(i32,i32)->i32 = cb(a,b).
        Module mod;
        TypeIdx sig = mod.FuncTypeOf<i32(i32, i32)>();
        FuncIdx cb = mod.ImportFunction("env", "cb", sig);
        FuncIdx run = mod.DeclareFunction(sig);
        mod.Export("run", run);
        {
            Function fn(mod, sig);
            fn.LocalGet(fn.Param(0));
            fn.LocalGet(fn.Param(1));
            fn.Call(cb);  // call the host-provided native function
            mod.DefineFunction(run, fn.Finish());
        }
        std::vector<u8> wasm = mod.Assemble();

        wasm_engine_t* engine = wasm_engine_new();
        wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
        wasmtime_context_t* ctx = wasmtime_store_context(store);
        wasmtime_module_t* module = nullptr;
        if (wasmtime_module_new(engine, wasm.data(), wasm.size(), &module)) {
            ++g_failures;
        } else {
            int calls = 0;
            // The native callback — its C++ signature matches the import.
            wasmtime_extern_t imports[1];
            imports[0] = wasmtime::MakeHostFunc<i32(i32, i32)>(
                ctx, [&](i32 a, i32 b) { ++calls; return a * 10 + b; });

            wasmtime_instance_t instance{};
            wasm_trap_t* trap = nullptr;
            if (wasmtime_instance_new(ctx, module, imports, 1, &instance, &trap) || trap) {
                ++g_failures;
            } else {
                wasmtime_extern_t ext{};
                wasmtime_instance_export_get(ctx, &instance, "run", 3, &ext);
                wasmtime_func_t rf = ext.of.func;
                i32 r = wasmtime::TypedCall<i32(i32, i32)>(ctx, &rf, 2, 3);
                CHECK(r == 23);      // native cb(2,3) = 2*10 + 3
                CHECK(calls == 1);   // the native callback actually ran
            }
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
