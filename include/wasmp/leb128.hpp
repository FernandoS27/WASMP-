// wasmp — leb128.hpp
// LEB128 variable-length integer encoders — the hottest primitive in any WASM
// producer. Encoders write into a caller-provided cursor that has already been
// sized (the emitter's single Ensure() per instruction guarantees room) and
// return the one-past-the-end pointer. No bounds checks on the encode path.
//
// Internal (detail) mechanism, exposed via CodeBuffer. See DESIGN.md §3.1.
#ifndef WASMP_LEB128_HPP
#define WASMP_LEB128_HPP

#include <bit>
#include <cstddef>
#include <cstdint>

#include "common.hpp"

namespace wasmp::detail {

// Raw cursor writers (no bounds checks; caller reserved space via CursorEnsure).
WASMP_FORCE_INLINE u8* PutByte(u8* dst, u8 v) noexcept {
    *dst++ = v;
    return dst;
}
WASMP_FORCE_INLINE u8* PutBytesRaw(u8* dst, const u8* src, size_t n) noexcept {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    return dst + n;
}

// Little-endian float stores — endian-independent (the bytes ARE the encoding).
WASMP_FORCE_INLINE u8* PutF32(u8* dst, f32 v) noexcept {
    const u32 bits = std::bit_cast<u32>(v);
    for (int i = 0; i < 4; ++i) *dst++ = static_cast<u8>(bits >> (8 * i));
    return dst;
}
WASMP_FORCE_INLINE u8* PutF64(u8* dst, f64 v) noexcept {
    const u64 bits = std::bit_cast<u64>(v);
    for (int i = 0; i < 8; ++i) *dst++ = static_cast<u8>(bits >> (8 * i));
    return dst;
}

// Max encoded lengths (bytes). u32→5, u64→10, i32→5, i64→10, block-type→5.
inline constexpr size_t kMaxULEB32 = 5;
inline constexpr size_t kMaxULEB64 = 10;
inline constexpr size_t kMaxSLEB32 = 5;
inline constexpr size_t kMaxSLEB64 = 10;
inline constexpr size_t kMaxSLEB33 = 5;

// Unsigned LEB128. Single-byte fast path for v < 0x80 covers the vast majority
// of real immediates (local/type indices, small constants, alignments).
WASMP_FORCE_INLINE u8* PutULEB(u8* dst, u64 v) noexcept {
    if (WASMP_LIKELY(v < 0x80)) {
        *dst++ = static_cast<u8>(v);
        return dst;
    }
    do {
        u8 byte = static_cast<u8>(v & 0x7F);
        v >>= 7;
        if (v != 0) byte |= 0x80;
        *dst++ = byte;
    } while (v != 0);
    return dst;
}

// Signed LEB128. Single-byte fast path for the [-64, 63] range.
WASMP_FORCE_INLINE u8* PutSLEB(u8* dst, i64 v) noexcept {
    if (WASMP_LIKELY(v >= -64 && v < 64)) {
        *dst++ = static_cast<u8>(v & 0x7F);
        return dst;
    }
    for (;;) {
        u8 byte = static_cast<u8>(v & 0x7F);
        v >>= 7;  // arithmetic shift — sign-extends
        const bool sign_bit_set = (byte & 0x40) != 0;
        if ((v == 0 && !sign_bit_set) || (v == -1 && sign_bit_set)) {
            *dst++ = byte;
            return dst;
        }
        *dst++ = static_cast<u8>(byte | 0x80);
    }
}

// Block-type / s33 encoding. A non-negative type index encoded as a signed
// LEB128 wide enough to keep the value positive — the ordinary signed encoder
// produces exactly that for a positive value (a trailing 0x00 continuation is
// appended when bit 6 would otherwise read as a sign bit).
WASMP_FORCE_INLINE u8* PutSLEB33(u8* dst, i64 v) noexcept {
    return PutSLEB(dst, v);
}

// Byte length v would occupy — used by Assemble()'s exact-size precompute.
inline size_t SizeULEB(u64 v) noexcept {
    size_t n = 1;
    while (v >= 0x80) { v >>= 7; ++n; }
    return n;
}

inline size_t SizeSLEB(i64 v) noexcept {
    size_t n = 1;
    for (;;) {
        const u8 byte = static_cast<u8>(v & 0x7F);
        v >>= 7;
        const bool sign_bit_set = (byte & 0x40) != 0;
        if ((v == 0 && !sign_bit_set) || (v == -1 && sign_bit_set)) break;
        ++n;
    }
    return n;
}

}  // namespace wasmp::detail

#endif  // WASMP_LEB128_HPP
