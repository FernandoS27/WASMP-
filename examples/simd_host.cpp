// examples/simd_host.cpp — pass SIMD (v128) values from the host INTO a JITed
// module and back. wasmp builds a module whose exports take/return v128; the
// host constructs v128 "registers", calls the exports, and reads the lanes.
//
// Requires the wasmtime C API (headers + import lib + wasmtime.dll on PATH).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <wasm.h>
#include <wasmtime.h>

#include <wasmp/wasmp.hpp>
#include <wasmp/interop/wasmtime.hpp>

using namespace wasmp;

static int g_failures = 0;
#define CHECK(c)                                                           \
    do {                                                                   \
        if (!(c)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Build a module with SIMD exports operating on v128 parameters.
static std::vector<u8> BuildModule() {
    Features feat = Features::Core();
    feat.simd = true;
    Module mod(feat);

    TypeIdx binop = mod.FuncTypeOf<v128(v128, v128)>();  // (v128, v128) -> v128
    TypeIdx extract = mod.FuncTypeOf<i32(v128)>();   // (v128) -> i32

    FuncIdx add = mod.DeclareFunction(binop);
    FuncIdx mul = mod.DeclareFunction(binop);
    FuncIdx lane0 = mod.DeclareFunction(extract);
    mod.Export("add_i32x4", add);
    mod.Export("mul_f32x4", mul);
    mod.Export("lane0_i32", lane0);

    {  // add_i32x4(a, b) = a + b   (per i32 lane)
        Function fn(mod, binop);
        fn.LocalGet(fn.Param(0));
        fn.LocalGet(fn.Param(1));
        fn.I32x4Add();
        mod.DefineFunction(add, fn.Finish());
    }
    {  // mul_f32x4(a, b) = a * b   (per f32 lane)
        Function fn(mod, binop);
        fn.LocalGet(fn.Param(0));
        fn.LocalGet(fn.Param(1));
        fn.F32x4Mul();
        mod.DefineFunction(mul, fn.Finish());
    }
    {  // lane0_i32(a) = a[0]       (v128 -> scalar)
        Function fn(mod, extract);
        fn.LocalGet(fn.Param(0));
        fn.I32x4ExtractLane(0);
        mod.DefineFunction(lane0, fn.Finish());
    }
    return mod.Assemble();
}

int main() {
    std::vector<u8> wasm = BuildModule();

    wasm_engine_t* engine = wasm_engine_new();
    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_module_t* module = nullptr;
    if (wasmtime_module_new(engine, wasm.data(), wasm.size(), &module)) {
        std::fprintf(stderr, "module_new failed\n");
        return 1;
    }
    wasmtime_instance_t instance{};
    wasm_trap_t* trap = nullptr;
    if (wasmtime_instance_new(ctx, module, nullptr, 0, &instance, &trap) || trap) {
        std::fprintf(stderr, "instance_new failed\n");
        return 1;
    }
    auto func = [&](const char* name) {
        wasmtime_extern_t e{};
        wasmtime_instance_export_get(ctx, &instance, name, std::strlen(name), &e);
        return e.of.func;
    };
    wasmtime_func_t addF = func("add_i32x4");
    wasmtime_func_t mulF = func("mul_f32x4");
    wasmtime_func_t laneF = func("lane0_i32");

    // --- Construct SIMD registers on the host and call the JITed module. ----
    v128 a = v128::FromI32x4(1, 2, 3, 4);
    v128 b = v128::FromI32x4(10, 20, 30, 40);
    v128 sum = wasmtime::TypedCall<v128(v128, v128)>(ctx, &addF, a, b);
    std::printf("i32x4:  [%d %d %d %d] + [%d %d %d %d] = [%d %d %d %d]\n",
                a.I32(0), a.I32(1), a.I32(2), a.I32(3),
                b.I32(0), b.I32(1), b.I32(2), b.I32(3),
                sum.I32(0), sum.I32(1), sum.I32(2), sum.I32(3));
    CHECK(sum.I32(0) == 11 && sum.I32(1) == 22 && sum.I32(2) == 33 && sum.I32(3) == 44);

    v128 fa = v128::FromF32x4(1.5f, 2.0f, 3.0f, 4.0f);
    v128 fb = v128::FromF32x4(2.0f, 2.0f, 2.0f, 2.0f);
    v128 prod = wasmtime::TypedCall<v128(v128, v128)>(ctx, &mulF, fa, fb);
    std::printf("f32x4:  [%.1f %.1f %.1f %.1f] * [2 2 2 2] = [%.1f %.1f %.1f %.1f]\n",
                fa.F32(0), fa.F32(1), fa.F32(2), fa.F32(3),
                prod.F32(0), prod.F32(1), prod.F32(2), prod.F32(3));
    CHECK(prod.F32(0) == 3.0f && prod.F32(1) == 4.0f && prod.F32(2) == 6.0f && prod.F32(3) == 8.0f);

    i32 l0 = wasmtime::TypedCall<i32(v128)>(ctx, &laneF, a);
    std::printf("lane0:  extract_lane(0) of [1 2 3 4] = %d\n", l0);
    CHECK(l0 == 1);

    std::printf("%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
