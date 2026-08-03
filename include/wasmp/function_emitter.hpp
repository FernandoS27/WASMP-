// wasmp — function_emitter.hpp
// The xbyak-style instruction layer: one FunctionEmitter per function body.
// Instruction methods are generated from instructions.inc so the opcode table
// stays the single source of truth; the structurally special operations
// (control frames, branches, indirect calls) are hand-written because they
// interact with the control stack and return RAII handles.
//
// M1: core emission (policy = Trust behaviour). The validating policy body is
// added in M3 — the hook points are structural, not yet populated.
// See DESIGN.md §6, §7, §9.
#ifndef WASMP_FUNCTION_EMITTER_HPP
#define WASMP_FUNCTION_EMITTER_HPP

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "code_buffer.hpp"
#include "common.hpp"
#include "leb128.hpp"
#include "validator.hpp"

namespace wasmp {

class Module;
template <typename Policy> class FunctionEmitter;

// ---------------------------------------------------------------------------
// Validation policies (compile-time Strategy + Null Object). policy::Trust is
// an empty base — EBO makes it add zero state. policy::Validate gains a type
// stack driven by the table in M3; today both behave as Trust.
// ---------------------------------------------------------------------------
namespace policy {
struct Trust {};
struct Validate {};
}  // namespace policy

#if defined(NDEBUG)
using DefaultPolicy = policy::Trust;
#else
using DefaultPolicy = policy::Validate;
#endif

namespace detail {

// Emit an opcode: single byte for the main space, or prefix + LEB sub-opcode
// for the 0xFC/0xFD/0xFE spaces (distinguished by magnitude — see WASMP_PFX).
inline constexpr size_t kMaxOpBytes = 1 + kMaxULEB32;

WASMP_FORCE_INLINE u8* EmitOp(u8* p, u32 op) noexcept {
    if (op <= 0xFF) {
        *p++ = static_cast<u8>(op);
        return p;
    }
    *p++ = static_cast<u8>(op >> 24);           // prefix
    return PutULEB(p, op & 0x00FFFFFFu);             // sub-opcode
}

// memarg = align (with optional multi-memory flag + memidx) then offset.
inline constexpr size_t kMaxMemArg = kMaxULEB32 + kMaxULEB32 + kMaxULEB64;

WASMP_FORCE_INLINE u8* EmitMemArg(u8* p, const MemArg& m,
                                       u8 natural) noexcept {
    const u32 align = (m.align == kNaturalAlign) ? natural : m.align;
    if (Raw(m.memory) == 0) {
        p = PutULEB(p, align);
        p = PutULEB(p, m.offset);
    } else {
        p = PutULEB(p, align | 0x40u);               // multi-memory flag bit
        p = PutULEB(p, Raw(m.memory));
        p = PutULEB(p, m.offset);
    }
    return p;
}

WASMP_FORCE_INLINE u8* EmitBlockType(u8* p, BlockType bt) noexcept {
    switch (bt.GetKind()) {
        case BlockType::Kind::Void:
            *p++ = 0x40;
            return p;
        case BlockType::Kind::Value:
            *p++ = static_cast<u8>(bt.Value());
            return p;
        case BlockType::Kind::Type:
            return PutSLEB33(p, static_cast<i64>(Raw(bt.Type())));
    }
    return p;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ControlFrame — move-only RAII handle for one block/loop/if scope. Branch
// targets are frames, not byte offsets. On destruction it emits its `end`
// unless End()/Detach() was called. Constructible only by FunctionEmitter.
// ---------------------------------------------------------------------------
template <typename Policy>
class ControlFrame {
public:
    ControlFrame(ControlFrame&& o) noexcept
        : owner_(o.owner_), depth_(o.depth_), live_(o.live_) {
        o.owner_ = nullptr;
        o.live_ = false;
    }
    ControlFrame& operator=(ControlFrame&& o) noexcept {
        if (this != &o) {
            owner_ = o.owner_;
            depth_ = o.depth_;
            live_ = o.live_;
            o.owner_ = nullptr;
            o.live_ = false;
        }
        return *this;
    }
    ControlFrame(const ControlFrame&) = delete;
    ControlFrame& operator=(const ControlFrame&) = delete;

    ~ControlFrame() {
        if (live_ && owner_) owner_->FrameEnd(depth_);
    }

    void Else();
    void End();
    void Detach() noexcept { live_ = false; }

private:
    friend class FunctionEmitter<Policy>;
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    ControlFrame(Passkey, FunctionEmitter<Policy>* owner, u32 depth) noexcept
        : owner_(owner), depth_(depth), live_(true) {}

private:
    FunctionEmitter<Policy>* owner_ = nullptr;
    u32 depth_ = 0;
    bool live_ = true;
};

// ---------------------------------------------------------------------------
// FunctionBody — the immutable product of Finish(): local-declaration block +
// body bytes (the body already ends with its `end` opcode). Move-only,
// constructible only by FunctionEmitter::Finish().
// ---------------------------------------------------------------------------
class FunctionBody {
public:
    FunctionBody(FunctionBody&&) noexcept = default;
    FunctionBody& operator=(FunctionBody&&) noexcept = default;
    FunctionBody(const FunctionBody&) = delete;
    FunctionBody& operator=(const FunctionBody&) = delete;
    ~FunctionBody() = default;

    std::span<const u8> LocalDecls() const noexcept { return local_decls_.Bytes(); }
    std::span<const u8> Code() const noexcept { return code_.Bytes(); }
    size_t EncodedSize() const noexcept { return local_decls_.Size() + code_.Size(); }

private:
    template <typename P> friend class FunctionEmitter;
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    FunctionBody(Passkey, CodeBuffer&& locals, CodeBuffer&& code) noexcept
        : local_decls_(std::move(locals)), code_(std::move(code)) {}

private:
    CodeBuffer local_decls_;
    CodeBuffer code_;
};

// ---------------------------------------------------------------------------
// FunctionEmitter<Policy>
// ---------------------------------------------------------------------------
template <typename Policy = DefaultPolicy>
class FunctionEmitter : private Policy {
public:
    using Frame = ControlFrame<Policy>;

    FunctionEmitter(Module& mod, TypeIdx signature);   // defined in module.hpp
    explicit FunctionEmitter(Module& mod);             // defined in module.hpp
    ~FunctionEmitter() = default;

    FunctionEmitter(FunctionEmitter&&) noexcept = default;
    FunctionEmitter& operator=(FunctionEmitter&&) noexcept = default;
    FunctionEmitter(const FunctionEmitter&) = delete;
    FunctionEmitter& operator=(const FunctionEmitter&) = delete;

    // --- Locals ------------------------------------------------------------
    LocalIdx Param(u32 i) const noexcept {
        WASMP_ASSERT(i < param_count_, "param index out of range");
        return LocalIdx{i};
    }
    LocalIdx DeclareLocal(ValType t) { return DeclareLocals(t, 1); }
    LocalIdx DeclareLocals(ValType t, u32 count) {
        const u32 first = param_count_ + num_locals_;
        if (!local_runs_.empty() && local_runs_.back().first == t)
            local_runs_.back().second += count;
        else
            local_runs_.emplace_back(t, count);
        num_locals_ += count;
        if constexpr (kValidate) v_.AddLocals(t, count);
        return LocalIdx{first};
    }

    // --- Structured control (hand-written; return / consume frames) --------
    [[nodiscard]] Frame Block(BlockType bt = {}) { return PushFrame(0x02, FrameKind::Block, bt); }
    [[nodiscard]] Frame Loop(BlockType bt = {}) { return PushFrame(0x03, FrameKind::Loop, bt); }
    [[nodiscard]] Frame If(BlockType bt = {}) { return PushFrame(0x04, FrameKind::If, bt); }

    void Br(const Frame& target) {
        const u32 rel = RelDepth(target);
        if constexpr (kValidate) v_.OnBr(rel);
        EmitBranch(0x0C, rel);
    }
    void BrIf(const Frame& target) {
        const u32 rel = RelDepth(target);
        if constexpr (kValidate) v_.OnBrIf(rel);
        EmitBranch(0x0D, rel);
    }
    void BrTable(std::span<const Frame* const> targets, const Frame& default_) {
        if constexpr (kValidate) {
            std::vector<u32> rels;
            rels.reserve(targets.size());
            for (const Frame* t : targets) rels.push_back(RelDepth(*t));
            v_.OnBrTable(rels, RelDepth(default_));
        }
        u8* p = code_.CursorEnsure(detail::kMaxOpBytes +
                                        detail::kMaxULEB32 * (targets.size() + 2));
        p = detail::EmitOp(p, 0x0E);
        p = detail::PutULEB(p, targets.size());
        for (const Frame* t : targets) p = detail::PutULEB(p, RelDepth(*t));
        p = detail::PutULEB(p, RelDepth(default_));
        code_.SetEnd(p);
    }

    // --- call_indirect / return_call_indirect (two immediates) -------------
    void CallIndirect(TypeIdx sig, TableIdx table = TableIdx{0}) {
        if constexpr (kValidate) v_.OnCallIndirect(sig, table);
        u8* p = code_.CursorEnsure(detail::kMaxOpBytes + 2 * detail::kMaxULEB32);
        p = detail::EmitOp(p, 0x11);
        p = detail::PutULEB(p, Raw(sig));
        p = detail::PutULEB(p, Raw(table));
        code_.SetEnd(p);
    }
    void ReturnCallIndirect(TypeIdx sig, TableIdx table = TableIdx{0}) {
        if constexpr (kValidate) {
            v_.CheckFeature("TAIL_CALL");
            v_.OnReturnCallIndirect(sig, table);
        }
        u8* p = code_.CursorEnsure(detail::kMaxOpBytes + 2 * detail::kMaxULEB32);
        p = detail::EmitOp(p, 0x13);
        p = detail::PutULEB(p, Raw(sig));
        p = detail::PutULEB(p, Raw(table));
        code_.SetEnd(p);
    }

    // --- Typed select (reference types): 0x1C + a 1-element valtype vec -----
    void SelectT(ValType type) {
        if constexpr (kValidate) {
            v_.CheckFeature("REFERENCE_TYPES");
            v_.OnSelectT(type);
        }
        u8* p = code_.CursorEnsure(detail::kMaxOpBytes + 2);
        p = detail::EmitOp(p, 0x1C);
        p = detail::PutByte(p, 1);  // vec length
        p = detail::PutByte(p, static_cast<u8>(type));
        code_.SetEnd(p);
    }

    // --- Escape hatch ------------------------------------------------------
    void EmitRaw(std::span<const u8> bytes) { code_.PutBytes(bytes); }

    // --- Table-generated instruction methods (definitions, not just decls) -
    // Each method optionally runs the validator hook for its shape (compiled
    // out entirely when the policy is Trust), then emits its bytes. The
    // stringized stack-effect column (Eff) drives the fixed-effect validation.
#define WASMP_ENSURE_(n) code_.CursorEnsure(detail::kMaxOpBytes + (n))
#define WASMP_V(call) do { if constexpr (kValidate) v_.call; } while (0)
#define WASMP_GATE(Feat) WASMP_V(CheckFeature(Feat))  // debug feature-flag check
#define WASMP_EMIT_IDX_(Op) \
    u8* p = WASMP_ENSURE_(detail::kMaxULEB32); \
    p = detail::EmitOp(p, (Op)); p = detail::PutULEB(p, Raw(a)); code_.SetEnd(p)
#define WASMP_DEF_NONE(Name, Op, N, Eff, Feat) \
    void Name() { WASMP_GATE(Feat); WASMP_V(OnNone(Mnemonic::Name, Eff)); \
        u8* p = WASMP_ENSURE_(0); p = detail::EmitOp(p, (Op)); code_.SetEnd(p); }
#define WASMP_DEF_LOCAL_IDX(Name, Op, N, Eff, Feat) \
    void Name(LocalIdx a) { WASMP_GATE(Feat); WASMP_V(OnLocal(Mnemonic::Name, a)); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_GLOBAL_IDX(Name, Op, N, Eff, Feat) \
    void Name(GlobalIdx a) { WASMP_GATE(Feat); WASMP_V(OnGlobal(Mnemonic::Name, a)); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_TABLE_IDX(Name, Op, N, Eff, Feat) \
    void Name(TableIdx a) { WASMP_GATE(Feat); WASMP_V(OnTable(Mnemonic::Name, a)); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_FUNC_IDX(Name, Op, N, Eff, Feat) \
    void Name(FuncIdx a) { WASMP_GATE(Feat); WASMP_V(OnFuncIdx(Mnemonic::Name, a)); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_TYPE_IDX(Name, Op, N, Eff, Feat) \
    void Name(TypeIdx a) { WASMP_GATE(Feat); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_DATA_IDX(Name, Op, N, Eff, Feat) \
    void Name(DataIdx a) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_ELEM_IDX(Name, Op, N, Eff, Feat) \
    void Name(ElemIdx a) { WASMP_GATE(Feat); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_MEM_IDX(Name, Op, N, Eff, Feat) \
    void Name(MemIdx a = MemIdx{0}) { WASMP_GATE(Feat); WASMP_V(OnMemBulk(Mnemonic::Name, Eff)); WASMP_EMIT_IDX_(Op); }
#define WASMP_DEF_I32(Name, Op, N, Eff, Feat) \
    void Name(i32 value) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); u8* p = WASMP_ENSURE_(detail::kMaxSLEB32); \
        p = detail::EmitOp(p, (Op)); p = detail::PutSLEB(p, value); code_.SetEnd(p); }
#define WASMP_DEF_I64(Name, Op, N, Eff, Feat) \
    void Name(i64 value) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); u8* p = WASMP_ENSURE_(detail::kMaxSLEB64); \
        p = detail::EmitOp(p, (Op)); p = detail::PutSLEB(p, value); code_.SetEnd(p); }
#define WASMP_DEF_F32(Name, Op, N, Eff, Feat) \
    void Name(f32 value) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); u8* p = WASMP_ENSURE_(4); \
        p = detail::EmitOp(p, (Op)); p = detail::PutF32(p, value); code_.SetEnd(p); }
#define WASMP_DEF_F64(Name, Op, N, Eff, Feat) \
    void Name(f64 value) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); u8* p = WASMP_ENSURE_(8); \
        p = detail::EmitOp(p, (Op)); p = detail::PutF64(p, value); code_.SetEnd(p); }
#define WASMP_DEF_REF_TYPE(Name, Op, N, Eff, Feat) \
    void Name(RefType t) { WASMP_GATE(Feat); WASMP_V(OnRefNull(t)); u8* p = WASMP_ENSURE_(1); \
        p = detail::EmitOp(p, (Op)); p = detail::PutByte(p, static_cast<u8>(t)); code_.SetEnd(p); }
#define WASMP_DEF_LANE(Name, Op, N, Eff, Feat) \
    void Name(u8 lane) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); u8* p = WASMP_ENSURE_(1); \
        p = detail::EmitOp(p, (Op)); p = detail::PutByte(p, lane); code_.SetEnd(p); }
