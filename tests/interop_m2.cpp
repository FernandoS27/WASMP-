// interop_m2.cpp — compile-time verification of the interop adapters' signature
// derivation (no runtime SDK needed; the real execution path is exec_wasmtime).

#include <array>
#include <string_view>
#include <tuple>

#include <wasmp/interop/wamr.hpp>
#include <wasmp/interop/wasmtime.hpp>

using namespace wasmp;

// Compare a null-terminated std::array<char,N> to a string_view.
template <typename A>
constexpr bool SigEq(const A& a, std::string_view s) {
    if (a.size() != s.size() + 1) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (a[i] != s[i]) return false;
    return a[s.size()] == '\0';
}

// --- WAMR signature strings -------------------------------------------------
static_assert(SigEq(wamr::SigString<i32(f32, f32)>(), "(ff)i"));
static_assert(SigEq(wamr::SigString<void(i32)>(), "(i)"));
static_assert(SigEq(wamr::SigString<i64(i64, i32)>(), "(Ii)I"));
static_assert(SigEq(wamr::SigString<f64()>(), "()F"));
static_assert(SigEq(wamr::SigString<void()>(), "()"));
static_assert(SigEq(wamr::SigString<f32(f64, i64)>(), "(FI)f"));

// --- wasmtime valkind mapping (standalone numbering) ------------------------
static_assert(wasmtime::ValKindOf(ValType::I32) == 0);
static_assert(wasmtime::ValKindOf(ValType::I64) == 1);
static_assert(wasmtime::ValKindOf(ValType::F32) == 2);
static_assert(wasmtime::ValKindOf(ValType::F64) == 3);

static_assert(wasmtime::ParamKinds<i32(f32, f64)>() == std::array<u8, 2>{2, 3});
static_assert(wasmtime::ResultKinds<std::tuple<i32, i64>(f32)>() ==
              std::array<u8, 2>{0, 1});
static_assert(wasmtime::ResultKinds<void(i32)>().empty());

int main() { return 0; }
