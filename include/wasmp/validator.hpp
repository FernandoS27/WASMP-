// wasmp — validator.hpp
// The debug-only type-stack validator behind policy::Validate (DESIGN.md §7).
// It implements the WebAssembly validation algorithm from the spec appendix,
// including the polymorphic "unreachable mode" stack. The bulk of opcodes are
// driven by the instruction table's stack-effect column (parsed from the
// stringized effect); the value-polymorphic / parametric / control ops get
// hand-written cases here.
//
// Methods that need module-level information (globals, function signatures,
// tables, block types) are declared here and defined in module.hpp, after the
// Module type is complete.
#ifndef WASMP_VALIDATOR_HPP
#define WASMP_VALIDATOR_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace wasmp {

class Module;

// One enumerator per instruction table row — lets the validator switch on the
// specific mnemonic for the hand-written cases.
enum class Mnemonic : u16 {
#define WASMP_OP(Name, Op, Imm, Eff, Feat) Name,
#include "instructions.inc"
#undef WASMP_OP
};

namespace detail {

// Parsed stack effect. `special` marks an effect the generic path cannot apply
// (a type variable like LOCAL_T, or the literal SPECIAL) — those mnemonics are
// handled by hand in the validator.
struct Effect {
    bool special = false;
    u8 npop = 0, npush = 0;
    ValType pop[4]{};
    ValType push[4]{};
};

inline bool EffScalar(std::string_view tok, ValType& out) {
    if (tok == "I32") out = ValType::I32;
    else if (tok == "I64") out = ValType::I64;
    else if (tok == "F32") out = ValType::F32;
    else if (tok == "F64") out = ValType::F64;
    else if (tok == "V128") out = ValType::V128;
    else return false;
    return true;
}

// Parse "(A, B) -> (C)" / "() -> ()" / "SPECIAL". A type variable (anything not
// scalar) makes the whole effect `special`.
inline Effect ParseEffect(std::string_view v) {
    Effect e;
    if (v == "SPECIAL") { e.special = true; return e; }
    const size_t arrow = v.find("->");
    auto group = [&](std::string_view g, ValType* arr, u8& n) -> bool {
        const size_t o = g.find('('), c = g.find(')');
        if (o == std::string_view::npos || c == std::string_view::npos) return false;
        g = g.substr(o + 1, c - o - 1);
        size_t start = 0;
        while (start < g.size()) {
            while (start < g.size() && g[start] == ' ') ++start;
            size_t comma = g.find(',', start);
            size_t end = (comma == std::string_view::npos) ? g.size() : comma;
            size_t te = end;
            while (te > start && g[te - 1] == ' ') --te;
            std::string_view tok = g.substr(start, te - start);
            if (!tok.empty()) {
                ValType t;
                if (!EffScalar(tok, t)) return false;  // parametric -> special
                if (n < 4) arr[n] = t;
                ++n;
            }
            start = (comma == std::string_view::npos) ? g.size() : comma + 1;
        }
        return true;
    };
    if (!group(v.substr(0, arrow), e.pop, e.npop) ||
        !group(v.substr(arrow + 2), e.push, e.npush)) {
        e.special = true;
        e.npop = e.npush = 0;
    }
    return e;
}

// Internal sentinel for a polymorphic (unknown-typed) stack value.
inline constexpr ValType kUnknown{0};

// The validator. State: the declared local types, the operand type stack, and
// the control-frame stack. `mod_` is used only by the Module-defined methods.
class Validator {
public:
    void Init(const Module& mod, std::span<const ValType> params,
              std::span<const ValType> results, const Features& features) {
        mod_ = &mod;
        features_ = features;
        addr_type_ = features.memory64 ? ValType::I64 : ValType::I32;
        locals_.assign(params.begin(), params.end());
        func_results_.assign(results.begin(), results.end());
        vals_.clear();
        ctrls_.clear();
        // Implicit function-body frame: its label/end type is the func result.
        PushCtrl(kFunc, {}, results);
    }

    // Feature-flag gate (§4): reject an opcode whose proposal is disabled for
    // this module. `feat` is the stringized Feature column from the table.
    void CheckFeature(std::string_view feat) {
        const bool ok =
            feat == "MVP"                 ? true :
            feat == "SIGN_EXTENSION"      ? features_.sign_extension :
            feat == "NONTRAPPING_FPTOINT" ? features_.nontrapping_fptoint :
            feat == "REFERENCE_TYPES"     ? features_.reference_types :
            feat == "BULK_MEMORY"         ? features_.bulk_memory :
            feat == "SIMD"                ? features_.simd :
            feat == "ATOMICS"             ? features_.atomics :
            feat == "TAIL_CALL"           ? features_.tail_call :
                                            true;  // unknown tag: do not block
        if (!ok) {
            std::string m = "instruction requires the '";
            m += feat;
            m += "' feature, which is disabled for this module";
            Fail(m);
        }
    }

