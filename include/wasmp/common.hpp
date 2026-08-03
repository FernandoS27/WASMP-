// wasmp — common.hpp
// Fixed value types, strong index handles, immediates, feature flags, and the
// library configuration/assert/error macros. Everything here is a pure
// compile-time construct: it is fully defined at the facade stage because none
// of it has behaviour to implement later.
//
// Part of the public facade. See DESIGN.md §3.2, §4 (Features), §5.
#ifndef WASMP_COMMON_HPP
#define WASMP_COMMON_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#if defined(WASMP_THROW_ON_ERROR)
#  include <stdexcept>
#endif

#include "common_types.hpp"  // u8..u64, i8..i64, f32/f64

// ---------------------------------------------------------------------------
// Configuration, platform, and error-policy macros
// ---------------------------------------------------------------------------

// Error policy. Exactly one is active; WASMP_ABORT_ON_ERROR is the default.
//   WASMP_ABORT_ON_ERROR  — report to stderr and std::abort() (no exceptions)
//   WASMP_THROW_ON_ERROR  — throw wasmp::Error (requires exceptions)
// `TryAssemble()` on Module always offers a non-aborting, std::expected-style
// path regardless of this choice (see module.hpp).
#if !defined(WASMP_ABORT_ON_ERROR) && !defined(WASMP_THROW_ON_ERROR)
#  define WASMP_ABORT_ON_ERROR 1
#endif

#if defined(_MSC_VER)
#  define WASMP_FORCE_INLINE __forceinline
#  define WASMP_NOINLINE     __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#  define WASMP_FORCE_INLINE inline __attribute__((always_inline))
#  define WASMP_NOINLINE     __attribute__((noinline))
#else
#  define WASMP_FORCE_INLINE inline
#  define WASMP_NOINLINE
#endif

// Zero-size empty members (so the Trust-policy validator stand-in costs
// nothing). Keyed on the MSVC ABI, not the compiler: clang targeting Windows
// (_MSC_VER defined) also needs the msvc-namespaced spelling.
#if defined(_MSC_VER)
#  define WASMP_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#  define WASMP_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define WASMP_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define WASMP_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define WASMP_LIKELY(x)   (x)
#  define WASMP_UNLIKELY(x) (x)
#endif

// Pack a prefixed opcode (the 0xFC misc / 0xFD SIMD / 0xFE atomics spaces) into
// a single u32 so the instruction table can carry it as one macro token: high
// byte = prefix, low 24 bits = sub-opcode (LEB-encoded when emitted). Plain
// single-byte opcodes stay <= 0xFF and are distinguished by magnitude.
#define WASMP_PFX(prefix, sub) \
    ((static_cast<uint32_t>(prefix) << 24) | static_cast<uint32_t>(sub))

// The WASM binary format is little-endian. We assume a little-endian host on
// the fast path and provide a byte-swapping fallback (see code_buffer.hpp).
// Mixed-endian hosts are unsupported by design.
static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big,
              "wasmp requires a pure little- or big-endian host");

