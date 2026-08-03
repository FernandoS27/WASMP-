// wasmp — interop/wamr.hpp  (optional adapter, not part of the core)
// Turns a compile-time C++ signature into the WAMR native-symbol signature
// string (e.g. i32(f32,f32) -> "(ff)i"), so registration/call strings
// are never hand-written. Header-only, consteval, no WAMR dependency to
// *build* the string (you only need WAMR to use it).
//
// See DESIGN.md §8.
#ifndef WASMP_INTEROP_WAMR_HPP
#define WASMP_INTEROP_WAMR_HPP

#include <array>
#include <string_view>

#include "../host_traits.hpp"

namespace wasmp::wamr {

// One WAMR signature character for a scalar ValType.
//   i32 -> 'i'   i64 -> 'I'   f32 -> 'f'   f64 -> 'F'
constexpr char SigChar(ValType v) noexcept {
    switch (v) {
        case ValType::I32: return 'i';
        case ValType::I64: return 'I';
        case ValType::F32: return 'f';
        case ValType::F64: return 'F';
        default:           return '?';  // ref/v128 use extended forms (later)
    }
}

// SigString<Sig>() -> a null-terminated std::array<char, N> holding
// "(<params>)<result>". WAMR native signatures are single-result; multi-value
// is rejected at compile time. Use `.data()` where a `const char*` is wanted.
template <typename Sig>
consteval auto SigString() {
    constexpr auto params = SignatureTraits<Sig>::params;
    constexpr auto results = SignatureTraits<Sig>::results;
    static_assert(results.size() <= 1,
                  "WAMR native signatures are single-result; a multi-value "
                  "export cannot be described by a WAMR signature string");
    constexpr size_t kLen = params.size() + results.size() + 2;  // '(' ')'
    std::array<char, kLen + 1> s{};
    size_t k = 0;
    s[k++] = '(';
    for (ValType v : params) s[k++] = SigChar(v);
    s[k++] = ')';
    for (ValType v : results) s[k++] = SigChar(v);
    s[k] = '\0';
    return s;
}

// The character length (excluding the trailing null) of SigString<Sig>().
template <typename Sig>
constexpr size_t SigStringLen() noexcept {
    return SignatureTraits<Sig>::params.size() + SignatureTraits<Sig>::results.size() + 2;
}

}  // namespace wasmp::wamr

#endif  // WASMP_INTEROP_WAMR_HPP
