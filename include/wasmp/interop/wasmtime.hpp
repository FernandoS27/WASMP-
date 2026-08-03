// wasmp — interop/wasmtime.hpp  (optional adapter, not part of the core)
// Derives wasmtime's C-API call machinery from the SAME compile-time signature
// used to build the module, so a function type is transcribed exactly once.
//
// The signature->valkind mapping (ValKindOf / ParamKinds / ResultKinds) is
// self-contained and always available. TypedCall and the marshalling helpers
// need the wasmtime C API and are compiled only when <wasmtime.h> has been
// included first (detected via its WASMTIME_VAL_H guard).
//
// See DESIGN.md §8.
#ifndef WASMP_INTEROP_WASMTIME_HPP
#define WASMP_INTEROP_WASMTIME_HPP

#include <array>
#include <cstdint>
#include <type_traits>

#include "../host_traits.hpp"

namespace wasmp::wasmtime {

// wasmtime_valkind_t values (WASMTIME_I32=0, I64=1, F32=2, F64=3). Kept as a
// plain u8 so this part compiles with no SDK present; asserted equal to
// the SDK macros below when the SDK is available.
constexpr u8 ValKindOf(ValType v) noexcept {
    switch (v) {
        case ValType::I32:  return 0;
        case ValType::I64:  return 1;
        case ValType::F32:  return 2;
        case ValType::F64:  return 3;
        case ValType::V128: return 4;
        default:            return 0xFF;  // refs: extended handling later
    }
}

template <typename Sig>
constexpr auto ParamKinds() noexcept {
    constexpr auto p = SignatureTraits<Sig>::params;
    std::array<u8, p.size()> k{};
    for (size_t i = 0; i < p.size(); ++i) k[i] = ValKindOf(p[i]);
    return k;
}

template <typename Sig>
constexpr auto ResultKinds() noexcept {
    constexpr auto r = SignatureTraits<Sig>::results;
    std::array<u8, r.size()> k{};
    for (size_t i = 0; i < r.size(); ++i) k[i] = ValKindOf(r[i]);
    return k;
}

namespace detail {

template <typename Sig> struct FnTraits;
template <typename R, typename... P>
struct FnTraits<R(P...)> {
    using Ret = R;
    static constexpr size_t arity = sizeof...(P);
};

template <typename T> struct is_tuple : std::false_type {};
template <typename... Ts> struct is_tuple<std::tuple<Ts...>> : std::true_type {};

template <typename R>
constexpr size_t ResultCount() {
    if constexpr (std::is_void_v<R>) return 0;
    else if constexpr (is_tuple<R>::value) return std::tuple_size_v<R>;
    else return 1;
}

}  // namespace detail

}  // namespace wasmp::wasmtime

// ---------------------------------------------------------------------------
// SDK-dependent part: only when <wasmtime.h> was included before this header.
// ---------------------------------------------------------------------------
#if defined(WASMTIME_VAL_H)

#include <array>
#include <functional>
#include <string>
#include <tuple>
#include <utility>

#include "../common.hpp"  // wasmp::detail::ReportError

namespace wasmp::wasmtime {

// Marshal a scalar C++ argument into a wasmtime_val_t (kind from the C++ type).
template <typename A>
inline wasmtime_val_t MakeVal(A a) {
    wasmtime_val_t v{};
    using T = std::remove_cvref_t<A>;
    if constexpr (std::is_same_v<T, i32> || std::is_same_v<T, u32>) {
        v.kind = WASMTIME_I32; v.of.i32 = static_cast<i32>(a);
    } else if constexpr (std::is_same_v<T, i64> || std::is_same_v<T, u64>) {
        v.kind = WASMTIME_I64; v.of.i64 = static_cast<i64>(a);
    } else if constexpr (std::is_same_v<T, f32>) {
        v.kind = WASMTIME_F32; v.of.f32 = a;
    } else if constexpr (std::is_same_v<T, f64>) {
        v.kind = WASMTIME_F64; v.of.f64 = a;
    } else if constexpr (std::is_same_v<T, ::wasmp::v128>) {
        v.kind = WASMTIME_V128;
        for (int i = 0; i < 16; ++i) v.of.v128[i] = a.bytes[i];
    } else {
        static_assert(sizeof(T) == 0, "unsupported wasmtime argument type");
    }
    return v;
}

template <typename R>
inline R ReadVal(const wasmtime_val_t& v) {
    using T = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<T, i32> || std::is_same_v<T, u32>)
        return static_cast<T>(v.of.i32);
    else if constexpr (std::is_same_v<T, i64> || std::is_same_v<T, u64>)
        return static_cast<T>(v.of.i64);
    else if constexpr (std::is_same_v<T, f32>)
        return v.of.f32;
    else if constexpr (std::is_same_v<T, f64>)
        return v.of.f64;
    else if constexpr (std::is_same_v<T, ::wasmp::v128>) {
        ::wasmp::v128 r;
        for (int i = 0; i < 16; ++i) r.bytes[i] = v.of.v128[i];
        return r;
    } else
        static_assert(sizeof(T) == 0, "unsupported wasmtime result type");
}

