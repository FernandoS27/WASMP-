// wasmp — code_buffer.hpp
// A growable byte buffer tuned for an assembler's write pattern: many tiny
// appends, occasional small patches, never random inserts. The hot path is
// Ensure(max_instr_size) once per instruction, then unchecked stores.
//
// Header-only. Not std::vector: we want uninitialised geometric growth and a
// reuse policy (Reset keeps capacity) the vector API cannot express, plus a
// stable ABI-free footprint. See DESIGN.md §3.1.
#ifndef WASMP_CODE_BUFFER_HPP
#define WASMP_CODE_BUFFER_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

#include "common.hpp"
#include "leb128.hpp"

namespace wasmp {

class CodeBuffer {
public:
    CodeBuffer() noexcept = default;

    explicit CodeBuffer(size_t reserve_hint) {
        if (reserve_hint) Reserve(reserve_hint);
    }

    ~CodeBuffer() { std::free(data_); }

    CodeBuffer(CodeBuffer&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = other.capacity_ = 0;
    }

    CodeBuffer& operator=(CodeBuffer&& other) noexcept {
        if (this != &other) {
            std::free(data_);
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = other.capacity_ = 0;
        }
        return *this;
    }

    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    // --- Lifecycle / reuse (object-pool protocol) --------------------------
    void Reserve(size_t n) {
        if (n > capacity_) GrowTo(n);
    }

    // Drop contents, keep the allocation for reuse across bodies/modules.
    void Reset() noexcept { size_ = 0; }

    // --- Hot path: Ensure once, then unchecked stores ----------------------
    WASMP_FORCE_INLINE void Ensure(size_t n) {
        if (WASMP_UNLIKELY(size_ + n > capacity_)) GrowTo(size_ + n);
    }

    WASMP_FORCE_INLINE void PutU8Unchecked(u8 v) noexcept {
        data_[size_++] = v;
    }

    WASMP_FORCE_INLINE void PutBytesUnchecked(const void* p, size_t n) noexcept {
        std::memcpy(data_ + size_, p, n);
        size_ += n;
    }

    // --- Checked convenience (Ensure + put fused) --------------------------
    void PutU8(u8 v) {
        Ensure(1);
        PutU8Unchecked(v);
    }

    void PutULEB(u64 v) {
        Ensure(detail::kMaxULEB64);
        size_ = static_cast<size_t>(detail::PutULEB(data_ + size_, v) - data_);
    }

    void PutSLEB(i64 v) {
        Ensure(detail::kMaxSLEB64);
        size_ = static_cast<size_t>(detail::PutSLEB(data_ + size_, v) - data_);
    }

    void PutSLEB33(i64 v) {
        Ensure(detail::kMaxSLEB33);
        size_ = static_cast<size_t>(detail::PutSLEB33(data_ + size_, v) - data_);
    }

    // Floats are written little-endian byte by byte — correct on any host
    // endianness with no runtime branch (the bytes ARE the encoding).
    void PutF32(f32 v) {
        Ensure(4);
        const u32 bits = std::bit_cast<u32>(v);
        PutU8Unchecked(static_cast<u8>(bits));
        PutU8Unchecked(static_cast<u8>(bits >> 8));
        PutU8Unchecked(static_cast<u8>(bits >> 16));
        PutU8Unchecked(static_cast<u8>(bits >> 24));
    }

    void PutF64(f64 v) {
        Ensure(8);
        const u64 bits = std::bit_cast<u64>(v);
        for (int i = 0; i < 8; ++i)
            PutU8Unchecked(static_cast<u8>(bits >> (8 * i)));
    }

    void PutBytes(std::span<const u8> bytes) {
        Ensure(bytes.size());
        if (!bytes.empty()) PutBytesUnchecked(bytes.data(), bytes.size());
    }

    // --- Single-Ensure cursor protocol (the emitter hot path) --------------
    // Reserve n bytes once, obtain a raw cursor, write with the detail::Put*
    // encoders (which advance and return the new cursor), then commit the end.
    // This is what lets one instruction cost a single capacity check.
    WASMP_FORCE_INLINE u8* CursorEnsure(size_t n) {
        Ensure(n);
        return data_ + size_;
    }
    WASMP_FORCE_INLINE void SetEnd(u8* p) noexcept {
        size_ = static_cast<size_t>(p - data_);
    }

    // --- Read-back / handoff -----------------------------------------------
    std::span<const u8> Bytes() const noexcept { return {data_, size_}; }
    size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }
    size_t Capacity() const noexcept { return capacity_; }

private:
    // Geometric growth: at least double, at least the requested minimum, with a
    // small floor so the first append doesn't allocate byte-by-byte.
    WASMP_NOINLINE void GrowTo(size_t min_capacity) {
        size_t new_cap = capacity_ ? capacity_ * 2 : kInitialCapacity;
        if (new_cap < min_capacity) new_cap = min_capacity;
        auto* p = static_cast<u8*>(std::realloc(data_, new_cap));
        if (WASMP_UNLIKELY(p == nullptr)) detail::ReportError("wasmp: out of memory growing CodeBuffer");
        data_ = p;
        capacity_ = new_cap;
    }

    static constexpr size_t kInitialCapacity = 64;

    u8* data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

}  // namespace wasmp

#endif  // WASMP_CODE_BUFFER_HPP