    void AddLocals(ValType t, u32 count) {
        for (u32 i = 0; i < count; ++i) locals_.push_back(t);
    }

    // --- Table-driven fixed effect (numeric, const, table.init/copy, ...) --
    void OnFixed(std::string_view eff) {
        const Effect e = ParseEffect(eff);
        if (e.special) return;  // should not happen for fixed-shape callers
        PopVals({e.pop, e.npop});
        PushVals({e.push, e.npush});
    }

    // Memory-access ops: like OnFixed, but the address operand (the deepest
    // pop, always encoded I32 in the table) becomes i64 under memory64. Also
    // gates a non-zero memory index on the multi-memory feature.
    void OnMemArg(std::string_view eff, u32 memidx = 0) {
        if (memidx != 0 && !features_.multi_memory)
            Fail("a non-zero memory index requires the multi-memory feature");
        Effect e = ParseEffect(eff);
        if (e.special) return;
        if (e.npop >= 1 && e.pop[0] == ValType::I32) e.pop[0] = addr_type_;
        PopVals({e.pop, e.npop});
        PushVals({e.push, e.npush});
    }

    // Bulk/whole-memory ops (memory.size/grow/fill/copy/init). Under memory64
    // the address-typed operands widen to i64; which operands those are depends
    // on the specific op.
    void OnMemBulk(Mnemonic m, std::string_view eff) {
        Effect e = ParseEffect(eff);
        if (addr_type_ != ValType::I32) {
            const ValType a = addr_type_;
            switch (m) {
                case Mnemonic::MemorySize: e.push[0] = a; break;
                case Mnemonic::MemoryGrow: e.pop[0] = a; e.push[0] = a; break;
                case Mnemonic::MemoryFill: e.pop[0] = a; e.pop[2] = a; break;
                case Mnemonic::MemoryCopy: e.pop[0] = e.pop[1] = e.pop[2] = a; break;
                case Mnemonic::MemoryInit: e.pop[0] = a; break;
                default: break;
            }
        }
        PopVals({e.pop, e.npop});
        PushVals({e.push, e.npush});
    }

    // --- NONE-shape ops: fixed, or a value-polymorphic special -------------
    void OnNone(Mnemonic m, std::string_view eff) {
        const Effect e = ParseEffect(eff);
        if (!e.special) {
            PopVals({e.pop, e.npop});
            PushVals({e.push, e.npush});
            return;
        }
        switch (m) {
            case Mnemonic::Drop: PopVal(); break;
            case Mnemonic::Select: {
                PopExpect(ValType::I32);           // condition
                ValType t2 = PopVal();
                ValType t1 = PopExpect(t2);
                PushVal(t1 == kUnknown ? t2 : t1);
                break;
            }
            case Mnemonic::Return:
                PopVals(func_results_);
                Unreachable();
                break;
            case Mnemonic::Unreachable: Unreachable(); break;
            case Mnemonic::RefIsNull: PopVal(); PushVal(ValType::I32); break;
            default: Fail("unhandled value-polymorphic opcode"); break;
        }
    }

    // --- Locals ------------------------------------------------------------
    void OnLocal(Mnemonic m, LocalIdx local) {
        const u32 i = Raw(local);
        if (i >= locals_.size()) Fail("local index out of range");
        const ValType t = locals_[i];
        switch (m) {
            case Mnemonic::LocalGet: PushVal(t); break;
            case Mnemonic::LocalSet: PopExpect(t); break;
            case Mnemonic::LocalTee: PopExpect(t); PushVal(t); break;
            default: break;
        }
    }

    void OnRefNull(RefType t) { PushVal(ToValType(t)); }

    // Typed select: [a: t, b: t, cond: i32] -> [t].
    void OnSelectT(ValType t) {
        PopExpect(ValType::I32);
        PopExpect(t);
        PopExpect(t);
        PushVal(t);
    }

    // --- Control-frame introspection used by hand-written emitter methods --
    void OnElse() {
        Ctrl f = PopCtrl();
        if (f.kind != kIf) Fail("else without matching if");
        if (f.else_seen) Fail("duplicate else");
        PushCtrl(kIf, f.in, f.out);
        ctrls_.back().else_seen = true;
    }

    void OnEnd() {
        Ctrl& f = ctrls_.back();
        if (f.kind == kIf && !f.else_seen && f.in != f.out)
            Fail("if without else must have matching param/result types");
        Ctrl r = PopCtrl();
        PushVals(r.out);
    }

