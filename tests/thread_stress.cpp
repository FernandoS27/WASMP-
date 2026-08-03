// thread_stress.cpp — the phase-2 parallel-emission contract (DESIGN.md §2.1).
// After all indices are declared (phase 1), function bodies are emitted on many
// threads concurrently; only the (serial) DefineFunction mutates the Module.
// Verifies: no data races (run under TSan/ASan) and that parallel emission
// yields byte-identical output to serial emission (determinism).

#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include <wasmp/wasmp.hpp>

using namespace wasmp;

static constexpr int kFuncs = 256;

// A simple valid body whose shape varies with the index (so it isn't trivially
// identical work): computes a small polynomial in its i32 param.
template <typename EmitterPolicy>
static FunctionBody BuildBody(Module& mod, TypeIdx sig, int idx) {
    FunctionEmitter<EmitterPolicy> fn(mod, sig);
    fn.LocalGet(fn.Param(0));
    for (int k = 0; k < (idx % 8) + 1; ++k) {
        fn.I32Const(idx + k);
        fn.I32Add();
    }
    return fn.Finish();
}

static std::vector<u8> BuildSerial() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32)>();
    std::vector<FuncIdx> ids;
    for (int i = 0; i < kFuncs; ++i) {
        FuncIdx f = mod.DeclareFunction(sig);
        mod.Export("f" + std::to_string(i), f);
        ids.push_back(f);
    }
    for (int i = 0; i < kFuncs; ++i)
        mod.DefineFunction(ids[i], BuildBody<policy::Validate>(mod, sig, i));
    return mod.Assemble();
}

static std::vector<u8> BuildParallel() {
    Module mod;
    TypeIdx sig = mod.FuncTypeOf<i32(i32)>();  // phase 1: declare all
    std::vector<FuncIdx> ids;
    for (int i = 0; i < kFuncs; ++i) {
        FuncIdx f = mod.DeclareFunction(sig);
        mod.Export("f" + std::to_string(i), f);
        ids.push_back(f);
    }

    // phase 2: emit bodies concurrently (Module read-only here), each thread
    // writing only its own result slot.
    std::vector<std::optional<FunctionBody>> bodies(kFuncs);
    const unsigned nthreads = std::max(2u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            for (int i = static_cast<int>(t); i < kFuncs; i += static_cast<int>(nthreads))
                bodies[i] = BuildBody<policy::Validate>(mod, sig, i);
        });
    }
    for (auto& th : pool) th.join();

    // Serial attach + assemble.
    for (int i = 0; i < kFuncs; ++i)
        mod.DefineFunction(ids[i], std::move(*bodies[i]));
    return mod.Assemble();
}

int main() {
    auto serial = BuildSerial();
    auto parallel = BuildParallel();
    const bool same = serial == parallel;
    std::printf("serial=%zu bytes parallel=%zu bytes  %s\n", serial.size(),
                parallel.size(), same ? "IDENTICAL" : "DIFFER");
    return same && !serial.empty() ? 0 : 1;
}
