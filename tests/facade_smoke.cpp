// facade_smoke.cpp
// Compiles (parse/typecheck only) the entire public surface and asserts the
// table-generated instruction methods have the signatures their immediate
// shapes imply. No bodies are ODR-used — everything is checked in unevaluated
// contexts — so this passes with -fsyntax-only against the declaration-only
// facade. This is the Step-0 definition-of-done gate.

#include <array>
#include <tuple>
#include <type_traits>

#include <wasmp/wasmp.hpp>

using namespace wasmp;

// --- Pure-compile-time layers are already realised: re-assert the contract --
static_assert(WasmValType<i32> == ValType::I32);
static_assert(WasmValType<f64> == ValType::F64);
static_assert(SignatureTraits<std::tuple<i32, f32>(i64)>::results.size() == 2);
static_assert(SignatureTraits<void()>::params.size() == 0);

// --- Strong index handles are distinct types (no cross-space conversions) ---
static_assert(!std::is_convertible_v<FuncIdx, TypeIdx>);
static_assert(!std::is_convertible_v<GlobalIdx, LocalIdx>);

// --- ConstExpr alternatives all present -------------------------------------
static_assert(std::is_same_v<
    ConstExpr,
    std::variant<I32Const, I64Const, F32Const, F64Const, GlobalGet, RefNull, RefFunc>>);

// --- The default emitter alias ----------------------------------------------
using Fn = Function;  // FunctionEmitter<DefaultPolicy>

// Member-pointer type checks: prove the X-macro produced the right signatures
// for every immediate shape in the seed table. Taking &Fn::X in decltype does
// NOT ODR-use the (still bodyless) method.
static_assert(std::is_same_v<decltype(&Fn::Nop),        void (Fn::*)()>);
static_assert(std::is_same_v<decltype(&Fn::I32Add),     void (Fn::*)()>);
static_assert(std::is_same_v<decltype(&Fn::LocalGet),   void (Fn::*)(LocalIdx)>);
static_assert(std::is_same_v<decltype(&Fn::GlobalSet),  void (Fn::*)(GlobalIdx)>);
static_assert(std::is_same_v<decltype(&Fn::I32Load),    void (Fn::*)(MemArg)>);
static_assert(std::is_same_v<decltype(&Fn::I32Store),   void (Fn::*)(MemArg)>);
static_assert(std::is_same_v<decltype(&Fn::I32Const),   void (Fn::*)(i32)>);
static_assert(std::is_same_v<decltype(&Fn::I64Const),   void (Fn::*)(i64)>);
static_assert(std::is_same_v<decltype(&Fn::F32Const),   void (Fn::*)(f32)>);
static_assert(std::is_same_v<decltype(&Fn::F64Const),   void (Fn::*)(f64)>);
static_assert(std::is_same_v<decltype(&Fn::MemorySize), void (Fn::*)(MemIdx)>);
static_assert(std::is_same_v<decltype(&Fn::RefFunc),    void (Fn::*)(FuncIdx)>);
static_assert(std::is_same_v<decltype(&Fn::RefNull),    void (Fn::*)(RefType)>);
static_assert(std::is_same_v<decltype(&Fn::Call),       void (Fn::*)(FuncIdx)>);
static_assert(std::is_same_v<decltype(&Fn::MemoryInit), void (Fn::*)(DataIdx)>);
static_assert(std::is_same_v<decltype(&Fn::DataDrop),   void (Fn::*)(DataIdx)>);
static_assert(std::is_same_v<decltype(&Fn::I8x16Add),   void (Fn::*)()>);
static_assert(std::is_same_v<decltype(&Fn::I8x16ExtractLaneS), void (Fn::*)(u8)>);

// M4 proposal shapes.
static_assert(std::is_same_v<decltype(&Fn::V128Const),  void (Fn::*)(const std::array<u8, 16>&)>);
static_assert(std::is_same_v<decltype(&Fn::MemoryCopy), void (Fn::*)(MemIdx, MemIdx)>);
static_assert(std::is_same_v<decltype(&Fn::TableInit),  void (Fn::*)(ElemIdx, TableIdx)>);
static_assert(std::is_same_v<decltype(&Fn::V128Load8Lane), void (Fn::*)(MemArg, u8)>);
static_assert(std::is_same_v<decltype(&Fn::ReturnCall), void (Fn::*)(FuncIdx)>);
static_assert(std::is_same_v<decltype(&Fn::AtomicFence), void (Fn::*)()>);

// The hand-written structured-control API exists with the intended shapes.
static_assert(std::is_same_v<decltype(&Fn::CallIndirect),
                             void (Fn::*)(TypeIdx, TableIdx)>);
static_assert(std::is_same_v<decltype(&Fn::ReturnCallIndirect),
                             void (Fn::*)(TypeIdx, TableIdx)>);

// --- Name every remaining public type (forces the declarations to be sane) --
using FrameT = Fn::Frame;
using BodyT  = FunctionBody;
using BtT    = BlockType;

// Reference key Module signatures in an unevaluated context.
template <typename T> T declval_() noexcept;
[[maybe_unused]] static void surface_check() {
    static_assert(std::is_same_v<decltype(declval_<Module>().FuncTypeOf<i32(f32)>()),
                                 TypeIdx>);
    static_assert(std::is_same_v<decltype(declval_<Module>().Assemble()),
                                 std::vector<u8>>);
    static_assert(std::is_same_v<decltype(declval_<Fn>().Block(BlockType{})),
                                 FrameT>);
    (void)sizeof(FrameT);
    (void)sizeof(BodyT);
    (void)sizeof(BtT);
}

int main() { return 0; }