#define WASMP_DEF_MEMARG(Name, Op, N, Eff, Feat) \
    void Name(MemArg mem = {}) { WASMP_GATE(Feat); WASMP_V(OnMemArg(Eff, Raw(mem.memory))); \
        u8* p = WASMP_ENSURE_(detail::kMaxMemArg); \
        p = detail::EmitOp(p, (Op)); p = detail::EmitMemArg(p, mem, (N)); code_.SetEnd(p); }
#define WASMP_DEF_MEMINIT(Name, Op, N, Eff, Feat) \
    void Name(DataIdx data) { WASMP_GATE(Feat); WASMP_V(OnMemBulk(Mnemonic::Name, Eff)); \
        u8* p = WASMP_ENSURE_(detail::kMaxULEB32 + 1); \
        p = detail::EmitOp(p, (Op)); p = detail::PutULEB(p, Raw(data)); \
        p = detail::PutByte(p, 0x00); code_.SetEnd(p); }
// M4 proposal shapes.
#define WASMP_DEF_BYTES16(Name, Op, N, Eff, Feat) \
    void Name(const std::array<u8, 16>& bytes) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); \
        u8* p = WASMP_ENSURE_(16); p = detail::EmitOp(p, (Op)); \
        p = detail::PutBytesRaw(p, bytes.data(), 16); code_.SetEnd(p); }