    void OnFunctionEnd() {
        PopCtrl();  // the implicit function frame; checks results + height
        if (!ctrls_.empty()) Fail("unbalanced control frames at Finish()");
    }

    void OnBr(u32 rel) {
        Ctrl& c = FrameAt(rel);
        PopVals(LabelTypes(c));
        Unreachable();
    }
    void OnBrIf(u32 rel) {
        PopExpect(ValType::I32);
        Ctrl& c = FrameAt(rel);
        auto lt = LabelTypes(c);
        PopVals(lt);
        PushVals(lt);
    }
    void OnBrTable(std::span<const u32> rels, u32 default_rel) {
        PopExpect(ValType::I32);
        auto def = LabelTypes(FrameAt(default_rel));
        for (u32 r : rels) {
            auto lt = LabelTypes(FrameAt(r));
            if (lt.size() != def.size()) Fail("br_table target arity mismatch");
        }
        PopVals(def);
        Unreachable();
    }

    // --- Module-dependent (defined in module.hpp) --------------------------
    void OnGlobal(Mnemonic m, GlobalIdx g);
    void OnFuncIdx(Mnemonic m, FuncIdx f);
    void OnTable(Mnemonic m, TableIdx t);
    void OnCtrlStart(u8 kind, BlockType bt);  // 0=block 1=loop 2=if
    void OnCallIndirect(TypeIdx sig, TableIdx table);
    void OnReturnCallIndirect(TypeIdx sig, TableIdx table);

    static constexpr u8 kBlock = 0, kLoop = 1, kIf = 2, kFunc = 3;

private:
    struct Ctrl {
        u8 kind;
        bool unreachable = false;
        bool else_seen = false;
        u32 height = 0;
        std::vector<ValType> in, out;
    };

    void PushVal(ValType t) { vals_.push_back(t); }
    ValType PopVal() {
        Ctrl& f = ctrls_.back();
        if (vals_.size() == f.height) {
            if (f.unreachable) return kUnknown;
            Fail("operand stack underflow");
        }
        ValType t = vals_.back();
        vals_.pop_back();
        return t;
    }
    ValType PopExpect(ValType e) {
        ValType a = PopVal();
        if (a != kUnknown && e != kUnknown && a != e) Fail("operand type mismatch");
        return a;
    }
    void PushVals(std::span<const ValType> ts) { for (ValType t : ts) PushVal(t); }
    void PopVals(std::span<const ValType> ts) {
        for (size_t i = ts.size(); i-- > 0;) PopExpect(ts[i]);
    }

    void Unreachable() {
        Ctrl& f = ctrls_.back();
        vals_.resize(f.height);
        f.unreachable = true;
    }

    void PushCtrl(u8 kind, std::span<const ValType> in, std::span<const ValType> out) {
        Ctrl c;
        c.kind = kind;
        c.height = static_cast<u32>(vals_.size());
        c.in.assign(in.begin(), in.end());
        c.out.assign(out.begin(), out.end());
        ctrls_.push_back(std::move(c));
        PushVals(in);
    }
    Ctrl PopCtrl() {
        if (ctrls_.empty()) Fail("control stack underflow");
        Ctrl f = std::move(ctrls_.back());
        PopVals(f.out);
        if (vals_.size() != f.height) Fail("operand stack height mismatch at end");
        ctrls_.pop_back();
        return f;
    }
    Ctrl& FrameAt(u32 rel) {
        if (rel >= ctrls_.size()) Fail("branch target out of range");
        return ctrls_[ctrls_.size() - 1 - rel];
    }
    static std::span<const ValType> LabelTypes(const Ctrl& c) {
        return c.kind == kLoop ? std::span<const ValType>(c.in)
                               : std::span<const ValType>(c.out);
    }

    [[noreturn]] void Fail(std::string_view msg) const {
        std::string m = "wasmp validation error: ";
        m += msg;
        ::wasmp::detail::ReportError(m);
    }

    // Shared helper for the Module-defined methods.
    void ApplySig(std::span<const ValType> params, std::span<const ValType> results) {
        PopVals(params);
        PushVals(results);
    }
    friend class ::wasmp::Module;  // Module-defined methods use the privates

    const Module* mod_ = nullptr;
    Features features_{};
    ValType addr_type_ = ValType::I32;  // i64 under memory64
    std::vector<ValType> locals_;
    std::vector<ValType> func_results_;
    std::vector<ValType> vals_;
    std::vector<Ctrl> ctrls_;
};

// The zero-overhead stand-in used when policy == Trust.
struct NoValidator {};

}  // namespace wasmp::detail

}  // namespace wasmp

#endif  // WASMP_VALIDATOR_HPP
