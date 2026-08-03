// wasmp — module.hpp
// The sirit-style resource layer: owns the type interner, imports, function
// declarations, tables/memories/globals, exports, segments, custom/name
// sections, and produces the final .wasm bytes via Assemble(). Enforces the
// three-phase lifecycle (declare -> emit -> assemble, DESIGN.md §2.1) via
// debug asserts.
//
// M1: full core module + Assemble. See DESIGN.md §5, §8.
#ifndef WASMP_MODULE_HPP
#define WASMP_MODULE_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "code_buffer.hpp"
#include "common.hpp"
#include "function_emitter.hpp"  // FunctionBody, FunctionEmitter
#include "host_traits.hpp"       // SignatureTraits

namespace wasmp {

using ByteSink = std::function<void(std::span<const u8>)>;

namespace detail {

inline void PutName(CodeBuffer& b, std::string_view s) {
    b.PutULEB(s.size());
    b.PutBytes({reinterpret_cast<const u8*>(s.data()), s.size()});
}

inline void PutLimits(CodeBuffer& b, u64 min, const std::optional<u64>& max,
                      bool shared, bool is64) {
    u8 flags = 0;
    if (max) flags |= 0x01;
    if (shared) flags |= 0x02;
    if (is64) flags |= 0x04;
    b.PutU8(flags);
    b.PutULEB(min);
    if (max) b.PutULEB(*max);
}

inline void PutConstExpr(CodeBuffer& b, const ConstExpr& e) {
    std::visit([&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, I32Const>) { b.PutU8(0x41); b.PutSLEB(v.value); }
        else if constexpr (std::is_same_v<T, I64Const>) { b.PutU8(0x42); b.PutSLEB(v.value); }
        else if constexpr (std::is_same_v<T, F32Const>) { b.PutU8(0x43); b.PutF32(v.value); }
        else if constexpr (std::is_same_v<T, F64Const>) { b.PutU8(0x44); b.PutF64(v.value); }
        else if constexpr (std::is_same_v<T, GlobalGet>) { b.PutU8(0x23); b.PutULEB(Raw(v.global)); }
        else if constexpr (std::is_same_v<T, RefNull>) { b.PutU8(0xD0); b.PutU8(static_cast<u8>(v.type)); }
        else if constexpr (std::is_same_v<T, RefFunc>) { b.PutU8(0xD2); b.PutULEB(Raw(v.func)); }
    }, e);
    b.PutU8(0x0B);  // end
}

}  // namespace detail

class Module {
public:
    Module() noexcept = default;
    explicit Module(Features features) : features_(features) {}
    ~Module() = default;

    Module(Module&&) noexcept = default;
    Module& operator=(Module&&) noexcept = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    const Features& GetFeatures() const noexcept { return features_; }

    // Number of parameters of an interned type — used by FunctionEmitter.
    u32 ParamCount(TypeIdx t) const noexcept {
        return static_cast<u32>(types_[Raw(t)].params.size());
    }

    // Read-only queries over declared entities (used by the validator, §7).
    std::span<const ValType> TypeParams(TypeIdx t) const noexcept { return types_[Raw(t)].params; }
    std::span<const ValType> TypeResults(TypeIdx t) const noexcept { return types_[Raw(t)].results; }
    u32 TypeCount() const noexcept { return static_cast<u32>(types_.size()); }
    u32 FuncCount() const noexcept { return imp_funcs_ + static_cast<u32>(funcs_.size()); }
    u32 GlobalCount() const noexcept { return imp_globals_ + static_cast<u32>(globals_.size()); }
    u32 TableCount() const noexcept { return imp_tables_ + static_cast<u32>(tables_.size()); }