#define WASMP_DEF_MEMARG_LANE(Name, Op, N, Eff, Feat) \
    void Name(MemArg mem, u8 lane) { WASMP_GATE(Feat); WASMP_V(OnMemArg(Eff, Raw(mem.memory))); \
        u8* p = WASMP_ENSURE_(detail::kMaxMemArg + 1); p = detail::EmitOp(p, (Op)); \
        p = detail::EmitMemArg(p, mem, (N)); p = detail::PutByte(p, lane); code_.SetEnd(p); }
#define WASMP_DEF_MEMCOPY(Name, Op, N, Eff, Feat) \
    void Name(MemIdx dst = MemIdx{0}, MemIdx src = MemIdx{0}) { WASMP_GATE(Feat); \
        WASMP_V(OnMemBulk(Mnemonic::Name, Eff)); \
        u8* p = WASMP_ENSURE_(2 * detail::kMaxULEB32); p = detail::EmitOp(p, (Op)); \
        p = detail::PutULEB(p, Raw(dst)); p = detail::PutULEB(p, Raw(src)); code_.SetEnd(p); }
#define WASMP_DEF_TABLE_INIT(Name, Op, N, Eff, Feat) \
    void Name(ElemIdx elem, TableIdx table = TableIdx{0}) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); \
        u8* p = WASMP_ENSURE_(2 * detail::kMaxULEB32); p = detail::EmitOp(p, (Op)); \
        p = detail::PutULEB(p, Raw(elem)); p = detail::PutULEB(p, Raw(table)); code_.SetEnd(p); }
