// examples/jit_expr.cpp — the headline use case: turn data chosen at runtime
// into a WebAssembly function, then call it. Here the "expression" is a
// polynomial whose coefficients are only known at runtime; we emit a Horner
// evaluator for them, instantiate it in wasmtime, and call it — the same shape
// as a shader/DSL/JIT pipeline.
//
// Requires the wasmtime C API (headers + import lib + wasmtime.dll on PATH).

#include <cstdint>
#include <cstdio>
#include <vector>

#include <wasm.h>
#include <wasmtime.h>

#include <wasmp/wasmp.hpp>
#include <wasmp/interop/wasmtime.hpp>

// Emit  f(x) = c[0]*x^n + c[1]*x^(n-1) + ... + c[n]  via Horner's method.
static std::vector<wasmp::u8> EmitPolynomial(const std::vector<wasmp::f64>& coeffs) {
    using namespace wasmp;
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<f64(f64)>();
    FuncIdx eval = mod.DeclareFunction(sig);
    mod.Export("eval", eval);

    Function fn(mod, sig);
    LocalIdx x = fn.Param(0);
    fn.F64Const(coeffs.empty() ? 0.0 : coeffs[0]);   // acc = c0
    for (size_t i = 1; i < coeffs.size(); ++i) {     // acc = acc*x + ci
        fn.LocalGet(x);
        fn.F64Mul();
        fn.F64Const(coeffs[i]);
        fn.F64Add();
    }
    mod.DefineFunction(eval, fn.Finish());
    return mod.Assemble();
}

int main() {
    // Coefficients "discovered" at runtime: 2x^2 - 3x + 1.
    const std::vector<wasmp::f64> coeffs = {2.0, -3.0, 1.0};
    std::vector<wasmp::u8> wasm = EmitPolynomial(coeffs);

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
    wasmtime_extern_t ext{};
    wasmtime_instance_export_get(ctx, &instance, "eval", 4, &ext);
    wasmtime_func_t eval = ext.of.func;

    std::printf("f(x) = 2x^2 - 3x + 1, JIT-compiled to wasm at runtime:\n");
    for (wasmp::f64 x : {0.0, 1.0, 2.0, 3.0}) {
        wasmp::f64 y = wasmp::wasmtime::TypedCall<wasmp::f64(wasmp::f64)>(ctx, &eval, x);
        std::printf("  f(%.1f) = %.1f\n", x, y);
    }
    return 0;
}
