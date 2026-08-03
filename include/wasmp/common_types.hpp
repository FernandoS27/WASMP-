// wasmp — common_types.hpp
// The short fixed-width scalar aliases used throughout the library (u8..u64,
// i8..i64, f32/f64). These are the single spelling for every primitive integer
// and floating-point type in wasmp code; prefer them over the <cstdint> names
// so the WASM value types (i32/i64/f32/f64) and their host spellings line up.
//
// Pure compile-time aliases with no behaviour — realised entirely at the facade
// stage. Included by common.hpp, so every wasmp header sees these names.
#ifndef WASMP_COMMON_TYPES_HPP
#define WASMP_COMMON_TYPES_HPP

#include <cstdint>

namespace wasmp {

// ---------------------------------------------------------------------------
// Basic scalar types
// ---------------------------------------------------------------------------

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

}  // namespace wasmp

#endif  // WASMP_COMMON_TYPES_HPP