#define WASMP_DEF_TABLE_COPY(Name, Op, N, Eff, Feat) \
    void Name(TableIdx dst = TableIdx{0}, TableIdx src = TableIdx{0}) { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); \
        u8* p = WASMP_ENSURE_(2 * detail::kMaxULEB32); p = detail::EmitOp(p, (Op)); \
        p = detail::PutULEB(p, Raw(dst)); p = detail::PutULEB(p, Raw(src)); code_.SetEnd(p); }
#define WASMP_DEF_FENCE(Name, Op, N, Eff, Feat) \
    void Name() { WASMP_GATE(Feat); WASMP_V(OnFixed(Eff)); u8* p = WASMP_ENSURE_(1); \
        p = detail::EmitOp(p, (Op)); p = detail::PutByte(p, 0x00); code_.SetEnd(p); }
// Control-structure shapes are hand-written above → generate nothing.
#define WASMP_DEF_BLOCK_TYPE(Name, Op, N, Eff, Feat)
#define WASMP_DEF_BR_LABEL(Name, Op, N, Eff, Feat)
#define WASMP_DEF_BR_TABLE(Name, Op, N, Eff, Feat)
#define WASMP_DEF_CALL_INDIRECT(Name, Op, N, Eff, Feat)
#define WASMP_DEF_CTRL_DELIM(Name, Op, N, Eff, Feat)
#define WASMP_DEF_SELECT_T(Name, Op, N, Eff, Feat)  // typed select: hand-written