    TypeIdx FuncSig(FuncIdx f) const noexcept {
        const u32 i = Raw(f);
        return i < imp_funcs_ ? NthImport(ExternalKind::Function, i)->type : funcs_[i - imp_funcs_].sig;
    }
    ValType GlobalType(GlobalIdx g) const noexcept {
        const u32 i = Raw(g);
        return i < imp_globals_ ? NthImport(ExternalKind::Global, i)->gtype : globals_[i - imp_globals_].type;
    }
    Mutability GlobalMut(GlobalIdx g) const noexcept {
        const u32 i = Raw(g);
        return i < imp_globals_ ? NthImport(ExternalKind::Global, i)->gmut : globals_[i - imp_globals_].mut;
    }
    RefType TableElem(TableIdx t) const noexcept {
        const u32 i = Raw(t);
        return i < imp_tables_ ? NthImport(ExternalKind::Table, i)->ref : tables_[i - imp_tables_].elem;
    }

    // === Phase 1: DECLARE ==================================================

    TypeIdx FuncType(std::span<const ValType> params, std::span<const ValType> results) {
        // Flat open-addressing interner (DESIGN.md §5): byte-hash the signature,
        // linear-probe the slot table, compare against the stored sequences.
        // Indices are assigned in first-intern order → deterministic output.
        EnsureSlots();
        const u64 h = HashSig(params, results);
        const size_t mask = type_slots_.size() - 1;
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            const i32 slot = type_slots_[i];
            if (slot < 0) {
                const u32 idx = static_cast<u32>(types_.size());
                types_.push_back({{params.begin(), params.end()},
                                  {results.begin(), results.end()}});
                type_slots_[i] = static_cast<i32>(idx);
                return TypeIdx{idx};
            }
            if (SigEq(types_[static_cast<size_t>(slot)], params, results))
                return TypeIdx{static_cast<u32>(slot)};
        }
    }
    TypeIdx FuncType(std::initializer_list<ValType> params,
                     std::initializer_list<ValType> results) {
        return FuncType(std::span<const ValType>(params.begin(), params.size()),
                        std::span<const ValType>(results.begin(), results.size()));
    }
    template <typename Sig>
    TypeIdx FuncTypeOf() {
        return FuncType(SignatureTraits<Sig>::ParamSpan(), SignatureTraits<Sig>::ResultSpan());
    }

    FuncIdx ImportFunction(std::string_view module, std::string_view name, TypeIdx sig) {
        WASMP_ASSERT(funcs_.empty(), "imports must precede function declarations");
        Import e;
        e.kind = ExternalKind::Function;
        e.module = module;
        e.name = name;
        e.type = sig;
        imports_.push_back(std::move(e));
        return FuncIdx{imp_funcs_++};
    }
    TableIdx ImportTable(std::string_view module, std::string_view name, RefType elem,
                         TableLimits limits) {
        WASMP_ASSERT(tables_.empty(), "table imports must precede table definitions");
        Import e;
        e.kind = ExternalKind::Table;
        e.module = module;
        e.name = name;
        e.ref = elem;
        e.tlim = limits;
        imports_.push_back(std::move(e));
        return TableIdx{imp_tables_++};
    }
    MemIdx ImportMemory(std::string_view module, std::string_view name, MemLimits limits) {
        WASMP_ASSERT(mems_.empty(), "memory imports must precede memory definitions");
        Import e;
        e.kind = ExternalKind::Memory;
        e.module = module;
        e.name = name;
        e.mlim = limits;
        imports_.push_back(std::move(e));
        return MemIdx{imp_mems_++};
    }
    GlobalIdx ImportGlobal(std::string_view module, std::string_view name, ValType type,
                           Mutability mut) {
        WASMP_ASSERT(globals_.empty(), "global imports must precede global definitions");
        Import e;
        e.kind = ExternalKind::Global;
        e.module = module;
        e.name = name;
        e.gtype = type;
        e.gmut = mut;
        imports_.push_back(std::move(e));
        return GlobalIdx{imp_globals_++};
    }

    FuncIdx DeclareFunction(TypeIdx sig) {
        WASMP_ASSERT(!phase2_, "cannot declare after the emit phase has begun");
        const FuncIdx idx{imp_funcs_ + static_cast<u32>(funcs_.size())};
        funcs_.push_back(DefinedFunc{sig, std::nullopt});
        return idx;
    }
    TableIdx Table(RefType elem, TableLimits limits) {
        const TableIdx idx{imp_tables_ + static_cast<u32>(tables_.size())};
        tables_.push_back({elem, limits});
        return idx;
    }
    MemIdx Memory(MemLimits limits) {
        const MemIdx idx{imp_mems_ + static_cast<u32>(mems_.size())};
        mems_.push_back(limits);
        return idx;
    }
    GlobalIdx Global(ValType type, Mutability mut, ConstExpr init) {
        const GlobalIdx idx{imp_globals_ + static_cast<u32>(globals_.size())};
        globals_.push_back({type, mut, init});
        return idx;
    }

    void Export(std::string_view name, FuncIdx f) { exports_.push_back({std::string(name), ExternalKind::Function, Raw(f)}); }
    void Export(std::string_view name, TableIdx t) { exports_.push_back({std::string(name), ExternalKind::Table, Raw(t)}); }
    void Export(std::string_view name, MemIdx m) { exports_.push_back({std::string(name), ExternalKind::Memory, Raw(m)}); }
    void Export(std::string_view name, GlobalIdx g) { exports_.push_back({std::string(name), ExternalKind::Global, Raw(g)}); }
    void SetStart(FuncIdx f) { start_ = Raw(f); }

    void ActiveData(MemIdx mem, ConstExpr offset, std::span<const u8> bytes) {
        data_.push_back({false, Raw(mem), offset, {bytes.begin(), bytes.end()}});
    }
    DataIdx PassiveData(std::span<const u8> bytes) {
        const DataIdx idx{static_cast<u32>(data_.size())};
        data_.push_back({true, 0, std::nullopt, {bytes.begin(), bytes.end()}});
        return idx;
    }
    void ActiveElem(TableIdx table, ConstExpr offset, std::span<const FuncIdx> funcs) {
        elem_.push_back({false, RefType::FuncRef, Raw(table), offset, ToRaw(funcs)});
    }
    void ActiveElem(TableIdx table, ConstExpr offset, std::initializer_list<FuncIdx> funcs) {
        ActiveElem(table, offset, std::span<const FuncIdx>(funcs.begin(), funcs.size()));
    }
    ElemIdx PassiveElem(RefType type, std::span<const FuncIdx> funcs) {
        const ElemIdx idx{static_cast<u32>(elem_.size())};
        elem_.push_back({true, type, 0, std::nullopt, ToRaw(funcs)});
        return idx;
    }
    ElemIdx PassiveElem(RefType type, std::initializer_list<FuncIdx> funcs) {
        return PassiveElem(type, std::span<const FuncIdx>(funcs.begin(), funcs.size()));
    }

    void CustomSection(std::string_view name, std::span<const u8> bytes) {
        customs_.push_back({std::string(name), {bytes.begin(), bytes.end()}});
    }
    void SetName(FuncIdx f, std::string_view name) { func_names_.emplace_back(Raw(f), std::string(name)); }
    void SetName(GlobalIdx g, std::string_view name) { global_names_.emplace_back(Raw(g), std::string(name)); }
    void SetLocalName(FuncIdx f, LocalIdx local, std::string_view name) {
        local_names_.push_back({Raw(f), Raw(local), std::string(name)});
    }
    void SetModuleName(std::string_view name) { module_name_ = name; }

    // === Phase 2: EMIT =====================================================
    void DefineFunction(FuncIdx f, FunctionBody&& body) {
        phase2_ = true;
        WASMP_ASSERT(Raw(f) >= imp_funcs_, "cannot define an imported function");
        const u32 i = Raw(f) - imp_funcs_;
        WASMP_ASSERT(i < funcs_.size(), "DefineFunction on an undeclared index");
        funcs_[i].body = std::move(body);
    }

    // === Phase 3: ASSEMBLE =================================================
    std::vector<u8> Assemble() {
        if (auto e = FindError()) detail::ReportError(*e);
        CodeBuffer out;
        AssembleInto(out);
        auto s = out.Bytes();
        return {s.begin(), s.end()};
    }
    void Assemble(const ByteSink& sink) {
        if (auto e = FindError()) detail::ReportError(*e);
        CodeBuffer out;
        AssembleInto(out);
        sink(out.Bytes());
    }

    struct AssembleError { std::string_view what; };
    struct AssembleResult {
        std::vector<u8> bytes;
        std::optional<AssembleError> error;
        explicit operator bool() const noexcept { return !error.has_value(); }
    };
    AssembleResult TryAssemble() {
        AssembleResult r;
        if (auto e = FindError()) {
            r.error = AssembleError{*e};
            return r;
        }
        CodeBuffer out;
        AssembleInto(out);
        auto s = out.Bytes();
        r.bytes.assign(s.begin(), s.end());
        return r;
    }