[[noreturn]] inline void FailError(wasmtime_error_t* err) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string m(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    ::wasmp::detail::ReportError(m);
}
[[noreturn]] inline void FailTrap(wasm_trap_t* trap) {
    wasm_message_t msg;
    wasm_trap_message(trap, &msg);
    std::string m(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasm_trap_delete(trap);
    ::wasmp::detail::ReportError(m);
}

// Call an export with compile-time-checked argument types and return the
// result marshalled back to the C++ type(s) declared in Sig (void, a scalar,
// or std::tuple<...> for multi-value). Reports (does not return) on error/trap.
template <typename Sig, typename... Args>
typename detail::FnTraits<Sig>::Ret
TypedCall(wasmtime_context_t* ctx, const wasmtime_func_t* func, Args... args) {
    using Ret = typename detail::FnTraits<Sig>::Ret;
    static_assert(sizeof...(Args) == detail::FnTraits<Sig>::arity,
                  "argument count does not match the signature");

    constexpr size_t nargs = sizeof...(Args);
    constexpr size_t nres = detail::ResultCount<Ret>();
    std::array<wasmtime_val_t, (nargs == 0 ? 1 : nargs)> in{};
    std::array<wasmtime_val_t, (nres == 0 ? 1 : nres)> out{};

    size_t i = 0;
    ((in[i++] = MakeVal(args)), ...);

    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call(ctx, func, in.data(), nargs, out.data(), nres, &trap);
    if (err) FailError(err);
    if (trap) FailTrap(trap);

    if constexpr (std::is_void_v<Ret>) {
        return;
    } else if constexpr (detail::is_tuple<Ret>::value) {
        return [&]<size_t... I>(std::index_sequence<I...>) {
            return Ret{ReadVal<std::tuple_element_t<I, Ret>>(out[I])...};
        }(std::make_index_sequence<std::tuple_size_v<Ret>>{});
    } else {
        return ReadVal<Ret>(out[0]);
    }
}

// --- Native callbacks the JITed module can call back into (wasm -> host) ----
// A host function whose type is derived from the SAME C++ signature the module
// declared its import with. The result is a wasmtime_extern_t to pass in the
// imports array at instantiation.
namespace detail {

template <typename Sig> struct HostTramp;
template <typename R, typename... Args>
struct HostTramp<R(Args...)> {
    using Fn = std::function<R(Args...)>;
    static wasm_trap_t* Call(void* env, wasmtime_caller_t*, const wasmtime_val_t* args,
                             size_t, wasmtime_val_t* results, size_t) {
        auto& fn = *static_cast<Fn*>(env);
        [&]<size_t... I>(std::index_sequence<I...>) {
            if constexpr (std::is_void_v<R>) {
                (void)results;
                fn(ReadVal<Args>(args[I])...);
            } else {
                results[0] = MakeVal(fn(ReadVal<Args>(args[I])...));
            }
        }(std::index_sequence_for<Args...>{});
        return nullptr;  // no trap
    }
    static void Finalize(void* env) { delete static_cast<Fn*>(env); }
};

inline void FillValtypeVec(wasm_valtype_vec_t* vec, std::span<const u8> kinds) {
    std::array<wasm_valtype_t*, 16> buf{};
    for (size_t i = 0; i < kinds.size(); ++i)
        buf[i] = wasm_valtype_new(static_cast<wasm_valkind_t>(kinds[i]));
    wasm_valtype_vec_new(vec, kinds.size(), buf.data());
}

}  // namespace detail

template <typename Sig, typename F>
inline wasmtime_extern_t MakeHostFunc(wasmtime_context_t* ctx, F&& f) {
    constexpr auto pk = ParamKinds<Sig>();
    constexpr auto rk = ResultKinds<Sig>();
    wasm_valtype_vec_t params, results;
    detail::FillValtypeVec(&params, {pk.data(), pk.size()});
    detail::FillValtypeVec(&results, {rk.data(), rk.size()});
    wasm_functype_t* ft = wasm_functype_new(&params, &results);

    using Tramp = detail::HostTramp<Sig>;
    auto* env = new typename Tramp::Fn(std::forward<F>(f));
    wasmtime_func_t func;
    wasmtime_func_new(ctx, ft, &Tramp::Call, env, &Tramp::Finalize, &func);
    wasm_functype_delete(ft);

    wasmtime_extern_t ext{};
    ext.kind = WASMTIME_EXTERN_FUNC;
    ext.of.func = func;
    return ext;
}

// Compile-time cross-check that our standalone valkind numbering matches the
// SDK's macros (so ParamKinds/ResultKinds are usable directly with the C API).
static_assert(ValKindOf(ValType::I32) == WASMTIME_I32);
static_assert(ValKindOf(ValType::I64) == WASMTIME_I64);
static_assert(ValKindOf(ValType::F32) == WASMTIME_F32);
static_assert(ValKindOf(ValType::F64) == WASMTIME_F64);
static_assert(ValKindOf(ValType::V128) == WASMTIME_V128);

}  // namespace wasmp::wasmtime

#endif  // WASMTIME_VAL_H

#endif  // WASMP_INTEROP_WASMTIME_HPP