// Each shape keyword expands to `TAG, param`; the router splits it and pastes
// WASMP_DEF_##TAG. Self-referential object-like macros yield the tag token
// unexpanded (blue-painted), which is exactly what we paste on.
#define NONE          NONE, 0
#define LOCAL_IDX     LOCAL_IDX, 0
#define GLOBAL_IDX    GLOBAL_IDX, 0
#define TABLE_IDX     TABLE_IDX, 0
#define FUNC_IDX      FUNC_IDX, 0
#define TYPE_IDX      TYPE_IDX, 0
#define DATA_IDX      DATA_IDX, 0
#define ELEM_IDX      ELEM_IDX, 0
#define MEM_IDX       MEM_IDX, 0
#define REF_TYPE      REF_TYPE, 0
#define LANE          LANE, 0
#define I32           I32, 0
#define I64           I64, 0
#define F32           F32, 0
#define F64           F64, 0
#define MEMINIT       MEMINIT, 0
#define MEMARG(n)     MEMARG, n
#define BYTES16       BYTES16, 0
#define MEMARG_LANE(n) MEMARG_LANE, n
#define MEMCOPY       MEMCOPY, 0
#define TABLE_INIT    TABLE_INIT, 0
#define TABLE_COPY    TABLE_COPY, 0
#define FENCE         FENCE, 0
#define BLOCK_TYPE    BLOCK_TYPE, 0
#define BR_LABEL      BR_LABEL, 0
#define BR_TABLE      BR_TABLE, 0
#define CALL_INDIRECT CALL_INDIRECT, 0
#define CTRL_DELIM    CTRL_DELIM, 0
#define SELECT_T      SELECT_T, 0