private:
    struct FuncTypeData { std::vector<ValType> params, results; };
    struct Import {
        ExternalKind kind{};
        std::string module, name;
        TypeIdx type{};
        RefType ref{};
        TableLimits tlim{};
        MemLimits mlim{};
        ValType gtype{};
        Mutability gmut{};
    };
    struct DefinedFunc { TypeIdx sig; std::optional<FunctionBody> body; };
    struct TableDef { RefType elem; TableLimits limits; };
    struct GlobalDef { ValType type; Mutability mut; ConstExpr init; };
    struct ExportEntry { std::string name; ExternalKind kind; u32 index; };
    struct DataSeg { bool passive; u32 mem; std::optional<ConstExpr> offset; std::vector<u8> bytes; };
    struct ElemSeg { bool passive; RefType type; u32 table; std::optional<ConstExpr> offset; std::vector<u32> funcs; };
    struct CustomSec { std::string name; std::vector<u8> bytes; };
    struct LocalName { u32 func; u32 local; std::string name; };

    static std::vector<u32> ToRaw(std::span<const FuncIdx> funcs) {
        std::vector<u32> out;
        out.reserve(funcs.size());
        for (FuncIdx f : funcs) out.push_back(Raw(f));
        return out;
    }

    // --- Type interner internals -------------------------------------------
    static u64 HashSig(std::span<const ValType> params,
                            std::span<const ValType> results) noexcept {
        u64 h = 1469598103934665603ull;  // FNV-1a
        auto mix = [&](u8 b) { h ^= b; h *= 1099511628211ull; };
        for (ValType v : params) mix(static_cast<u8>(v));
        mix(0xFF);  // separator (never a valtype byte)
        for (ValType v : results) mix(static_cast<u8>(v));
        return h;
    }
    static bool SigEq(const FuncTypeData& t, std::span<const ValType> params,
                      std::span<const ValType> results) noexcept {
        return t.params.size() == params.size() && t.results.size() == results.size() &&
               std::equal(t.params.begin(), t.params.end(), params.begin()) &&
               std::equal(t.results.begin(), t.results.end(), results.begin());
    }
    void EnsureSlots() {
        if (type_slots_.empty()) {
            type_slots_.assign(16, -1);
            return;
        }
        if ((types_.size() + 1) * 4 <= type_slots_.size() * 3) return;  // load < 0.75
        std::vector<i32> next(type_slots_.size() * 2, -1);
        const size_t mask = next.size() - 1;
        for (u32 k = 0; k < types_.size(); ++k) {
            size_t i = HashSig(types_[k].params, types_[k].results) & mask;
            while (next[i] >= 0) i = (i + 1) & mask;
            next[i] = static_cast<i32>(k);
        }
        type_slots_ = std::move(next);
    }

    // The n-th import of a given kind (imports share index spaces with defs).
    const Import* NthImport(ExternalKind kind, u32 n) const noexcept {
        u32 c = 0;
        for (const auto& e : imports_)
            if (e.kind == kind && c++ == n) return &e;
        return nullptr;
    }

    std::optional<std::string_view> FindError() const {
        for (const auto& f : funcs_)
            if (!f.body) return std::string_view("function declared but never defined");
        return std::nullopt;
    }

    using Section = std::pair<u8, CodeBuffer>;
    // Collect a built section; the whole module is sized and written in one
    // reserve + pass afterwards (DESIGN.md §5), instead of growing `out`
    // per section.
    static void EmitSection(std::vector<Section>& secs, u8 id, CodeBuffer& sec) {
        secs.emplace_back(id, std::move(sec));
    }

    bool HasPassiveData() const {
        for (const auto& d : data_) if (d.passive) return true;
        return false;
    }

    void AssembleInto(CodeBuffer& out) const {
        const bool is64 = features_.memory64;
        std::vector<Section> secs;

        // 1: Type
        if (!types_.empty()) {
            CodeBuffer s;
            s.PutULEB(types_.size());
            for (const auto& t : types_) {
                s.PutU8(0x60);
                s.PutULEB(t.params.size());
                for (ValType v : t.params) s.PutU8(static_cast<u8>(v));
                s.PutULEB(t.results.size());
                for (ValType v : t.results) s.PutU8(static_cast<u8>(v));
            }
            EmitSection(secs, 1, s);
        }

        // 2: Import
        if (!imports_.empty()) {
            CodeBuffer s;
            s.PutULEB(imports_.size());
            for (const auto& e : imports_) {
                detail::PutName(s, e.module);
                detail::PutName(s, e.name);
                s.PutU8(static_cast<u8>(e.kind));
                switch (e.kind) {
                    case ExternalKind::Function: s.PutULEB(Raw(e.type)); break;
                    case ExternalKind::Table:
                        s.PutU8(static_cast<u8>(e.ref));
                        detail::PutLimits(s, e.tlim.min, e.tlim.max, false, false);
                        break;
                    case ExternalKind::Memory:
                        detail::PutLimits(s, e.mlim.min, e.mlim.max, e.mlim.shared, is64);
                        break;
                    case ExternalKind::Global:
                        s.PutU8(static_cast<u8>(e.gtype));
                        s.PutU8(static_cast<u8>(e.gmut));
                        break;
                }
            }
            EmitSection(secs, 2, s);
        }

        // 3: Function
        if (!funcs_.empty()) {
            CodeBuffer s;
            s.PutULEB(funcs_.size());
            for (const auto& f : funcs_) s.PutULEB(Raw(f.sig));
            EmitSection(secs, 3, s);
        }

        // 4: Table
        if (!tables_.empty()) {
            CodeBuffer s;
            s.PutULEB(tables_.size());
            for (const auto& t : tables_) {
                s.PutU8(static_cast<u8>(t.elem));
                detail::PutLimits(s, t.limits.min, t.limits.max, false, false);
            }
            EmitSection(secs, 4, s);
        }

        // 5: Memory
        if (!mems_.empty()) {
            CodeBuffer s;
            s.PutULEB(mems_.size());
            for (const auto& m : mems_) detail::PutLimits(s, m.min, m.max, m.shared, is64);
            EmitSection(secs, 5, s);
        }

        // 6: Global
        if (!globals_.empty()) {
            CodeBuffer s;
            s.PutULEB(globals_.size());
            for (const auto& g : globals_) {
                s.PutU8(static_cast<u8>(g.type));
                s.PutU8(static_cast<u8>(g.mut));
                detail::PutConstExpr(s, g.init);
            }
            EmitSection(secs, 6, s);
        }

        // 7: Export
        if (!exports_.empty()) {
            CodeBuffer s;
            s.PutULEB(exports_.size());
            for (const auto& e : exports_) {
                detail::PutName(s, e.name);
                s.PutU8(static_cast<u8>(e.kind));
                s.PutULEB(e.index);
            }
            EmitSection(secs, 7, s);
        }

        // 8: Start
        if (start_) {
            CodeBuffer s;
            s.PutULEB(*start_);
            EmitSection(secs, 8, s);
        }

        // 9: Element
        if (!elem_.empty()) {
            CodeBuffer s;
            s.PutULEB(elem_.size());
            for (const auto& e : elem_) {
                if (e.passive) {                       // flag 1: passive funcref
                    s.PutULEB(1);
                    s.PutU8(0x00);                     // elemkind
                    s.PutULEB(e.funcs.size());
                    for (u32 f : e.funcs) s.PutULEB(f);
                } else if (e.table == 0) {             // flag 0: active table 0
                    s.PutULEB(0);
                    detail::PutConstExpr(s, *e.offset);
                    s.PutULEB(e.funcs.size());
                    for (u32 f : e.funcs) s.PutULEB(f);
                } else {                               // flag 2: active explicit table
                    s.PutULEB(2);
                    s.PutULEB(e.table);
                    detail::PutConstExpr(s, *e.offset);
                    s.PutU8(0x00);                     // elemkind
                    s.PutULEB(e.funcs.size());
                    for (u32 f : e.funcs) s.PutULEB(f);
                }
            }
            EmitSection(secs, 9, s);
        }

        // 12: DataCount — required when data-referencing bulk ops may appear.
        if (HasPassiveData()) {
            CodeBuffer s;
            s.PutULEB(data_.size());
            EmitSection(secs, 12, s);
        }

        // 10: Code
        if (!funcs_.empty()) {
            CodeBuffer s;
            s.PutULEB(funcs_.size());
            for (const auto& f : funcs_) {
                const FunctionBody& body = *f.body;
                s.PutULEB(body.EncodedSize());
                s.PutBytes(body.LocalDecls());
                s.PutBytes(body.Code());
            }
            EmitSection(secs, 10, s);
        }

        // 11: Data
        if (!data_.empty()) {
            CodeBuffer s;
            s.PutULEB(data_.size());
            for (const auto& d : data_) {
                if (d.passive) {
                    s.PutULEB(1);
                    s.PutULEB(d.bytes.size());
                    s.PutBytes({d.bytes.data(), d.bytes.size()});
                } else if (d.mem == 0) {
                    s.PutULEB(0);
                    detail::PutConstExpr(s, *d.offset);
                    s.PutULEB(d.bytes.size());
                    s.PutBytes({d.bytes.data(), d.bytes.size()});
                } else {
                    s.PutULEB(2);
                    s.PutULEB(d.mem);
                    detail::PutConstExpr(s, *d.offset);
                    s.PutULEB(d.bytes.size());
                    s.PutBytes({d.bytes.data(), d.bytes.size()});
                }
            }
            EmitSection(secs, 11, s);
        }

        // Custom: name section (module + function names), then user customs.
        EmitNameSection(secs);
        for (const auto& c : customs_) {
            CodeBuffer s;
            detail::PutName(s, c.name);
            s.PutBytes({c.bytes.data(), c.bytes.size()});
            EmitSection(secs, 0, s);
        }

        // Size everything, reserve once, and write the module in a single pass.
        static constexpr u8 kMagic[] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        size_t total = sizeof(kMagic);
        for (const auto& [id, s] : secs)
            total += 1 + detail::SizeULEB(s.Size()) + s.Size();
        out.Reserve(total);
        out.PutBytes(kMagic);
        for (const auto& [id, s] : secs) {
            out.PutU8(id);
            out.PutULEB(s.Size());
            out.PutBytes(s.Bytes());
        }
    }

    // A `namemap`: sorted (index -> name) pairs.
    static void PutNameMap(CodeBuffer& b, std::vector<std::pair<u32, std::string>> m) {
        std::sort(m.begin(), m.end(), [](const auto& a, const auto& c) { return a.first < c.first; });
        b.PutULEB(m.size());
        for (const auto& [idx, nm] : m) {
            b.PutULEB(idx);
            detail::PutName(b, nm);
        }
    }
    static void PutSubsection(CodeBuffer& names, u8 id, const CodeBuffer& sub) {
        names.PutU8(id);
        names.PutULEB(sub.Size());
        names.PutBytes(sub.Bytes());
    }

    void EmitNameSection(std::vector<Section>& secs) const {
        if (module_name_.empty() && func_names_.empty() && local_names_.empty() &&
            global_names_.empty())
            return;
        CodeBuffer names;

        if (!module_name_.empty()) {  // subsection 0: module name
            CodeBuffer sub;
            detail::PutName(sub, module_name_);
            PutSubsection(names, 0, sub);
        }
        if (!func_names_.empty()) {   // subsection 1: function names
            CodeBuffer sub;
            PutNameMap(sub, func_names_);
            PutSubsection(names, 1, sub);
        }
        if (!local_names_.empty()) {  // subsection 2: local names (indirectnamemap)
            auto rows = local_names_;
            std::sort(rows.begin(), rows.end(), [](const LocalName& a, const LocalName& b) {
                return a.func != b.func ? a.func < b.func : a.local < b.local;
            });
            CodeBuffer sub;
            // Count distinct functions first.
            size_t nfuncs = 0;
            for (size_t i = 0; i < rows.size();) {
                const u32 f = rows[i].func;
                ++nfuncs;
                while (i < rows.size() && rows[i].func == f) ++i;
            }
            sub.PutULEB(nfuncs);
            for (size_t i = 0; i < rows.size();) {
                const u32 f = rows[i].func;
                size_t j = i;
                while (j < rows.size() && rows[j].func == f) ++j;
                sub.PutULEB(f);
                sub.PutULEB(j - i);
                for (size_t k = i; k < j; ++k) {
                    sub.PutULEB(rows[k].local);
                    detail::PutName(sub, rows[k].name);
                }
                i = j;
            }
            PutSubsection(names, 2, sub);
        }
        if (!global_names_.empty()) {  // subsection 7: global names
            CodeBuffer sub;
            PutNameMap(sub, global_names_);
            PutSubsection(names, 7, sub);
        }

        CodeBuffer s;
        detail::PutName(s, "name");
        s.PutBytes(names.Bytes());
        EmitSection(secs, 0, s);
    }

    Features features_{};
    std::vector<FuncTypeData> types_;
    std::vector<i32> type_slots_;  // open-addressing slots: -1 empty, else type index
    std::vector<Import> imports_;
    u32 imp_funcs_ = 0, imp_tables_ = 0, imp_mems_ = 0, imp_globals_ = 0;
    std::vector<DefinedFunc> funcs_;
    std::vector<TableDef> tables_;
    std::vector<MemLimits> mems_;
    std::vector<GlobalDef> globals_;
    std::vector<ExportEntry> exports_;
    std::optional<u32> start_;
    std::vector<DataSeg> data_;
    std::vector<ElemSeg> elem_;
    std::vector<CustomSec> customs_;
    std::string module_name_;
    std::vector<std::pair<u32, std::string>> func_names_;
    std::vector<std::pair<u32, std::string>> global_names_;
    std::vector<LocalName> local_names_;
    bool phase2_ = false;
};