// Debug-only invariant check. Compiles to nothing in release builds; the
// validating emitter policy (policy::Validate) is the release-agnostic path
// for user-facing correctness diagnostics.
#if defined(NDEBUG)
#  define WASMP_ASSERT(cond, msg) ((void)0)
#else
#  define WASMP_ASSERT(cond, msg) \
      ((cond) ? (void)0 : ::wasmp::detail::AssertFail(#cond, (msg), __FILE__, __LINE__))
#endif

namespace wasmp {

#if defined(WASMP_THROW_ON_ERROR)
// Thrown by the error path under WASMP_THROW_ON_ERROR. Requires exceptions.
class Error : public std::runtime_error {
public:
    explicit Error(std::string_view msg)
        : std::runtime_error(std::string(msg)) {}
};
#endif

namespace detail {

// The library-wide error sink. Under the default policy it reports and aborts;
// under WASMP_THROW_ON_ERROR it throws wasmp::Error. TryAssemble() offers a
// non-terminating path independent of this choice (see module.hpp).
[[noreturn]] inline void ReportError(std::string_view msg) {
#if defined(WASMP_THROW_ON_ERROR)
    throw Error(msg);
#else
    std::fputs("wasmp error: ", stderr);
    std::fwrite(msg.data(), 1, msg.size(), stderr);
    std::fputc('\n', stderr);
    std::abort();
#endif
}

// Backstop for WASMP_ASSERT (debug builds only). Always terminates — a failed
// internal invariant is a bug, not a recoverable condition.
[[noreturn]] inline void AssertFail(const char* expr, const char* msg,
                                    const char* file, int line) {
    std::fprintf(stderr, "wasmp assertion failed: %s\n  %s\n  at %s:%d\n",
                 expr, msg, file, line);
    std::abort();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Value and reference types (binary encodings are the enum values)
// ---------------------------------------------------------------------------

enum class ValType : u8 {
    I32 = 0x7F,
    I64 = 0x7E,
    F32 = 0x7D,
    F64 = 0x7C,
    V128 = 0x7B,
    FuncRef = 0x70,
    ExternRef = 0x6F,
};

// Narrowing view over the subset of ValType usable as a table element / ref.
enum class RefType : u8 {
    FuncRef = 0x70,
    ExternRef = 0x6F,
};

enum class Mutability : u8 {
    Const = 0x00,
    Var = 0x01,
};

// Export/import descriptor kind (binary encoding).
enum class ExternalKind : u8 {
    Function = 0x00,
    Table = 0x01,
    Memory = 0x02,
    Global = 0x03,
};

constexpr ValType ToValType(RefType r) noexcept {
    return static_cast<ValType>(static_cast<u8>(r));
}

// ---------------------------------------------------------------------------
// Strong index handles — one per WASM index space (eight total).
// Zero-cost enum-class wrappers; passing the wrong space is a compile error.
// ---------------------------------------------------------------------------

enum class TypeIdx : u32 {};
enum class FuncIdx : u32 {};
enum class TableIdx : u32 {};
enum class MemIdx : u32 {};
enum class GlobalIdx : u32 {};
enum class LocalIdx : u32 {};
enum class DataIdx : u32 {};  // bulk memory: memory.init / data.drop
enum class ElemIdx : u32 {};  // bulk memory: table.init / elem.drop

template <typename Idx>
constexpr u32 Raw(Idx i) noexcept {
    return static_cast<u32>(i);
}

// ---------------------------------------------------------------------------
// Limits (memory / table) — binary "flags min [max]" (+ shared for memory).
// ---------------------------------------------------------------------------

struct MemLimits {
    u64 min = 0;                  // pages (64 KiB); u64 for memory64
    std::optional<u64> max = {};
    bool shared = false;               // threads proposal (requires max)
};

struct TableLimits {
    u64 min = 0;                  // elements
    std::optional<u64> max = {};
};

// ---------------------------------------------------------------------------
// MemArg — the immediate carried by every load/store.
// ---------------------------------------------------------------------------

// Sentinel meaning "use this instruction's natural alignment", supplied by the
// instruction table (§4). Natural alignment is the common case, so it is the
// default and callers rarely set `align` explicitly.
inline constexpr u8 kNaturalAlign = 0xFF;

struct MemArg {
    u64 offset = 0;               // u64-ready for memory64
    u8 align = kNaturalAlign;     // log2(bytes); resolved against the table
    MemIdx memory = MemIdx{0};         // non-zero sets the multi-memory flag bit
};

// ---------------------------------------------------------------------------
// v128 — a host-side 128-bit SIMD value. Little-endian lane layout (matching
// the WASM v128 memory/value representation), with lane pack/unpack helpers so
// SIMD values can be built on the host and passed into a module (see the
// wasmtime interop adapter).
// ---------------------------------------------------------------------------
struct v128 {
    std::array<u8, 16> bytes{};

    static v128 FromI32x4(i32 a, i32 b, i32 c, i32 d) noexcept {
        v128 v{};
        const i32 lanes[4] = {a, b, c, d};
        for (int l = 0; l < 4; ++l) {
            const u32 u = static_cast<u32>(lanes[l]);
            for (int i = 0; i < 4; ++i) v.bytes[l * 4 + i] = static_cast<u8>(u >> (8 * i));
        }
        return v;
    }
    static v128 FromF32x4(f32 a, f32 b, f32 c, f32 d) noexcept {
        const f32 lanes[4] = {a, b, c, d};
        v128 v{};
        for (int l = 0; l < 4; ++l) {
            const u32 u = std::bit_cast<u32>(lanes[l]);
            for (int i = 0; i < 4; ++i) v.bytes[l * 4 + i] = static_cast<u8>(u >> (8 * i));
        }
        return v;
    }
    i32 I32(size_t lane) const noexcept {
        u32 u = 0;
        for (int i = 0; i < 4; ++i) u |= static_cast<u32>(bytes[lane * 4 + i]) << (8 * i);
        return static_cast<i32>(u);
    }
    f32 F32(size_t lane) const noexcept {
        u32 u = 0;
        for (int i = 0; i < 4; ++i) u |= static_cast<u32>(bytes[lane * 4 + i]) << (8 * i);
        return std::bit_cast<f32>(u);
    }
};

// ---------------------------------------------------------------------------
// Constant expressions — global initialisers and active-segment offsets.
// A closed set (variant), not a general emitter: init exprs are cold and tiny.
// ---------------------------------------------------------------------------

struct I32Const { i32 value = 0; };
struct I64Const { i64 value = 0; };
struct F32Const { f32 value = 0.0f; };
struct F64Const { f64 value = 0.0; };
struct GlobalGet { GlobalIdx global; };     // imported-global-relative
struct RefNull { RefType type; };
struct RefFunc { FuncIdx func; };

using ConstExpr = std::variant<I32Const, I64Const, F32Const, F64Const,
                               GlobalGet, RefNull, RefFunc>;

// ---------------------------------------------------------------------------
// BlockType — the operand of block/loop/if: void (0x40), a single ValType, or
// a TypeIdx (multi-value, encoded SLEB33). Implicitly constructible from the
// common spellings so call sites stay terse.
// ---------------------------------------------------------------------------
class BlockType {
public:
    enum class Kind : u8 { Void, Value, Type };

    constexpr BlockType() noexcept : kind_(Kind::Void) {}
    constexpr BlockType(ValType v) noexcept : kind_(Kind::Value), val_(v) {}
    constexpr BlockType(TypeIdx t) noexcept : kind_(Kind::Type), type_(t) {}

    constexpr Kind GetKind() const noexcept { return kind_; }
    constexpr ValType Value() const noexcept { return val_; }
    constexpr TypeIdx Type() const noexcept { return type_; }

private:
    Kind kind_;
    ValType val_{};
    TypeIdx type_{};
};

// ---------------------------------------------------------------------------
// Feature set — gates emission and validator expectations.
// Constructing an emitter/module with a feature off makes its opcodes
// debug-assert (release: no check). Defaults track the stable WASM 2.0 core.
// ---------------------------------------------------------------------------

struct Features {
    // WASM 2.0 core (on by default)
    bool mutable_globals = true;
    bool sign_extension = true;
    bool nontrapping_fptoint = true;
    bool multi_value = true;
    bool reference_types = true;
    bool bulk_memory = true;
    // Post-2.0 proposals (opt in)
    bool simd = false;
    bool relaxed_simd = false;
    bool atomics = false;       // threads / shared memory
    bool tail_call = false;
    bool multi_memory = false;
    bool memory64 = false;
    bool exceptions = false;
    bool gc = false;

    // The stable baseline this library targets first.
    static constexpr Features Core() noexcept { return Features{}; }

    // Everything wasmp knows how to emit (for tooling/tests).
    static constexpr Features All() noexcept {
        Features f{};
        f.simd = f.relaxed_simd = f.atomics = f.tail_call = f.multi_memory =
            f.memory64 = f.exceptions = f.gc = true;
        return f;
    }
};

}  // namespace wasmp

#endif  // WASMP_COMMON_HPP