#define WASMP_ROUTE(Name, Op, Tag, N, Eff, Feat) WASMP_DEF_##Tag(Name, Op, N, Eff, Feat)
#define WASMP_OP(Name, Op, Imm, Eff, Feat) WASMP_ROUTE(Name, Op, Imm, #Eff, #Feat)
#include "instructions.inc"
#undef WASMP_OP
#undef WASMP_ROUTE

#undef NONE
#undef LOCAL_IDX
#undef GLOBAL_IDX
#undef TABLE_IDX
#undef FUNC_IDX
#undef TYPE_IDX
#undef DATA_IDX
#undef ELEM_IDX
#undef MEM_IDX
#undef REF_TYPE
#undef LANE
#undef I32
#undef I64
#undef F32
#undef F64
#undef MEMINIT
#undef MEMARG
#undef BYTES16
#undef MEMARG_LANE
#undef MEMCOPY
#undef TABLE_INIT
#undef TABLE_COPY
#undef FENCE
#undef BLOCK_TYPE
#undef BR_LABEL
#undef BR_TABLE
#undef CALL_INDIRECT
#undef CTRL_DELIM
#undef SELECT_T

#undef WASMP_DEF_NONE
#undef WASMP_DEF_LOCAL_IDX
#undef WASMP_DEF_GLOBAL_IDX
#undef WASMP_DEF_TABLE_IDX
#undef WASMP_DEF_FUNC_IDX
#undef WASMP_DEF_TYPE_IDX
#undef WASMP_DEF_DATA_IDX
#undef WASMP_DEF_ELEM_IDX
#undef WASMP_DEF_MEM_IDX
#undef WASMP_DEF_I32
#undef WASMP_DEF_I64
#undef WASMP_DEF_F32
#undef WASMP_DEF_F64
#undef WASMP_DEF_REF_TYPE
#undef WASMP_DEF_LANE
#undef WASMP_DEF_MEMARG
#undef WASMP_DEF_MEMINIT
#undef WASMP_DEF_BYTES16
#undef WASMP_DEF_MEMARG_LANE
#undef WASMP_DEF_MEMCOPY
#undef WASMP_DEF_TABLE_INIT
#undef WASMP_DEF_TABLE_COPY
#undef WASMP_DEF_FENCE
#undef WASMP_DEF_BLOCK_TYPE
#undef WASMP_DEF_BR_LABEL
#undef WASMP_DEF_BR_TABLE
#undef WASMP_DEF_CALL_INDIRECT
#undef WASMP_DEF_CTRL_DELIM
#undef WASMP_DEF_SELECT_T
#undef WASMP_EMIT_IDX_
#undef WASMP_GATE
#undef WASMP_V
#undef WASMP_ENSURE_

    // --- Terminal / reuse --------------------------------------------------
    [[nodiscard]] FunctionBody Finish() {
        WASMP_ASSERT(control_.empty(), "unbalanced control frames at Finish()");
        if constexpr (kValidate) v_.OnFunctionEnd();
        code_.PutU8(0x0B);  // the function body's terminating `end`
        CodeBuffer locals;
        locals.PutULEB(local_runs_.size());
        for (const auto& [t, count] : local_runs_) {
            locals.PutULEB(count);
            locals.PutU8(static_cast<u8>(t));
        }
        return FunctionBody(FunctionBody::Passkey{}, std::move(locals), std::move(code_));
    }

    void Reset() {
        code_.Reset();
        local_runs_.clear();
        num_locals_ = 0;
        control_.clear();
        if constexpr (kValidate) InitValidator();
    }

    // (Re)bind the signature and reset — makes the signature-later ctor usable
    // and lets one emitter be reused across functions of different types.
    // Defined in module.hpp (needs the complete Module). Call before emitting.
    void SetSignature(TypeIdx sig);

