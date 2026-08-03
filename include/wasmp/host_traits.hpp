// wasmp — host_traits.hpp
// The compile-time bridge between C++ function types and WASM signatures.
// `WasmTypeTraits<T>` maps a scalar C++ type to a ValType; `SignatureTraits<Sig>`
// decomposes `R(Args...)` into param/result ValType arrays (multi-value results
// via std::tuple). Module::FuncTypeOf<Sig>() and the interop adapters are built
// entirely on these — a signature is written once and everything else derives.
//
// Fully realised at the facade stage (pure compile-time). See DESIGN.md §8.
#ifndef WASMP_HOST_TRAITS_HPP
#define WASMP_HOST_TRAITS_HPP

#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>

#include "common.hpp"

namespace wasmp {

// ---------------------------------------------------------------------------
// WasmTypeTraits<T> — customisation point. Specialise it to map a user type
// (e.g. a GuestPtr<T> wrapper) onto a ValType. Unmapped types hard-error.
// ---------------------------------------------------------------------------

template <typename T, typename = void>
struct WasmTypeTraits {
    static_assert(sizeof(T) == 0,
                  "wasmp: no WasmTypeTraits mapping for this C++ type — "
                  "use i32/i64/f32/f64 scalar types or specialise WasmTypeTraits");
};

template <> struct WasmTypeTraits<i32>  { static constexpr ValType type = ValType::I32; };
template <> struct WasmTypeTraits<u32> { static constexpr ValType type = ValType::I32; };
template <> struct WasmTypeTraits<i64>  { static constexpr ValType type = ValType::I64; };
template <> struct WasmTypeTraits<u64> { static constexpr ValType type = ValType::I64; };
template <> struct WasmTypeTraits<f32>    { static constexpr ValType type = ValType::F32; };
template <> struct WasmTypeTraits<f64>   { static constexpr ValType type = ValType::F64; };
template <> struct WasmTypeTraits<v128>     { static constexpr ValType type = ValType::V128; };

template <typename T>
inline constexpr ValType WasmValType = WasmTypeTraits<std::remove_cvref_t<T>>::type;

// ---------------------------------------------------------------------------
// Result-type decomposition. A bare type is a single (or void → zero) result;
// std::tuple<...> expresses multi-value results, which C++ return types cannot.
// ---------------------------------------------------------------------------

namespace detail {

template <typename R>
struct ResultTypes {
    static constexpr std::array<ValType, 1> value{WasmValType<R>};
};
template <>
struct ResultTypes<void> {
    static constexpr std::array<ValType, 0> value{};
};
template <typename... Rs>
struct ResultTypes<std::tuple<Rs...>> {
    static constexpr std::array<ValType, sizeof...(Rs)> value{WasmValType<Rs>...};
};

}  // namespace detail

// ---------------------------------------------------------------------------
// SignatureTraits<Sig> — Sig is a function type R(Args...).
//   ::params  — std::array<ValType, sizeof...(Args)>
//   ::results — std::array<ValType, arity(R)>   (0 for void, N for tuple<...>)
// ---------------------------------------------------------------------------

template <typename Sig>
struct SignatureTraits;

template <typename R, typename... Args>
struct SignatureTraits<R(Args...)> {
    static constexpr std::array<ValType, sizeof...(Args)> params{WasmValType<Args>...};
    static constexpr auto results = detail::ResultTypes<R>::value;

    static constexpr std::span<const ValType> ParamSpan() noexcept { return params; }
    static constexpr std::span<const ValType> ResultSpan() noexcept { return results; }
};

// Convenience: pull the two spans for any callable signature type.
template <typename Sig>
constexpr std::span<const ValType> ParamsOf() noexcept {
    return SignatureTraits<Sig>::ParamSpan();
}
template <typename Sig>
constexpr std::span<const ValType> ResultsOf() noexcept {
    return SignatureTraits<Sig>::ResultSpan();
}

// ---------------------------------------------------------------------------
// Facade-stage self-checks — these already hold; they pin the contract.
// ---------------------------------------------------------------------------

static_assert(WasmValType<f32> == ValType::F32);
static_assert(WasmValType<u64> == ValType::I64);
static_assert(SignatureTraits<i32(f32, f32)>::params.size() == 2);
static_assert(SignatureTraits<i32(f32, f32)>::results.size() == 1);
static_assert(SignatureTraits<void(i32)>::results.size() == 0);
static_assert(SignatureTraits<std::tuple<i32, i32>(i64)>::results.size() == 2);
static_assert(SignatureTraits<i32(f32, f32)>::params[0] == ValType::F32);

}  // namespace wasmp

#endif  // WASMP_HOST_TRAITS_HPP