// FunctionEmitter constructors need the complete Module (for ParamCount and,
// under policy::Validate, for initialising the validator's control frame).
template <typename Policy>
inline FunctionEmitter<Policy>::FunctionEmitter(Module& mod, TypeIdx signature)
    : mod_(&mod), sig_(signature), param_count_(mod.ParamCount(signature)) {
    if constexpr (kValidate) InitValidator();
}

template <typename Policy>
inline FunctionEmitter<Policy>::FunctionEmitter(Module& mod) : mod_(&mod) {}

template <typename Policy>
inline void FunctionEmitter<Policy>::InitValidator() {
    v_.Init(*mod_, mod_->TypeParams(sig_), mod_->TypeResults(sig_), mod_->GetFeatures());
}

template <typename Policy>
inline void FunctionEmitter<Policy>::SetSignature(TypeIdx sig) {
    sig_ = sig;
    param_count_ = mod_->ParamCount(sig);
    Reset();  // clears body state and (under Validate) re-inits the validator
}

// --- Validator methods that require the complete Module --------------------
namespace detail {

inline void Validator::OnGlobal(Mnemonic m, GlobalIdx g) {
    if (Raw(g) >= mod_->GlobalCount()) Fail("global index out of range");
    const ValType t = mod_->GlobalType(g);
    if (m == Mnemonic::GlobalGet) {
        PushVal(t);
    } else {  // GlobalSet
        if (mod_->GlobalMut(g) != Mutability::Var) Fail("global.set on an immutable global");
        PopExpect(t);
    }
}

inline void Validator::OnFuncIdx(Mnemonic m, FuncIdx f) {
    if (Raw(f) >= mod_->FuncCount()) Fail("function index out of range");
    if (m == Mnemonic::Call || m == Mnemonic::ReturnCall) {
        const TypeIdx s = mod_->FuncSig(f);
        PopVals(mod_->TypeParams(s));
        if (m == Mnemonic::Call) PushVals(mod_->TypeResults(s));
        else Unreachable();  // return_call transfers control away
    } else {  // RefFunc
        PushVal(ValType::FuncRef);
    }
}

inline void Validator::OnTable(Mnemonic m, TableIdx t) {
    if (Raw(t) >= mod_->TableCount()) Fail("table index out of range");
    const ValType elem = ToValType(mod_->TableElem(t));
    switch (m) {
        case Mnemonic::TableGet: PopExpect(ValType::I32); PushVal(elem); break;
        case Mnemonic::TableSet: PopExpect(elem); PopExpect(ValType::I32); break;
        case Mnemonic::TableSize: PushVal(ValType::I32); break;
        case Mnemonic::TableGrow:  // (init: elem, delta: i32) -> i32
            PopExpect(ValType::I32); PopExpect(elem); PushVal(ValType::I32); break;
        case Mnemonic::TableFill:  // (dst: i32, val: elem, n: i32) -> ()
            PopExpect(ValType::I32); PopExpect(elem); PopExpect(ValType::I32); break;
        default: break;
    }
}

inline void Validator::OnCtrlStart(u8 kind, BlockType bt) {
    std::vector<ValType> in, out;
    switch (bt.GetKind()) {
        case BlockType::Kind::Void: break;
        case BlockType::Kind::Value: out.push_back(bt.Value()); break;
        case BlockType::Kind::Type: {
            auto p = mod_->TypeParams(bt.Type());
            auto r = mod_->TypeResults(bt.Type());
            in.assign(p.begin(), p.end());
            out.assign(r.begin(), r.end());
            break;
        }
    }
    if (kind == kIf) PopExpect(ValType::I32);
    PopVals(in);
    PushCtrl(kind, in, out);
}

inline void Validator::OnCallIndirect(TypeIdx sig, TableIdx table) {
    if (Raw(table) >= mod_->TableCount()) Fail("table index out of range");
    if (Raw(sig) >= mod_->TypeCount()) Fail("type index out of range");
    PopExpect(ValType::I32);
    ApplySig(mod_->TypeParams(sig), mod_->TypeResults(sig));
}

inline void Validator::OnReturnCallIndirect(TypeIdx sig, TableIdx table) {
    if (Raw(table) >= mod_->TableCount()) Fail("table index out of range");
    if (Raw(sig) >= mod_->TypeCount()) Fail("type index out of range");
    PopExpect(ValType::I32);
    PopVals(mod_->TypeParams(sig));
    Unreachable();
}

}  // namespace detail

}  // namespace wasmp

#endif  // WASMP_MODULE_HPP