private:
    friend class ControlFrame<Policy>;
    friend class Module;

    static constexpr bool kValidate = std::is_same_v<Policy, policy::Validate>;

    // Defined in module.hpp (needs the complete Module for the signature spans).
    void InitValidator();

    enum class FrameKind : u8 { Block, Loop, If };

    Frame PushFrame(u32 opcode, FrameKind kind, BlockType bt) {
        if constexpr (kValidate) v_.OnCtrlStart(static_cast<u8>(kind), bt);
        u8* p = code_.CursorEnsure(detail::kMaxOpBytes + 1 + detail::kMaxSLEB33);
        p = detail::EmitOp(p, opcode);
        p = detail::EmitBlockType(p, bt);
        code_.SetEnd(p);
        const u32 depth = static_cast<u32>(control_.size());
        control_.push_back(kind);
        return Frame(typename Frame::Passkey{}, this, depth);
    }

    void FrameEnd(u32 depth) {
        WASMP_ASSERT(!control_.empty() && depth + 1 == control_.size(),
                     "control frames closed out of order");
        (void)depth;
        if constexpr (kValidate) v_.OnEnd();
        code_.PutU8(0x0B);
        control_.pop_back();
    }

    void FrameElse(u32 depth) {
        WASMP_ASSERT(depth + 1 == control_.size() && control_.back() == FrameKind::If,
                     "Else() on a non-if or non-innermost frame");
        (void)depth;
        if constexpr (kValidate) v_.OnElse();
        code_.PutU8(0x05);
    }

    u32 RelDepth(const Frame& f) const noexcept {
        return static_cast<u32>(control_.size()) - 1 - f.depth_;
    }

    void EmitBranch(u32 opcode, u32 rel) {
        u8* p = code_.CursorEnsure(detail::kMaxOpBytes + detail::kMaxULEB32);
        p = detail::EmitOp(p, opcode);
        p = detail::PutULEB(p, rel);
        code_.SetEnd(p);
    }

    Module* mod_ = nullptr;
    TypeIdx sig_{};
    u32 param_count_ = 0;
    CodeBuffer code_;
    std::vector<std::pair<ValType, u32>> local_runs_;
    u32 num_locals_ = 0;
    std::vector<FrameKind> control_;
    WASMP_NO_UNIQUE_ADDRESS
    std::conditional_t<kValidate, detail::Validator, detail::NoValidator> v_;
};

// ControlFrame out-of-line members (need the complete FunctionEmitter).
template <typename Policy>
inline void ControlFrame<Policy>::Else() {
    WASMP_ASSERT(live_ && owner_, "Else() on a closed frame");
    owner_->FrameElse(depth_);
}
template <typename Policy>
inline void ControlFrame<Policy>::End() {
    if (live_ && owner_) {
        live_ = false;  // set first: if FrameEnd() throws (throwing error policy
        owner_->FrameEnd(depth_);  // + validation failure) the dtor won't re-fire
    }
}

#if defined(WASMP_EXTERN_TEMPLATE)
extern template class FunctionEmitter<policy::Trust>;
extern template class FunctionEmitter<policy::Validate>;
#endif

}  // namespace wasmp

#endif  // WASMP_FUNCTION_EMITTER_HPP
