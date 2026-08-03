// unit_m0.cpp — M0 mechanism-layer tests (LEB128 + CodeBuffer).
// Dependency-free: a tiny inline harness, no external test framework.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

#include <wasmp/code_buffer.hpp>
#include <wasmp/leb128.hpp>

// --------------------------------------------------------------------------
// Minimal harness
// --------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d  CHECK(%s)\n", __FILE__,      \
                         __LINE__, #cond);                                 \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using namespace wasmp;

// --------------------------------------------------------------------------
// Reference decoders (independent of the encoder under test)
// --------------------------------------------------------------------------
static u64 DecodeULEB(const u8*& p) {
    u64 result = 0;
    int shift = 0;
    u8 byte;
    do {
        byte = *p++;
        result |= static_cast<u64>(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    return result;
}

static i64 DecodeSLEB(const u8*& p) {
    // Accumulate in unsigned to keep shifts/sign-extension free of signed UB.
    u64 result = 0;
    int shift = 0;
    u8 byte;
    do {
        byte = *p++;
        result |= static_cast<u64>(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40))
        result |= ~static_cast<u64>(0) << shift;  // sign-extend
    return static_cast<i64>(result);
}

static std::vector<u8> EncU(u64 v) {
    u8 buf[16];
    u8* end = detail::PutULEB(buf, v);
    return {buf, end};
}
static std::vector<u8> EncS(i64 v) {
    u8 buf[16];
    u8* end = detail::PutSLEB(buf, v);
    return {buf, end};
}

static bool Eq(const std::vector<u8>& a, std::initializer_list<u8> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.begin(), a.size()) == 0;
}

// --------------------------------------------------------------------------
// LEB128
// --------------------------------------------------------------------------
static void TestLebKnownVectors() {
    CHECK(Eq(EncU(0), {0x00}));
    CHECK(Eq(EncU(1), {0x01}));
    CHECK(Eq(EncU(127), {0x7F}));
    CHECK(Eq(EncU(128), {0x80, 0x01}));
    CHECK(Eq(EncU(624485), {0xE5, 0x8E, 0x26}));

    CHECK(Eq(EncS(0), {0x00}));
    CHECK(Eq(EncS(1), {0x01}));
    CHECK(Eq(EncS(-1), {0x7F}));
    CHECK(Eq(EncS(63), {0x3F}));
    CHECK(Eq(EncS(64), {0xC0, 0x00}));
    CHECK(Eq(EncS(-64), {0x40}));
    CHECK(Eq(EncS(-65), {0xBF, 0x7F}));
    CHECK(Eq(EncS(-123456), {0xC0, 0xBB, 0x78}));
}

static void TestLebRoundTrip() {
    // Unsigned: sweep small values, powers-of-two boundaries, and extremes.
    for (u64 v = 0; v < 100000; ++v) {
        auto bytes = EncU(v);
        const u8* p = bytes.data();
        CHECK(DecodeULEB(p) == v);
        CHECK(static_cast<size_t>(p - bytes.data()) == bytes.size());
        CHECK(detail::SizeULEB(v) == bytes.size());
    }
    for (int shift = 0; shift < 64; ++shift) {
        for (i64 d = -2; d <= 2; ++d) {
            u64 v = (static_cast<u64>(1) << shift) + static_cast<u64>(d);
            auto bytes = EncU(v);
            const u8* p = bytes.data();
            CHECK(DecodeULEB(p) == v);
            CHECK(detail::SizeULEB(v) == bytes.size());
        }
    }
    CHECK(EncU(std::numeric_limits<u64>::max()).size() == detail::kMaxULEB64);

    // Signed: sweep across zero and both sign boundaries.
    for (i64 v = -100000; v < 100000; ++v) {
        auto bytes = EncS(v);
        const u8* p = bytes.data();
        CHECK(DecodeSLEB(p) == v);
        CHECK(static_cast<size_t>(p - bytes.data()) == bytes.size());
        CHECK(detail::SizeSLEB(v) == bytes.size());
    }
    for (int shift = 0; shift < 63; ++shift) {
        for (i64 d = -2; d <= 2; ++d) {
            i64 base = static_cast<i64>(1) << shift;
            for (i64 v : {base + d, -base + d}) {
                auto bytes = EncS(v);
                const u8* p = bytes.data();
                CHECK(DecodeSLEB(p) == v);
                CHECK(detail::SizeSLEB(v) == bytes.size());
            }
        }
    }
    CHECK(EncS(std::numeric_limits<i64>::min()).size() == detail::kMaxSLEB64);
    CHECK(EncS(std::numeric_limits<i64>::max()).size() == detail::kMaxSLEB64);
}

static void TestSleb33() {
    // Positive type indices must stay positive (trailing 0x00 when bit6 set).
    u8 buf[8];
    for (i64 idx : {0, 1, 63, 64, 65, 128, 1000, 1'000'000}) {
        u8* end = detail::PutSLEB33(buf, idx);
        const u8* p = buf;
        CHECK(DecodeSLEB(p) == idx);
        CHECK(p == end);
    }
    // The empty/void block type (0x40) and value types decode as negatives.
    {
        u8* end = detail::PutSLEB(buf, -64);  // 0x40 == empty block type
        CHECK(end - buf == 1 && buf[0] == 0x40);
    }
}

// --------------------------------------------------------------------------
// CodeBuffer
// --------------------------------------------------------------------------
static void TestBufferBasics() {
    CodeBuffer b;
    CHECK(b.Empty());
    CHECK(b.Size() == 0);

    b.PutU8(0xAB);
    b.PutULEB(300);       // 0xAC 0x02
    b.PutSLEB(-1);        // 0x7F
    CHECK(!b.Empty());

    auto s = b.Bytes();
    CHECK(s.size() == 4);
    CHECK(s[0] == 0xAB);
    CHECK(s[1] == 0xAC);
    CHECK(s[2] == 0x02);
    CHECK(s[3] == 0x7F);
}

static void TestBufferFloats() {
    CodeBuffer b;
    b.PutF32(1.0f);   // 0x3F800000 -> LE 00 00 80 3F
    b.PutF64(2.0);    // 0x4000000000000000 -> LE 00 00 00 00 00 00 00 40
    auto s = b.Bytes();
    CHECK(s.size() == 12);
    const u8 f32[] = {0x00, 0x00, 0x80, 0x3F};
    CHECK(std::memcmp(s.data(), f32, 4) == 0);
    const u8 f64[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40};
    CHECK(std::memcmp(s.data() + 4, f64, 8) == 0);
}

static void TestBufferGrowth() {
    CodeBuffer b;
    // Force many reallocations; content must survive every move.
    std::vector<u8> expected;
    for (int i = 0; i < 10000; ++i) {
        u8 v = static_cast<u8>(i * 31 + 7);
        b.PutU8(v);
        expected.push_back(v);
    }
    CHECK(b.Size() == expected.size());
    CHECK(std::memcmp(b.Bytes().data(), expected.data(), expected.size()) == 0);
}

static void TestBufferReset() {
    CodeBuffer b;
    for (int i = 0; i < 500; ++i) b.PutU8(static_cast<u8>(i));
    const size_t cap = b.Capacity();
    CHECK(cap >= 500);
    b.Reset();
    CHECK(b.Empty());
    CHECK(b.Capacity() == cap);  // reuse: capacity retained
    b.PutU8(0x42);
    CHECK(b.Size() == 1 && b.Bytes()[0] == 0x42);
    CHECK(b.Capacity() == cap);  // no realloc needed
}

static void TestBufferMove() {
    CodeBuffer a;
    a.PutU8(1);
    a.PutU8(2);
    a.PutU8(3);
    CodeBuffer moved = std::move(a);
    CHECK(moved.Size() == 3);
    CHECK(a.Empty() && a.Capacity() == 0);  // source left valid + empty

    CodeBuffer c;
    c.PutU8(9);
    c = std::move(moved);
    CHECK(c.Size() == 3);
    CHECK(c.Bytes()[0] == 1 && c.Bytes()[2] == 3);
}

static void TestBufferEnsureUnchecked() {
    CodeBuffer b;
    b.Ensure(4);
    b.PutU8Unchecked(0xDE);
    b.PutU8Unchecked(0xAD);
    const u8 more[] = {0xBE, 0xEF};
    b.PutBytesUnchecked(more, 2);
    auto s = b.Bytes();
    CHECK(s.size() == 4);
    const u8 expect[] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(std::memcmp(s.data(), expect, 4) == 0);
}

int main() {
    TestLebKnownVectors();
    TestLebRoundTrip();
    TestSleb33();
    TestBufferBasics();
    TestBufferFloats();
    TestBufferGrowth();
    TestBufferReset();
    TestBufferMove();
    TestBufferEnsureUnchecked();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
