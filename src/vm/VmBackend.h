#pragma once
#include <nlang/compiler/ICodeBackend.h>
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/StdLib.h"
#include "nlang/vm/TypeDesc.h"
#include "BytecodeEmitter.h"
//Macro-generated per-node-kind Visit methods; VmBackend doubles as the
//accessor (Access overloads below) for emission dispatch.
#include <nlang/compiler/SyntaxNodeVisitor.h>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include <functional>

namespace nlang {

class SnExpression;
class SnField;
class SnStatement;
class SnFunction;
class SnEnumDecl;
class SnInvokeExpr;
class SnClassDecl;
class SnSubscriptAssignStmt;
class SnStructDecl;
class SnFieldExpr;
class SnLiteralExpr;
class SnBinaryExpr;
class SnCastExpr;
class SnAsExpr;
class SnSubscriptExpr;
class SnMemberExpr;
class SnAssignStmt;
class SnCompoundAssignStmt;

//Phase 9c: forward-declared so EmitBinding/EmitCallArgs can take
//references without including SnExpressions.h (heavy header dep). Full
//type defined in SnExpressions.h.
struct FormalBinding;
//Phase 8e-6 init-list entry; the init-list emitters below hand entries
//to their per-entry helpers. Defined in SnExpressions.h.
struct InitEntry;
//Library declaration index (nlang_langservice); codegen reads stdlib
//signatures from it. Pointer only, never owned.
namespace langservice { class SymbolIndex; struct SymbolInfo; }

//Phase 5: the compile-time module registry (src/compiler/builder/
//ModuleRegistry.h) — the package/qualified-name seam KeyOf spells table
//keys from. Pointer only, never owned.
class ModuleRegistry;

//Phase 9d: the built-in Exception family — synthetic declarations carry
//no AST fields but a fixed two-field (message/backtrace) runtime layout.
//Shared by field-offset lookup (EmitExprMember.cpp) and the registration
//side layout injection (RegisterClass.cpp).
inline bool IsBuiltinExceptionName(const std::string& name) {
    return name == "Exception" || name == "NullPointerException"
        || name == "DivByZeroException"
        || name == "IndexOutOfBoundsException"
        || name == "AssertionException" || name == "IOException";
}

class VmBackend : public ICodeBackend {
public:
    VmBackend();
    ~VmBackend() override;

    void OnModuleCreate(Module& module) override;
    void GenerateTypes(SnNamespace& root) override;
    void GenerateData(SnNamespace& root) override;
    void GenerateStatements(SnNamespace& root) override;
    bool SaveModule(BuildEnvironment& env) override;

    //Write an arbitrary compiled module as this build's .ncu artifact
    //(same output-path logic as SaveModule). Phase 6: BuildArtifacts
    //single-file mode saves the ENTRY UNIT IMAGE through here (project
    //mode packs the .npkg instead and never calls this) — the backend's
    //own m_compiledModule is a moved-out husk after GenerateUnits.
    bool WriteModuleArtifact(BuildEnvironment& env,
                             const CompiledModule& module,
                             const std::string& entryKey);

    //Phase 6 per-unit codegen: reset the per-unit tables and select the
    //unit whose members the registration walks will see. unitIdx ==
    //MERGED_MODE selects the legacy single-pass mode (whole tree, no
    //owner filtering) — it serves only the pre-flip ModuleBuilder::
    //Build() merge path, whose deletion is the d2 step.
    void BeginUnit(uint32_t unitIdx, const std::string& modulePath);
    static constexpr uint32_t MERGED_MODE = 0xFFFFFFFFu;

    //Own-unit test for the per-unit walks (Register.cpp). MERGED_MODE
    //keeps everything own.
    bool IsOwnUnit(const SnField& member) const;

    //Cross-unit reference slots (SymbolSlots.hpp): resolve a bound
    //declaration to its unit-local table slot — own declarations via the
    //existing lookups, cross-unit ones via placeholder records + import
    //entries. MERGED_MODE never creates slots (everything is local).
    uint32_t FunctionSlotFor(SnFunction& callee);
    uint32_t ClassSlotFor(SnClassDecl& decl);
    uint32_t StructSlotFor(SnStructDecl& decl);
    uint32_t EnumSlotFor(SnEnumDecl& decl);

    //Ctor slot for a class that already has its class slot: own/builtin
    //records carry constructorIdx (PopulateClassMethods and the builtin
    //writers); a cross-unit placeholder carries no metadata, so the ctor
    //declaration is discovered in the AST (the Name()==class-name rule)
    //and slotted through FunctionSlotFor. 0xFFFF = no ctor.
    uint16_t CtorSlotFor(SnClassDecl& decl, uint16_t classSlot);

    //Phase 6 per-unit build: one image per unit in unitIdxs (module
    //identity = registry ModulePathOf). Every cross-unit reference in a
    //unit's code lands as a placeholder slot + import record; nothing is
    //baked in. entryKey is the program entry (empty = none) — at most
    //one unit provides it (the merged-namespace front end rejects
    //duplicate top-level names at resolve; the per-unit gate in
    //FindEntryCandidate routes the entry to its owning unit's image).
    struct UnitBuildResult
    {
        std::vector<CompiledModule> units;
        std::string entryKey;
    };
    UnitBuildResult GenerateUnits(SnNamespace& root,
                                  const std::vector<uint32_t>& unitIdxs);

    //Phase 9c cross-module import infrastructure: inject compiled modules
    //loaded from .ncu files. Must be called before GenerateStatements.
    //ModuleBuilder transfers ownership here so GenerateStatements can
    //access the imported modules when merging them into the user module.
    void SetImportedModules(std::vector<CompiledModule> mods)
    {
        m_importedModules = std::move(mods);
    }

    //Inject the library declaration index (stdlib/*.n signatures) before
    //GenerateStatements. Codegen reads param/return types from it; the
    //pointer is borrowed, not owned.
    void SetLibraryIndex(const langservice::SymbolIndex* pIndex)
    {
        m_pLibraryIndex = pIndex;
    }

    //Phase 5: the package/qualified-name seam lives on the compile-time
    //registry. Codegen must build the SAME string the resolver shows the
    //user, so it borrows the registry (never owns it; it outlives codegen
    //inside one Build() call). The env pointer is the diagnostic channel:
    //VmBackend has no m_Env member (env arrives by value in Build() and by
    //reference in SaveModule()), so the entry-point scan and the D5 native
    //package diagnostics need it injected.
    void SetModuleRegistry(const ModuleRegistry* pRegistry,
        BuildEnvironment* pEnv)
    {
        m_pRegistry = pRegistry;
        m_pEnv = pEnv;
    }

    //Phase 5: canonical VM key of a declaration — "<package>.<name>", or
    //the bare name when the node carries no owner tag. Every name written
    //into or looked up in the VM tables goes through here, so the spelling
    //cannot drift from what the resolver reports. Requires the registry
    //injected by ModuleBuilder. Defined in Register.cpp.
    std::string KeyOf(const SnField& field) const;

    //Phase 9c cross-module: register an imported function stub to its
    //source-module index pair. CompiledModuleNodeBuilder produces stubs;
    //ModuleBuilder.LoadImports iterates the builder's ImportedFunctions()
    //and registers each here. Later, MergeImportedFinalize looks up the
    //merged func index via this side-table to fill m_funcIndexMap[stub].
    void RegisterImportedFunctionStub(SnFunction *stub, uint32_t srcModIdx,
                                       uint32_t srcFuncIdx)
    {
        m_importedFuncSourceIdx[stub] = {srcModIdx, srcFuncIdx};
    }

    const CompiledModule& Result() const { return m_compiledModule; }

    //--- Emission visitor (2026-09-25 maintainability refactor) ---
    //EmitExpression/EmitStatement are thin entries: they store the emitter
    //and result slot into m_pCurrEmitter/m_resultOffset, then dispatch
    //here through m_EmitVisitor. One overload per node kind; each method
    //snapshots the context members into locals at entry (nested emission
    //rewrites them). Unlisted kinds hit the base-class fallbacks
    //Access(SnExpression&)/Access(SnStatement&) with the same
    //unhandled-kind throw as the pre-refactor chain tails.
    void Access(SnArrayTypeExpr&);   //round-13: type reference reaching codegen = invariant break
    void Access(SnGenericTypeExpr&); //same
    void Access(SnQualifiedTypeExpr&); //same (Phase 4b)
    void Access(SnLiteralExpr&);
    void Access(SnIdentifierExpr&);
    void Access(SnInvokeExpr&);
    void Access(SnOutArgExpr&);      //out argument in value position (pre-existing throw)
    void Access(SnCastExpr&);
    void Access(SnAsExpr&);
    void Access(SnMemberExpr&);
    void Access(SnNameExpr&);
    void Access(SnNewExpr&);
    void Access(SnThisExpr&);
    void Access(SnNewArrayExpr&);
    void Access(SnInitListExpr&);
    void Access(SnSubscriptExpr&);
    void Access(SnBinaryExpr&);
    void Access(SnExpression&);      //fallback: unhandled expression kind
    void Access(SnReturnStmt&);
    void Access(SnInvokeStmt&);
    void Access(SnParagraph&);
    void Access(SnLocalDeclStmt&);
    void Access(SnAssignStmt&);
    void Access(SnAssertStmt&);
    void Access(SnCompoundAssignStmt&);
    void Access(SnSubscriptAssignStmt&);
    void Access(SnIfStmt&);
    void Access(SnWhileStmt&);
    void Access(SnDoStmt&);
    void Access(SnForStmt&);
    void Access(SnForeachStmt&);
    void Access(SnBreakStmt&);
    void Access(SnContinueStmt&);
    void Access(SnSwitchStmt&);
    void Access(SnTryStmt&);
    void Access(SnThrowStmt&);
    void Access(SnSuperCallStmt&);
    void Access(SnStatement&);       //fallback: unhandled statement kind
    void Access(SyntaxNode&);        //fallback: non-emissible node (internal error)

private:
    void GenerateFunction(SnFunction& func, size_t funcIdx);

    //pResult reload before any accumulator-reading opcode (the cast_f2i
    //quirk and the "s" + (a+b) dedup bug — see the definition comment in
    //EmitCall.cpp). Consumed by every emission path.
    static void EmitPResultRefresh(BytecodeEmitter& emitter, uint16_t slot);

    //Shared shape predicates, consumed by both the emission members and
    //the frame-size walkers (Walkers*.cpp) so the dispatch
    //decision cannot drift between codegen and walkers.
    static bool IsContainerSubscript(SnExpression& baseExpr);
    static bool IsDelegateInvoke(const SnInvokeExpr& invoke);

    //Flattened member-field offset helpers (definitions live in
    //EmitExprMember.cpp), shared by the member-read, assign-store,
    //and new-expression emitters across emission TUs.
    static int FindFieldOffset(SnStructDecl& structDecl,
                               const std::string& fieldName);
    static int FindClassFieldOffset(SnClassDecl& classDecl,
                                    const std::string& fieldName);
    //FindClassFieldOffset arm: one ancestor in the root→parent walk —
    //returns the field offset when the field lives there, else -1; `off`
    //always advances past that ancestor's data fields.
    static int ClassAncestorFieldOffset(SnClassDecl& ancestor,
                                        const std::string& fieldName,
                                        uint16_t& off);
    static SnClassDecl* OwningClassOfMemberField(SnField* pField);

    //--- Member-expression emission helpers (definitions in
    //EmitExprMember*.cpp, split by responsibility: reads and
    //properties in EmitExprMember.cpp, the method-call family
    //in EmitExprMemberCall.cpp, the string-method/toString
    //family in EmitExprMemberString.cpp). Like every Access
    //helper they
    //take the emission context (emitter + resultOffset) as parameters:
    //m_pCurrEmitter/m_resultOffset are overwritten by nested emission
    //and must never be read inside a helper.
    //Forward-declared here; defined below with the Phase 9c/9e call
    //machinery (the helpers only pass them by reference).
    struct ArgBoxPlan;
    struct OutSpill;

    //Access(SnMemberExpr&) dispatch phases, in evaluation order; each
    //bool phase returns true when it emitted the expression.
    bool EmitMemberHeaderDispatch(SnMemberExpr& member, SnField* field,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset);
    bool EmitMemberBuiltinDispatch(SnMemberExpr& member,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    bool EmitMemberTypedReceiverDispatch(SnMemberExpr& member,
                                         SnField* outerType,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset);
    bool EmitMemberSubscriptElementRead(SnMemberExpr& member,
                                        SnFieldExpr* inner,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset);
    void EmitMemberStringMethodTail(SnMemberExpr& member, SnFieldExpr* inner,
                                    SnField* outerType,
                                    BytecodeEmitter& emitter,
                                    uint16_t resultOffset);
    //Field read tails: offset lookup + OP_LoadField into resultOffset,
    //shared by the direct and arr[i].field read paths.
    void EmitStructFieldLoad(SnStructDecl& structDecl,
                             const std::string& fieldName,
                             BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitClassFieldLoad(SnClassDecl& classDecl,
                            const std::string& fieldName,
                            BytecodeEmitter& emitter, uint16_t resultOffset);
    //Array .length builtin property.
    bool EmitMemberArrayLengthProperty(SnMemberExpr& member,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset);
    //Bound method reference in value position (c.foo) — Func handle.
    void EmitMemberFuncHandleRef(SnMemberExpr& member, SnField* field,
                                 BytecodeEmitter& emitter,
                                 uint16_t resultOffset);
    //User-defined enum method call.
    bool EmitMemberEnumMethodCall(SnMemberExpr& member,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset);
    //Non-class receiver toString() dispatch (array/string/enum/int/float).
    bool EmitMemberToStringNonClass(SnMemberExpr& member,
                                    BytecodeEmitter& emitter,
                                    uint16_t resultOffset);
    bool EmitMemberArrayToString(SnMemberExpr& member,
                                 BytecodeEmitter& emitter,
                                 uint16_t resultOffset);
    bool EmitMemberEnumToString(SnMemberExpr& member, SnField* outerType,
                                BytecodeEmitter& emitter,
                                uint16_t resultOffset);
    bool EmitMemberEnumLiteralToString(SnMemberExpr& member,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset);
    bool EmitMemberNumericToString(SnMemberExpr& member, NodeKind outerKind,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    //Interface method call — always virtual by name.
    void EmitMemberInterfaceCall(SnMemberExpr& member,
                                 BytecodeEmitter& emitter,
                                 uint16_t resultOffset);
    //Class method call family: func-handle toString / delegate / generic.
    void EmitMemberClassMethodCall(SnInvokeExpr& invoke,
                                   SnClassDecl& classDecl,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    void EmitMemberDelegateInvoke(SnInvokeExpr& invoke,
                                  SnClassDecl& classDecl,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset);
    void EmitMemberGenericMethodCall(SnInvokeExpr& invoke,
                                     SnClassDecl& classDecl,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset);
    //Phase 8e-4 boxing plans for built-in generic method calls.
    void PlanGenericMethodBoxing(SnClassDecl& classDecl,
                                 const std::string& methodName,
                                 std::map<uint16_t, ArgBoxPlan>& argPlans,
                                 bool& returnsBoxed, uint8_t& returnTag);
    void PlanListMethodBoxing(const std::vector<SnField*>& typeArgs,
                              const std::string& methodName,
                              std::map<uint16_t, ArgBoxPlan>& argPlans,
                              bool& returnsBoxed, uint8_t& returnTag);
    void PlanDictMethodBoxing(const std::vector<SnField*>& typeArgs,
                              const std::string& methodName,
                              std::map<uint16_t, ArgBoxPlan>& argPlans,
                              bool& returnsBoxed, uint8_t& returnTag);
    //Class method dispatch tail: virtual / direct / builtin + unbox,
    //assign, out spills, ParaEnd.
    void EmitMemberMethodDispatch(const SnInvokeExpr& invoke,
                                  SnFunction* callee, bool returnsBoxed,
                                  uint8_t returnTag,
                                  const std::vector<OutSpill>& outSpills,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset);
    void EmitMemberDirectMethodCall(SnFunction* callee,
                                    const std::vector<OutSpill>& outSpills,
                                    BytecodeEmitter& emitter);
    void EmitMemberBuiltinMethodCall(const SnInvokeExpr& invoke,
                                     const std::vector<OutSpill>& outSpills,
                                     BytecodeEmitter& emitter);
    //String receiver builtin methods (intrinsic-dispatched).
    void EmitMemberStringHashCode(SnMemberExpr& member,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset);
    void EmitMemberStringEquals(SnInvokeExpr& invoke, SnMemberExpr& member,
                                BytecodeEmitter& emitter,
                                uint16_t resultOffset);
    bool EmitMemberTableStringMethod(SnInvokeExpr& invoke,
                                     SnMemberExpr& member,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset);

    //Phase 9c follow-up: compute per-function call slot statistics for
    //dynamic frame sizing. Returns {maxArgs, peakDepth} where:
    //  maxArgs   = max callee formal count (+1 for method `this`) across all
    //              InvokeExpr in the function body. Determines callParamBase size.
    //  peakDepth = max simultaneous evalArea slot need across all call sites.
    //              Determines evalArea size. Computed as the maximum over all
    //              InvokeExpr of: claimSize + max(peakDepth of arg sub-exprs,
    //              peakDepth of callee default expressions).
    struct CallSlotStats { uint16_t maxArgs; uint16_t peakDepth; };
    static CallSlotStats ComputeCallSlotStats(SnFunction& sn);

    //GenerateFunction decomposition (2026-09-25): each phase stamps one
    //slice of the CompiledFunction record / frame layout, in call order.
    //FuncContext is defined below in the private section (forward-declared
    //here so these declarations can refer to it, same as PerModuleRemap).
    struct FuncContext;
    //v1.12: stamp paramTypeDescs + returnTypeDesc from the AST signature.
    void CollectSignatureTypeDescs(SnFunction& func, CompiledFunction& compiledFunc);
    //Phase 9f: fill the signature-only record for a `native` declaration.
    void FillNativeFunctionRecord(SnFunction& func, CompiledFunction& compiledFunc);
    //Allocate the `this` + formal slots, then collect foldable defaults.
    void AllocParamsAndDefaults(SnFunction& func, FuncContext& ctx,
                                CompiledFunction& compiledFunc);
    //Reserve return slot, temp pool, callParamBase and evalArea; returns
    //the call-slot stats consumed by the finalize-time drift check.
    CallSlotStats ReserveReturnAndCallSlots(SnFunction& func, FuncContext& ctx,
                                            CompiledFunction& compiledFunc);
    //Emit the body statements, then the implicit fallback return.
    void EmitBodyAndImplicitReturn(SnFunction& func, FuncContext& ctx,
                                   BytecodeEmitter& emitter);
    //Finalize-time tripwire: evalArea claims must fit the reserved peakDepth.
    void CheckEvalAreaWalkerDrift(SnFunction& func, FuncContext& ctx,
                                  const CallSlotStats& stats);

    //Recursive frame-depth walkers behind ComputeCallSlotStats.
    //visited guards recursion through callee default expressions.
    //isMethodContext=true when the InvokeExpr is the Inner() of a
    //MemberExpr (method-call shape reserving slot 0 for `this`).
    static uint16_t ExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited,
        bool isMethodContext = false);
    static uint16_t StmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //ExprPeakDepth arms — one expression shape family each; the node-kind
    //dispatch stays in ExprPeakDepth.
    //NK_InvokeExpr: call claim + arg/default-expression depth.
    static uint16_t InvokeExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited, bool isMethodContext);
    //NK_BinaryExpr (also unary: Right()==nullptr).
    static uint16_t BinaryExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SubscriptExpr: 2-slot claim over the deeper operand.
    static uint16_t SubscriptExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_MemberExpr: receiver + method-call inner (string-method trailing
    //arg reservation included).
    static uint16_t MemberExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_NewExpr: {this, args...} claim + arg depth.
    static uint16_t NewExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_NewArrayExpr: 1-slot size-expression claim.
    static uint16_t NewArrayExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_InitListExpr: per-entry claim (Dict 3 / List 2 / other 1).
    static uint16_t InitListExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //StmtPeakDepth arms — one statement shape each.
    //NK_Paragraph: max over child statements.
    static uint16_t ParagraphPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ReturnStmt: result-expression depth.
    static uint16_t ReturnStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_AssignStmt: lvalue + RHS depth + member/identifier claim.
    static uint16_t AssignStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_IfStmt: condition claim + then/else branches.
    static uint16_t IfStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_WhileStmt: condition claim + body.
    static uint16_t WhileStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_DoStmt: condition claim + body.
    static uint16_t DoStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ForStmt: init/InitExtras/cond/fini/body.
    static uint16_t ForStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SwitchStmt: condition + case labels + case/default bodies.
    static uint16_t SwitchStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ForeachStmt: iterable + body.
    static uint16_t ForeachStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_AssertStmt: condition claim.
    static uint16_t AssertStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_TryStmt: try/catch/finally bodies.
    static uint16_t TryStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SuperCallStmt: {this, args...} claim + arg depth.
    static uint16_t SuperCallStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ThrowStmt: thrown-expression claim + depth.
    static uint16_t ThrowStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_CompoundAssignStmt: uniform 3-slot claim + receiver/RHS depth.
    static uint16_t CompoundAssignStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SubscriptAssignStmt: 3-slot claim + array/index/value depth.
    static uint16_t SubscriptAssignStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //MaxArgs walker behind ComputeCallSlotStats (callParamBase sizing):
    //statement/expression recursion collecting the largest call claim
    //into maxArgs; isMethodContext as in ExprPeakDepth.
    static void MaxArgsWalkStmt(SnStatement& stmt, uint16_t& maxArgs);
    static void MaxArgsWalkExpr(SnExpression& expr, uint16_t& maxArgs,
        bool isMethodContext = false);
    //MaxArgsWalkStmt arms.
    //NK_AssignStmt: Left() lvalue + RHS (subscript receivers).
    static void MaxArgsAssignStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_ForStmt: init/InitExtras/cond/fini/body.
    static void MaxArgsForStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_SwitchStmt: condition + labels + case/default bodies.
    static void MaxArgsSwitchStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_ForeachStmt: expansion prelude reserves 2 slots + iterable/body.
    static void MaxArgsForeachStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_TryStmt: try/catch/finally bodies.
    static void MaxArgsTryStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_SuperCallStmt: 1 + argc claim + args.
    static void MaxArgsSuperCallStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_CompoundAssignStmt: Left() receiver + RHS.
    static void MaxArgsCompoundAssignStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_SubscriptAssignStmt: container set() reserves 3 slots + children.
    static void MaxArgsSubscriptAssignStmt(SnStatement& stmt, uint16_t& maxArgs);
    //MaxArgsWalkExpr arms.
    //NK_InvokeExpr: the call's own claim, then nested calls in its args.
    static void MaxArgsInvokeExpr(SnExpression& expr, uint16_t& maxArgs,
        bool isMethodContext);
    //NK_SubscriptExpr: container get() reserves 2 slots.
    static void MaxArgsSubscriptExpr(SnExpression& expr, uint16_t& maxArgs);
    //NK_MemberExpr: method-context flag for a method-call inner.
    static void MaxArgsMemberExpr(SnExpression& expr, uint16_t& maxArgs);
    //NK_NewExpr: ctor claim = 1 (this) + argCount.
    static void MaxArgsNewExpr(SnExpression& expr, uint16_t& maxArgs);
    //NK_InitListExpr: implicit Dict set(3) / List add(2) claims.
    static void MaxArgsInitListExpr(SnExpression& expr, uint16_t& maxArgs);
    void EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                        uint16_t resultOffset);
    void EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter);
    void EmitStatementAnchor(SnStatement& stmt, BytecodeEmitter& emitter);

    //--- Statement emission helpers, one family per statement TU
    //(EmitStmt*.cpp). Same hazard contract as the expression
    //helpers: they take the emitter snapshot as a parameter and never
    //read m_pCurrEmitter/m_resultOffset (overwritten by nested emission).
    //Subscript store (EmitStmtStore.cpp): container set()
    //sugar vs plain array element store.
    void EmitContainerSubscriptSet(SnSubscriptAssignStmt& sub,
                                   BytecodeEmitter& emitter);
    void EmitArrayElementStore(SnSubscriptAssignStmt& sub,
                               BytecodeEmitter& emitter);

    //Assign store (EmitStmtDecl.cpp): bare-identifier and member
    //targets, one helper per destination shape. BareIdentifier
    //dispatches Local vs ThisField; MemberField tries the
    //subscripted-element store first, then the plain class/struct
    //field stores. ResolveMemberFieldOffset is the shared offset
    //lookup (throws when the field is missing) for the class/struct
    //element/member targets.
    uint16_t ResolveMemberFieldOffset(SyntaxNode* elemType,
                                      const std::string& fieldName);
    void EmitAssignBareIdentifier(SnAssignStmt& assign, SnField* field,
                                  BytecodeEmitter& emitter);
    void EmitAssignToLocal(SnAssignStmt& assign, uint16_t offset,
                           SnField* varType, BytecodeEmitter& emitter);
    void EmitAssignToThisField(SnAssignStmt& assign, int fieldOff,
                               BytecodeEmitter& emitter);
    void EmitAssignMemberField(SnAssignStmt& assign,
                               SnMemberExpr& memberExpr,
                               BytecodeEmitter& emitter);
    bool EmitAssignSubscriptElementStore(SnAssignStmt& assign,
                                         SnMemberExpr& memberExpr,
                                         BytecodeEmitter& emitter);
    void EmitAssignContainerElementFieldStore(SnAssignStmt& assign,
        SnSubscriptExpr& sub, SyntaxNode* elemType, uint16_t fieldOff,
        BytecodeEmitter& emitter);
    void EmitAssignArrayElementFieldStore(SnAssignStmt& assign,
        SnSubscriptExpr& sub, SyntaxNode* elemType, uint16_t fieldOff,
        BytecodeEmitter& emitter);
    void EmitAssignClassField(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, SnClassDecl* classDecl,
        BytecodeEmitter& emitter);
    void EmitAssignStructField(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, SnStructDecl* structDecl,
        BytecodeEmitter& emitter);
    void EmitAssignStructFieldDeepCopy(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, uint16_t fieldOff, SnField* fieldType,
        BytecodeEmitter& emitter);
    void EmitAssignStructFieldPlain(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, uint16_t fieldOff,
        BytecodeEmitter& emitter);

    //Compound assign (EmitStmtDecl.cpp): read-modify-write on the bare
    //local / implicit this-field / member field targets. op is the
    //underlying binary operator; int avoids requiring SnBinaryExpr's
    //full definition here (same as EmitCompoundOp).
    void EmitCompoundAssignBareIdentifier(SnCompoundAssignStmt& ca,
        SnField* field, int op, BytecodeEmitter& emitter);
    void EmitCompoundAssignThisField(SnCompoundAssignStmt& ca,
        SnField* field, int fieldOff, int op, BytecodeEmitter& emitter);
    void EmitCompoundAssignMemberField(SnCompoundAssignStmt& ca,
        SnMemberExpr& memberExpr, int op, BytecodeEmitter& emitter);

    //Foreach (EmitStmtForeach.cpp): the user loop variable plus the
    //three uniquified hidden locals of the index-based expansion
    //(iterable ref, counter, cached length). Slot kinds feed GC root
    //tracing — see EmitStmtForeach.cpp / the foreach-lowering doc.
    struct ForeachSlots {
        uint16_t userVarSlot = 0;
        uint16_t iterSlot = 0;
        uint16_t iSlot = 0;
        uint16_t nSlot = 0;
    };
    ForeachSlots AllocForeachLocals(SnForeachStmt& fe, uint8_t elemKind,
                                    bool isArray);
    //Dict arm: inline keys() prelude replacing iterSlot with List<K>.
    void EmitForeachDictKeysPrelude(uint16_t iterSlot,
                                    BytecodeEmitter& emitter);
    //Iteration count into nSlot (OP_ArrayLength vs List.length()).
    void EmitForeachLength(bool isArray, uint16_t iterSlot,
                           uint16_t nSlot, BytecodeEmitter& emitter);
    //Loop start + LoopContext push + i<n condition; returns loopStart
    //for the back-jump. Registers the miss-jump as this loop's break
    //target.
    size_t EmitForeachLoopHead(uint16_t iSlot, uint16_t nSlot,
                               BytecodeEmitter& emitter);
    //Body-prelude: element i into the user slot (LoadElement +
    //struct deep copy vs get() + unbox).
    void EmitForeachLoadElement(bool isArray, SnField* pElemType,
                                uint8_t elemKind,
                                const ForeachSlots& slots,
                                BytecodeEmitter& emitter);
    //Loop tail: i+=1 continue target, back-jump, break/continue
    //patching, LoopContext pop.
    void EmitForeachLoopTail(size_t loopStart, uint16_t iSlot,
                             BytecodeEmitter& emitter);

    //Switch (EmitStmtSwitchTry.cpp): per-clause emission and the
    //clause-exit fixup. EmitSwitchCaseClause appends to clauseExits the
    //jumps the fixup must patch (OP_Case placeholder + last label's
    //miss) and reports the implicit no-fallthrough exit via
    //bodyExitJump. EmitSwitchOneLabelCompare returns the label's
    //miss-jump offset (caller picks next-label vs clause-exit target).
    void EmitSwitchCaseClause(SnCaseClause& clause, uint16_t switchSlot,
                              OpCode compareOp,
                              std::vector<size_t>& clauseExits,
                              size_t& bodyExitJump,
                              BytecodeEmitter& emitter);
    void EmitSwitchLabelCompares(SnCaseClause& clause,
                                 uint16_t switchSlot, OpCode compareOp,
                                 std::vector<size_t>& clauseExits,
                                 BytecodeEmitter& emitter);
    size_t EmitSwitchOneLabelCompare(SnExpression& label,
                                     uint16_t switchSlot,
                                     OpCode compareOp,
                                     BytecodeEmitter& emitter);
    void EmitSwitchClauseExits(
        const std::vector<size_t>& caseStartOffsets,
        const std::vector<std::vector<size_t>>& exitJumps,
        bool hasDefault, size_t locCaseEnd, size_t locEnd,
        BytecodeEmitter& emitter);

    //Try/catch/finally (EmitStmtSwitchTry.cpp). EmitTryCatchClauses
    //registers each handler's tryBlocks entry, emits its body and the
    //completion jump; the returned patch offsets target postTry (or
    //finallyNormal when a finally clause exists). EmitTryFinallyTail
    //lays out the handler + normal finally copies and pops
    //m_finallyStack.
    uint16_t FindCatchExceptionClassIdx(SnCatchClause& pCatch);
    std::vector<size_t> EmitTryCatchClauses(SnTryStmt& ts,
        uint16_t tryStart, uint16_t tryEnd, BytecodeEmitter& emitter);
    void EmitTryFinallyTail(SnStatement* pFinally, uint16_t tryStart,
        size_t tryEndJumpPatch,
        const std::vector<size_t>& catchEndJumpPatches,
        BytecodeEmitter& emitter);

    //Phase 9c: per-argument boxing plan for built-in generic class methods
    //(List<T>, Dict<K,V>). Maps callParamBase slot index to {type tag, needs
    //box}. Empty for user methods (no boxing — values pass as heap idxs).
    struct ArgBoxPlan { uint8_t tag; bool needsBox; };

    //Phase 9e: an out-argument writeback. After the *Out call opcode
    //copies callee frame slot `slotIdx` back into callParamBase, this
    //records the caller local to spill it into.
    struct OutSpill { uint16_t slotIdx; uint16_t localOffset; };

    //Phase 9e: emit the post-call spill code for out arguments:
    //per spill, `OP_VarLocal callParamBase+slotIdx*4; OP_Assign localOffset`.
    void EmitOutSpills(const std::vector<OutSpill>& spills,
                       BytecodeEmitter& emitter);

    //Phase 9e: build the outMask operand from spills (bit i set = staging
    //slot i is out). Caps at 32 slots — kMaxFuncParams is 64, but out
    //params beyond slot 31 are rejected here as unsupported rather than
    //silently dropped.
    static uint32_t BuildOutMask(const std::vector<OutSpill>& spills);

    //Phase 9c: emit each formal's actual-or-default expression into the
    //callParamBase area, applying binding decisions from the resolver.
    //pCallee is the resolved function (may be null for unresolved invokes —
    //in that case, fall back to legacy positional emit). For B_Default
    //entries, evaluates the formal's default expression under a binding
    //override scope that maps earlier formals' names to their slots.
    //slotBase is the callParamBase offset (in slot units) where binding[0]
    //begins — 0 for free functions, 1 for method calls (slot 0 is `this`).
    //pArgPlans (optional): if non-null, applied after each arg's emit —
    //used by built-in generic class methods to box primitive-typed args.
    //pOutSpills (optional): if non-null, filled with one entry per out
    //argument (Phase 9e) so the caller can emit the *Out call opcode and
    //the post-call spills.
    void EmitCallArgs(const SnInvokeExpr& invoke, SnFunction* pCallee,
                      BytecodeEmitter& emitter, size_t slotBase = 0,
                      const std::map<uint16_t, ArgBoxPlan>* pArgPlans = nullptr,
                      uint16_t thisSlot = UINT16_MAX,
                      std::vector<OutSpill>* pOutSpills = nullptr);

    //Phase 9c: emit a single binding (positional/named caller expr or
    //default expression) at callParamBase[slotIdx]. Handles default's
    //override scope and struct deep-copy. Used by EmitCallArgs and by
    //method call paths that need per-arg post-processing (e.g. boxing
    //for built-in generic class methods).
    //Phase 9c: emit a single binding at callParamBase[slotIdx]. For
    //B_Default entries, pushes an override scope mapping earlier formals'
    //names (looked up via bindings[0..bindingIdx-1]) to their slots so the
    //default expression can reference earlier formals by name. Handles
    //struct deep-copy. Caller is responsible for any post-emit per-arg
    //work (e.g. boxing for built-in generic class methods).
    //claimBase: the base offset of the current evalArea claim slice
    //(or callParamBase for non-EmitCallArgs callers).
    void EmitBinding(const FormalBinding* pBindings, size_t bindingIdx,
                     uint16_t slotIdx, size_t slotBase,
                     BytecodeEmitter& emitter,
                     uint16_t thisSlot,
                     uint16_t claimBase);

    //Struct value-semantics tail shared by every deep-copy site: copy
    //the heap subtree src → dst via OP_CopyStruct. Per-unit: a
    //cross-unit struct slots as an import placeholder (StructSlotFor).
    void EmitStructDeepCopy(uint16_t dst, uint16_t src,
                            SnStructDecl& structDecl,
                            BytecodeEmitter& emitter);

    //EmitCallArgs decomposition (2026-09-25): one arm/phase per helper.
    //applyBox flows through as std::function so the per-arg boxing
    //decision stays defined once in EmitCallArgs.

    //Optional per-arg boxing application (built-in generic class methods):
    //box the staged argument in place when a plan marks its slot.
    void ApplyArgBoxPlan(uint16_t slotIdx, uint16_t claimBase,
                         BytecodeEmitter& emitter,
                         const std::map<uint16_t, ArgBoxPlan>* pArgPlans);

    //Stage the receiver into claim slot 0 before method-call bindings.
    void StageReceiverInClaim(size_t slotBase, uint16_t thisSlot,
                              uint16_t claimBase, BytecodeEmitter& emitter);

    //Bulk-copy an evalArea claim slice to callParamBase just before a
    //call. Used by EmitCallArgs.
    void CopyClaimToCallParams(uint16_t claimBase, uint16_t slotCount,
                               BytecodeEmitter& emitter);

    //EmitCallArgs arm: unresolved invoke (pCallee == null) — positional
    //emit plus Phase 13 Step 2 out-argument spill recording.
    void EmitUnresolvedInvokeArgs(const SnInvokeExpr& invoke,
                                  BytecodeEmitter& emitter, size_t slotBase,
                                  uint16_t claimBase,
                                  std::vector<OutSpill>* pOutSpills,
                                  const std::function<void(uint16_t)>& applyBox);

    //EmitCallArgs arm: legacy positional emit (no resolver bindings).
    void EmitLegacyPositionalArgs(const SnInvokeExpr& invoke,
                                  BytecodeEmitter& emitter, size_t slotBase,
                                  uint16_t claimBase,
                                  const std::function<void(uint16_t)>& applyBox);

    //EmitCallArgs arm: resolver bindings (Phase 9c) — param ceiling,
    //default-emission recursion guard, per-binding emission. Returns
    //true when the kMaxFuncParams ceiling fired (caller aborts emission).
    bool EmitFormalBindingArgs(const SnInvokeExpr& invoke,
                               SnFunction* pCallee, BytecodeEmitter& emitter,
                               size_t slotBase, uint16_t claimBase,
                               uint16_t thisSlot,
                               std::vector<OutSpill>* pOutSpills,
                               const std::function<void(uint16_t)>& applyBox);

    //One iteration of the resolved-binding loop: emit + optional boxing
    //+ out-spill target record.
    void EmitOneFormalBinding(const FormalBinding* pBindings,
                              size_t bindingIdx, size_t slotBase,
                              BytecodeEmitter& emitter, uint16_t thisSlot,
                              uint16_t claimBase,
                              std::vector<OutSpill>* pOutSpills,
                              const std::function<void(uint16_t)>& applyBox);

    //Phase 9a: emit a compound-assign arithmetic op (locals[dst] op= locals[src]).
    //op is the underlying binary operator (Add/Sub/Mul/Div/Mod).
    //lhsType determines int/float variant selection.
    //Uses int for op to avoid requiring SnBinaryExpr's full definition here;
    //implementation casts back to SnBinaryExpr::Operator.
    void EmitCompoundOp(int op, BytecodeEmitter& emitter,
                        uint16_t dst, uint16_t src, SnField* lhsType);

    //Compilation phases (called by GenerateStatements in order).
    //Each phase corresponds to a distinct compilation pass over the AST.
    //Future evolution: each phase can become an Accessor for multi-backend support.
    void RegisterBuiltinClasses();
    void RegisterStructs(SnNamespace& root);
    //RegisterStructs phase: build one CompiledStruct (+ parallel field lists).
    void RegisterStructDecl(SnStructDecl& sn);
    void RegisterClasses(SnNamespace& root);
    //RegisterClasses phases: one class record; its post-registration
    //resolution pass; the Phase 8e-1 implicit-Object parent fixup.
    void RegisterClassDecl(SnClassDecl& sn,
        std::unordered_map<std::string, SnClassDecl*>& declMap);
    void CollectInheritedClassFields(SnClassDecl& sn, CompiledClass& cc);
    void AppendBuiltinExceptionFields(CompiledClass& cc);
    void CollectOwnClassFields(SnClassDecl& sn, CompiledClass& cc);
    void ResolveClassMetadata(std::unordered_map<std::string, SnClassDecl*>& declMap);
    //classIdx (not a CompiledClass&) because slot resolution can append
    //import placeholders to m_compiledModule.classes mid-call — a held
    //element reference would dangle across the reallocation, so the
    //helpers re-index per statement.
    void ResolveClassFieldRefs(SnClassDecl* pDecl, size_t classIdx);
    void BuildClassFieldTypeDescs(SnClassDecl* pDecl, size_t classIdx);
    //TypeLeafSlots wiring for BuildTypeDesc: leaves slot through
    //StructSlotFor/ClassSlotFor (own entry or import placeholder).
    TypeLeafSlots LeafSlotResolvers();
    void ApplyImplicitObjectInheritance();
    void ResolveStructClassRefs();
    void RegisterArrayTypes(SnNamespace& root);
    //RegisterArrayTypes walkers: type expressions, declaration fields and
    //formal params, statements, and namespace/class child nodes.
    void RegisterArrayTypeExpr(SnFieldExpr* pTypeExpr);
    void WalkArrayTypeField(SnField& f);
    void WalkArrayTypeStmt(SnStatement& s);
    //Per-unit: only this unit's declarations contribute array types (a
    //foreign unit's array entries belong to its own image; a shared
    //element type slots through StructSlotFor/ClassSlotFor).
    void WalkArrayTypeNode(SnField& n);
    void RegisterEnums(SnNamespace& root);
    void RegisterFunctions(SnNamespace& root);
    void PopulateClassMethods(SnNamespace& root);
    void GenerateAllBytecode(SnNamespace& root);
    //GenerateAllBytecode close-out: scan the root for the entry function
    //and stamp m_compiledModule.entryPoint. Defined in Register.cpp (it
    //shares the registry spelling helpers with the key registration).
    void ResolveEntryPoint(SnNamespace& root);
    //ResolveEntryPoint worker: this unit's entry candidate from the
    //merged root — the unique project member function `main`. Methods,
    //library units, and (in per-unit builds) other units' mains are
    //not candidates. Null when the unit has none.
    SnFunction* FindEntryCandidate(SnNamespace& root);
    //Phase 5 D5, run at the FillNativeFunctionRecord call side: returns
    //true (after logging) when a native's package is multi-segment — the
    //host DLL is chosen by the first dot segment (phase 6 lifts this).
    bool RejectMultiSegmentNativePackage(SnFunction& func);

    //Recursively visit every declaration node under a function-parent
    //(namespace/class/interface) at any depth; enum methods are visited as
    //functions. fn runs on each node before descending. Replaces the old
    //fixed two-level (root -> parent -> member) traversal so a type nested
    //inside a library namespace (root -> ns -> class -> method) is reached.
    void ForEachDeclNode(SnField& parent,
        const std::function<void(SnField&)>& fn);
    //GenerateAllBytecode worker: recursive, maintaining m_pCurrClass as it
    //descends into a class so method bodies resolve the right receiver.
    void GenerateBytecodeRecursive(SnField& parent);

    //Phase 9c cross-module: Phase A merges imported classes/structs/arrayTypes
    //(and builds per-module stringMap + classMap/structMap/arrayTypeMap) right
    //after RegisterBuiltinClasses. Partial class metadata remap (superClassIdx,
    //fieldClassIndices, fieldStructIndices) and arrayType elemTypeIdx remap are
    //done here; methodIndices/constructorIdx are deferred to Phase B because they
    //need functionMap (which is built in Phase B since RegisterFunctions.clear()
    //would wipe Phase A data).
    void MergeImportedClassesStructsArrays();
    //Phase 9c cross-module: Phase B runs after RegisterFunctions. Builds
    //enumMap + functionMap, copies imported bytecode and patches indices via
    //RemapBytecode, completes class metadata remap (methodIndices,
    //constructorIdx), and fills m_funcIndexMap[stub] via side-table lookup.
    void MergeImportedFinalize();
    //Phase 9c cross-module: walk a bytecode buffer and patch cross-module
    //index operands (string/func/class/struct/array/enum) using the
    //per-module remap tables. Used by Phase B when copying each imported
    //function's bytecode into the merged module.
    //PerModuleRemap is defined below in the private section (forward-declared
    //here so the declaration can refer to it).
    struct PerModuleRemap;
    void RemapBytecode(std::vector<uint8_t>& bc, const PerModuleRemap& pm);

    //Phase A stages: per-module dedup/push of the type tables, then the
    //partial metadata remap on the pushed copies (per kind, in order).
    void MergeImportedTypeTables();
    void RemapImportedTypeMetadata();
    void RemapImportedClassMetadata(CompiledModule& im, PerModuleRemap& pm);
    void RemapImportedStructMetadata(CompiledModule& im, PerModuleRemap& pm);
    void RemapImportedArrayTypeMetadata(CompiledModule& im, PerModuleRemap& pm);
    //Phase B stages (run in this order): placeholder push, bytecode copy,
    //method-index remap, stub table fill.
    void PushImportedEnumAndFunctionPlaceholders();
    void PushImportedFunctionPlaceholder(CompiledModule& im, PerModuleRemap& pm,
                                         uint32_t i);
    void CopyImportedFunctionBytecode();
    void RemapImportedMethodIndices();
    void BindImportedFunctionStubs();

    //Register an array type from its element type field.
    //Returns the arrayTypeIdx in m_compiledModule.arrayTypes.
    uint16_t RegisterArrayType(SnField* pElemType);

    static uint8_t RuntimeTypeKind(SnField* pType);
    //Return-type kind for .ncu serialization; array-ness is read from the
    //return TYPE EXPRESSION, not from Field() (which resolves to the
    //element field and would degrade `int[]` to RTK_Int32).
    static uint16_t SerializedReturnKind(SnFunction& func);
    //Option B Step 3: extract a constant-foldable default expression into
    //a DefaultValueDesc for serialization. Returns tag=RTK_Void when the
    //expression isn't foldable (caller-side check rejects for IsImported).
    DefaultValueDesc ExtractDefaultValue(SnExpression* pExpr);
    //ExtractDefaultValue arms: direct literal, and OP_Neg over a literal.
    void ExtractLiteralDefault(SnLiteralExpr* lit, DefaultValueDesc& dv);
    void ExtractNegatedLiteralDefault(SnBinaryExpr* bin, DefaultValueDesc& dv);
    //Phase 8e-4: RTK_* tag for boxing a primitive-T argument, plus an
    //isPrimitive flag (needed because RTK_Int32 == 0 — same collision as
    //the Phase 8e-3 C1 fix). Shared by List<T> and Dict<K,V> codegen.
    struct BoxingTagResult { uint8_t tag; bool isPrimitive; };
    static BoxingTagResult BoxingTagFor(SnField* pT);
    //Emit one operand into its claim slot, boxing it in place when
    //the plan says primitive (the OP_Box sequence needs a pResult
    //refresh first). Used by the container subscript SET sugar path;
    //the get path keeps its own inline boxing (EmitExprCast.cpp).
    void EmitBoxedOperand(SnExpression& operand, uint16_t slot,
                          const BoxingTagResult& box,
                          BytecodeEmitter& emitter);
    uint16_t AllocLocal(const std::string& name, uint16_t size,
                        uint8_t typeKind, bool isParam);
    uint16_t FindLocal(const std::string& name) const;
    uint16_t AddStringConstant(const std::string& s);

    //--- Access(SnXExpr) decomposition (2026-09-25 maintainability
    //refactor): one helper per emission sub-path. Each receives the
    //emission context (emitter + resultOffset) explicitly — helpers must
    //never read m_pCurrEmitter/m_resultOffset, which nested emission
    //overwrites mid-flight.

    //SnIdentifierExpr arms: default-param binding-override read, enum
    //member constant, function handle, local/implicit-this field tail.
    void EmitIdentifierOverrideRead(BytecodeEmitter& emitter,
                                    uint16_t resultOffset,
                                    uint16_t overrideSlot);
    void EmitIdentifierEnumMemberRead(BytecodeEmitter& emitter,
                                      uint16_t resultOffset, SnField* field);
    void EmitIdentifierFuncHandleRead(BytecodeEmitter& emitter,
                                      uint16_t resultOffset, SnField* field);
    void EmitIdentifierFieldRead(BytecodeEmitter& emitter,
                                 uint16_t resultOffset, SnField* field);

    //SnInvokeExpr arms: delegate invoke (callee handle + dispatch) and
    //the free-function OP_CallFunc/OP_CallFuncOut tail.
    void EmitDelegateInvoke(const SnInvokeExpr& invoke,
                            BytecodeEmitter& emitter, uint16_t resultOffset,
                            const std::vector<OutSpill>& outSpills);
    void EmitDelegateDispatch(BytecodeEmitter& emitter, uint16_t calleeSlot,
                              uint16_t resultOffset,
                              const std::vector<OutSpill>& outSpills);
    void EmitFreeFunctionCall(int funcIndex, BytecodeEmitter& emitter,
                              uint16_t resultOffset,
                              const std::vector<OutSpill>& outSpills);

    //SnCastExpr arms: boxing, the to-string special cases (enum detection,
    //func/class/array), and the numeric/primitive-string conversions.
    void EmitCastBoxOp(SnCastExpr& cast, BytecodeEmitter& emitter,
                       uint16_t resultOffset);
    bool EmitCastToStringOp(SnCastExpr& cast, NodeKind srcKind,
                            NodeKind dstKind, SnField* sourceType,
                            BytecodeEmitter& emitter, uint16_t resultOffset);
    bool EmitCastEnumToString(SnCastExpr& cast, NodeKind srcKind,
        SnField* sourceType, BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitCastFuncToString(BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitCastClassToString(BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitCastArrayToString(BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitCastNumericOrStringOp(NodeKind srcKind, NodeKind dstKind,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);

    //SnAsExpr arms: box / unbox / downcast, and the `f as string` render.
    void EmitAsBoxOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                     uint16_t resultOffset);
    void EmitAsUnboxOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                       uint16_t resultOffset);
    void EmitAsDowncastOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                          uint16_t resultOffset);
    bool EmitAsFuncToString(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                            uint16_t resultOffset);

    //SnSubscriptExpr arms: List/Dict get() sugar — classification, then
    //the claimed receive/index/call emission.
    void EmitContainerSubscriptGet(SnSubscriptExpr& sub,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    void EmitContainerGetCall(SnSubscriptExpr& sub, BoxingTagResult keyBox,
                              BoxingTagResult valBox,
                              BytecodeEmitter& emitter, uint16_t resultOffset);

    //SnBinaryExpr arms: unary, short-circuit And/Or, and the binary tail
    //(operand staging + opcode dispatch into the arithmetic/relational/
    //equality family emitters).
    void EmitUnaryOp(SnBinaryExpr& bin, BytecodeEmitter& emitter,
                     uint16_t resultOffset);
    void EmitShortCircuitOp(SnBinaryExpr& bin, BytecodeEmitter& emitter,
                            uint16_t resultOffset);
    void EmitShortCircuitAnd(SnExpression& leftChild, SnExpression& rightChild,
                             BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitShortCircuitOr(SnExpression& leftChild, SnExpression& rightChild,
                            BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitBinaryOp(SnBinaryExpr& bin, BytecodeEmitter& emitter,
                      uint16_t resultOffset);
    void EmitBinaryOpCode(SnBinaryExpr& bin, bool isFloat, bool isString,
                          bool isFunc, BytecodeEmitter& emitter,
                          uint16_t resultOffset, uint16_t rightSlot);
    void EmitBinaryArithmeticOp(SnBinaryExpr& bin, bool isFloat, bool isString,
                                BytecodeEmitter& emitter, uint16_t resultOffset,
                                uint16_t rightSlot);
    void EmitBinaryRelationalOp(SnBinaryExpr& bin, bool isFloat, bool isString,
                                BytecodeEmitter& emitter, uint16_t resultOffset,
                                uint16_t rightSlot);
    void EmitBinaryEqualityOp(SnBinaryExpr& bin, bool isFloat, bool isString,
                              bool isFunc, BytecodeEmitter& emitter,
                              uint16_t resultOffset, uint16_t rightSlot);

    //SnNewExpr arms: Round-12 class-resolution guards, positional ctor-arg
    //count, and the ctor-invocation emission ({this, args} claim → OP_New
    //→ OP_CallMethodDirect).
    int ResolveNewExprClassIdx(SnNewExpr& newExpr);
    static int CountNewExprCtorArgs(SnNewExpr& newExpr);
    void EmitCtorInvocation(SnNewExpr& newExpr, BytecodeEmitter& emitter,
                            uint16_t classIdx, uint16_t ctorIdx,
                            uint16_t resultOffset);

    //SnInitListExpr arms: one per target shape (array / List<T> /
    //Dict<K,V> / user class / struct). EmitNewObjectAndNoArgCtor is the
    //OP_New + optional no-arg-ctor sequence shared by the class-shaped
    //arms; classDecl feeds CtorSlotFor (placeholders have no table
    //ctor). The per-entry helpers stage values in evalArea claims.
    void EmitInitListArray(SnInitListExpr& initList, SnField* pElemField,
                           BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitInitListArrayEntry(const InitEntry& entry, int32_t entryIndex,
                                SnField* pElemField, BytecodeEmitter& emitter,
                                uint16_t resultOffset, uint16_t valueSlot);
    void EmitNewObjectAndNoArgCtor(SnClassDecl& classDecl, uint16_t classIdx,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    void EmitInitListListForm(SnInitListExpr& initList, SnClassDecl& classDecl,
                              BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitInitListEntryAdd(const InitEntry& entry, BoxingTagResult tBox,
                              uint16_t addNameIdx, uint16_t resultOffset,
                              BytecodeEmitter& emitter);
    void EmitInitListDictForm(SnInitListExpr& initList, SnClassDecl& classDecl,
                              BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitInitListEntrySet(const InitEntry& entry, BoxingTagResult kBox,
                              BoxingTagResult vBox, uint16_t setNameIdx,
                              uint16_t resultOffset, BytecodeEmitter& emitter);
    void EmitInitListClassForm(SnInitListExpr& initList, SnClassDecl& classDecl,
                               BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitInitListStructForm(SnInitListExpr& initList,
                                SnStructDecl& structDecl,
                                BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitInitListStructEntries(SnInitListExpr& initList,
                                   SnStructDecl& structDecl,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    //Per-entry call tail shared by the List "add" and Dict "set" emitters:
    //this (resultOffset) into claim slot 0, bulk-copy the claim to
    //callParamBase, then OP_CallMethod methodName.
    void EmitInitListMethodCallTail(uint16_t claimBase, uint16_t slotCount,
                                    uint16_t methodNameIdx,
                                    uint16_t resultOffset,
                                    BytecodeEmitter& emitter);

    //SnForStmt arms: claim-staged condition + JumpIfNot placeholder
    //(returns the patch offset), and the shared break/continue patch.
    size_t EmitForCondition(SnForStmt& forStmt, BytecodeEmitter& emitter);
    void EmitLoopExitFixups(size_t breakTarget, size_t continueTarget,
                            BytecodeEmitter& emitter);

    //Codegen-side mirror of the resolver's identifier binding order for a
    //bare identifier that resolved to an SnField:
    //  1. binding override scope (default-param formal) — checked by the
    //     caller via LookupOverride() before calling here
    //  2. local frame (params + user locals) — localOffsets
    //  3. implicit this.<classField> — bare member access inside a method
    //  4. NotFound — resolver/codegen model divergence (internal error)
    //Single source of truth shared by the IdentifierExpr read, AssignStmt
    //write, and CompoundAssignStmt paths so the three stay in lockstep.
    struct BareIdTarget {
        enum Kind { Local, ThisField, NotFound };
        Kind kind = NotFound;
        uint16_t localOffset = 0;     //Local
        SnClassDecl* owner = nullptr; //ThisField: owning class decl
        int fieldOff = -1;            //ThisField: flattened field offset
    };
    BareIdTarget ResolveBareIdentifier(SnField* field);

    //Receiver slot for implicit this.<field> access: local 0 (`this` in a
    //method body), or the caller-side this-override slot when emitting a
    //method-call default-param expression that references a bare field.
    uint16_t ImplicitThisSlot() const
    {
        auto ovr = LookupThisOverride();
        return ovr.first ? ovr.second : 0;
    }

    //Emission dispatch state (see the Access block above): the visitor is
    //bound to *this at construction; thin entries store the per-call
    //context here right before Accept.
    SyntaxNodeVisitor<VmBackend> m_EmitVisitor;
    BytecodeEmitter* m_pCurrEmitter = nullptr;
    uint16_t m_resultOffset = 0;

    //v2.0: the entry function's qualified key (ResolveEntryPoint), e.g.
    //"main.main". Serialized so the loader can re-resolve the index —
    //the index itself is table-layout-dependent and not serialized.
    std::string m_entryKey;
    CompiledModule m_compiledModule;
    std::unordered_map<SnFunction*, size_t> m_funcIndexMap;

    //Phase 6 per-unit mode. MERGED_MODE (sentinel) = legacy single-pass;
    //otherwise the registration walks only see members owned by this
    //unit, and cross-unit references become import slots.
    uint32_t m_currentUnitIdx = MERGED_MODE;
    std::unordered_map<SnEnumDecl*, size_t> m_enumIndexMap;  //Phase 8e-9b: AST enum decl → enumDefIdx (parallel to m_compiledModule.enumNames)
    //v1.12: per-struct resolved field types (parallel to the own-struct
    //entries) — captured at registration, descriptors and field
    //struct/class indices built from the nodes once all tables exist
    //(names alone cannot carry cross-unit provenance for the slots).
    std::vector<std::vector<SnField*>> m_structFieldTypes;
    int16_t m_objectClassIdx = -1;  //Phase 8e-1: index of synthesized Object class (-1 until RegisterBuiltinClasses)
    int16_t m_listClassIdx = -1;    //Phase 8e-3: index of List<T> built-in class (-1 until RegisterBuiltinClasses)
    int16_t m_dictClassIdx = -1;    //Phase 8e-4: index of Dict<K,V> built-in class (-1 until RegisterBuiltinClasses)
    int16_t m_exceptionClassIdx = -1;   //Phase 9d: Exception base class
    int16_t m_nullPtrExcClassIdx = -1;  //Phase 9d: NullPointerException
    int16_t m_divZeroExcClassIdx = -1;  //Phase 9d: DivByZeroException
    int16_t m_oobExcClassIdx = -1;      //Phase 9d: IndexOutOfBoundsException
    int16_t m_assertExcClassIdx = -1;   //Phase 9d: AssertionException
    int16_t m_ioExcClassIdx = -1;       //Phase 11: IOException

    //Phase 9c cross-module import infrastructure (R5-1 + R4-8 + R12-1).
    //Injected by ModuleBuilder before GenerateStatements; consumed by
    //MergeImportedClassesStructsArrays (Phase A) + MergeImportedFinalize
    //(Phase B) during GenerateStatements.
    std::vector<CompiledModule> m_importedModules;
    //Borrowed library declaration index (SetLibraryIndex); null until
    //ModuleBuilder injects it. Codegen reads stdlib signatures from it.
    const langservice::SymbolIndex* m_pLibraryIndex = nullptr;
    //Phase 5: borrowed compile-time registry + diagnostic channel
    //(SetModuleRegistry; one injection point carries both). The registry
    //outlives codegen inside one Build() call; m_pEnv is where the entry
    //scan's ambiguity diagnostic goes (VmBackend has no env of its own).
    const ModuleRegistry* m_pRegistry = nullptr;
    BuildEnvironment* m_pEnv = nullptr;
    //Side-table: imported function stub → (srcModIdx, srcFuncIdx). Filled
    //by ModuleBuilder via RegisterImportedFunctionStub(); read by
    //MergeImportedFinalize to fill m_funcIndexMap[stub] for user codegen.
    std::unordered_map<SnFunction*, std::pair<uint32_t, uint32_t>> m_importedFuncSourceIdx;
    //Per-module index remap tables (R12-1: persisted from Phase A to B).
    //Cleared at the start of Phase A each GenerateStatements call.
    struct PerModuleRemap {
        std::unordered_map<uint32_t, uint32_t> stringMap;
        std::unordered_map<uint32_t, uint32_t> functionMap;   //filled in Phase B
        std::unordered_map<uint32_t, uint32_t> classMap;
        std::unordered_map<uint32_t, uint32_t> structMap;
        std::unordered_map<uint32_t, uint32_t> arrayTypeMap;
        std::unordered_map<uint32_t, uint32_t> enumMap;       //filled in Phase B
        //R8-1: track which source idx was push-new (vs dedup to existing).
        std::unordered_set<uint32_t> classWasPushed;
        std::unordered_set<uint32_t> structWasPushed;
    };
    std::vector<PerModuleRemap> m_importRemaps;

    //Functions whose default-parameter expressions are currently being
    //emitted (EmitCallArgs recursion guard — round-9, finding 3). Re-entry
    //means the default expansion recurses infinitely (compile-time stack
    //overflow pre-fix); the emitter rejects it with a clean error.
    std::unordered_set<SnFunction*> m_defaultEmitting;

    //Per-loop code generation context.
    struct LoopContext {
        std::vector<size_t> breakJumps;
        std::vector<size_t> continueJumps;
        bool isSwitch = false;  //true for switch contexts, false for loops
        //Phase 9d follow-up: snapshot of m_catchBodyDepth at loop entry.
        //break/continue exiting this loop must pop
        //(m_catchBodyDepth - catchBodyDepthAtEntry) OP_PopHandler entries
        //to balance handlerExcStack for the catch bodies it exits through.
        int catchBodyDepthAtEntry = 0;
        //Phase 9d-2: snapshot of m_finallyStack size at loop entry.
        //break/continue exiting this loop must run (inline copies of) the
        //finally bodies added after the loop was entered, i.e.
        //m_finallyStack entries with index >= finallyDepthAtEntry.
        int finallyDepthAtEntry = 0;
    };
    std::vector<LoopContext> m_loopStack;

    //Phase 9d follow-up: tracks how many catch bodies we're currently
    //lexically nested inside. Used by break/continue to emit the right
    //number of OP_PopHandler instructions when a jump exits one or more
    //catch bodies (specifically: when the target loop sits outside a
    //catch body that the break/continue is lexically inside).
    int m_catchBodyDepth = 0;

    //Phase 9d-2: stack of enclosing finally bodies (innermost last). A
    //break/continue/return that leaves the corresponding try region must
    //execute a copy of each finally body it passes through, innermost
    //first. Exception paths are covered separately by the catch-all
    //finally handler (see EmitStatement NK_TryStmt).
    std::vector<SnStatement*> m_finallyStack;

    //Phase 9d follow-up: push a loop context with catch-body depth snapshot.
    void PushLoopContext(bool isSwitch = false) {
        LoopContext ctx;
        ctx.isSwitch = isSwitch;
        ctx.catchBodyDepthAtEntry = m_catchBodyDepth;
        ctx.finallyDepthAtEntry = static_cast<int>(m_finallyStack.size());
        m_loopStack.push_back(std::move(ctx));
    }

    //Per-function code generation context.
    //Layout of the local variable frame (all slots are VALUE_SIZE=4 bytes):
    //  [params...] [returnSlot] [tempSlot..tempSlot4] [callParamBase(N)] [evalArea(peakDepth)] [user locals...]
    //N = max callee formal count seen in this function's body (min 1).
    //peakDepth = max simultaneous evalArea slot need across all call sites.
    //The 4-slot temp pool is pure scratch (each use consumed immediately);
    //live operands across nested emission stage in the evalArea via
    //EvalAreaClaim. Historically PickTempSlot chained temps for nested
    //binaries (pre-8e-1.5 had only 2 slots, clobbering outer-left operands);
    //Phase 10 audit rounds replaced every live-value use with claims and
    //removed PickTempSlot entirely — temps must never hold a live value.
    struct FuncContext {
        CompiledFunction* func = nullptr;
        std::unordered_map<std::string, uint16_t> localOffsets;  //name -> frame offset
        uint16_t nextOffset = 0;     //next free offset in the frame
        uint16_t tempSlot = 0;       //temp pool slot 0 (binary op left / condition eval)
        uint16_t tempSlot2 = 0;      //temp pool slot 1
        uint16_t tempSlot3 = 0;      //temp pool slot 2
        uint16_t tempSlot4 = 0;      //temp pool slot 3
        uint16_t returnSlot = 0;     //slot for function return value
        uint16_t callParamBase = 0;  //base of N-slot area for call arguments
        uint16_t callParamSlots = 0; //N = max callee formal count in body
        uint16_t evalAreaBase = 0;   //base of evalArea (disjoint from callParamBase)
        uint16_t evalAreaCursor = 0; //current claim offset in evalArea (stack-disciplined)
        uint16_t observedPeakCursor = 0; //max evalAreaCursor seen — asserted ≤ walker's peakDepth at function finalize (walker drift must fail loudly at compile time, not OOB at runtime)
        //Phase 8e-5: per-function counter for foreach hidden-local uniquification.
        //AllocLocal dedupes by name (VmBackend.cpp:2194); without uniquification,
        //nested foreach loops would collide on __foreach_iter / __foreach_i / __foreach_n.
        uint16_t foreachCounter = 0;
    };
    FuncContext* m_currFunc = nullptr;

    //Phase 9d-2: the class whose methods are currently being generated.
    //super(...) statements look up the parent class ctor through it.
    //Set in GenerateAllBytecode's class loop; nullptr elsewhere.
    SnClassDecl* m_pCurrClass = nullptr;

    //Phase 9c: identifier binding override stack for default-parameter
    //evaluation. When emitting a default expression that references earlier
    //formal params (e.g., foo(int a, int b = a + 1) called as foo(5)), the
    //resolver has decided each formal's slot in callParamBase. EmitCallArgs
    //pushes a scope mapping earlier formals' names to their slots before
    //evaluating the default; IdentifierExpr emit consults LookupOverride()
    //first and emits OP_VarLocal of the bound slot when matched.
    //
    //The stack (not a single map) supports nested default evaluation: a
    //default expression that itself calls a function with defaults would
    //push another scope recursively.
    std::vector<std::map<std::string, uint16_t>> m_OverrideStack;

    //When evaluating a default-param expression inside a method call,
    //`this` must resolve to callParamBase[0] (the caller-side slot where
    //the receiver was placed), NOT the hardcoded local 0. The default
    //expression is emitted in the caller's context, where local 0 is
    //the caller's own return slot or a temp — not `this`. This stack
    //is pushed/popped alongside OverrideScope; NK_ThisExpr consults
    //the top entry. A value of UINT16_MAX means "no override" (use
    //the normal method-internal path).
    std::vector<uint16_t> m_ThisOverrideStack;

    //RAII guard for a single binding-override scope. Pushes an empty map
    //on construction, pops on destruction (exception-safe).
    class OverrideScope {
        VmBackend& m_Backend;
    public:
        explicit OverrideScope(VmBackend& b) : m_Backend(b)
        {
            m_Backend.m_OverrideStack.emplace_back();
            //Inherit `this` from the enclosing scope (or mark as
            //unbound if no scope is active). This lets nested default
            //evaluation still see the right `this`.
            if (m_Backend.m_ThisOverrideStack.empty())
                m_Backend.m_ThisOverrideStack.push_back(UINT16_MAX);
            else
                m_Backend.m_ThisOverrideStack.push_back(
                    m_Backend.m_ThisOverrideStack.back());
        }
        ~OverrideScope()
        {
            m_Backend.m_OverrideStack.pop_back();
            m_Backend.m_ThisOverrideStack.pop_back();
        }
        void Add(const std::string& name, uint16_t slot)
        { m_Backend.m_OverrideStack.back()[name] = slot; }
        //Bind `this` to the caller-side callParamBase slot holding the
        //receiver object. Used when emitting method-call default params.
        void BindThis(uint16_t slot)
        { m_Backend.m_ThisOverrideStack.back() = slot; }
    };

    //RAII guard for claiming a slice of the evalArea. Each EmitCallArgs
    //invocation claims N slots (where N = callee formal count + slotBase).
    //Nested calls claim deeper slices, so inner bindings never overwrite
    //outer bindings. On destruction, the cursor restores automatically.
    class EvalAreaClaim {
        VmBackend& m_B;
        uint16_t   m_slots;
    public:
        EvalAreaClaim(VmBackend& b, uint16_t slots) : m_B(b), m_slots(slots)
        {
            auto& ctx = *m_B.m_currFunc;
            ctx.evalAreaCursor += m_slots * 4;
            if (ctx.evalAreaCursor > ctx.observedPeakCursor)
                ctx.observedPeakCursor = ctx.evalAreaCursor;
        }
        ~EvalAreaClaim()
        { m_B.m_currFunc->evalAreaCursor -= m_slots * 4; }
        uint16_t base() const
        { return m_B.m_currFunc->evalAreaBase
              + m_B.m_currFunc->evalAreaCursor
              - m_slots * 4; }
    };

    //Returns {true, slot} if `name` is bound in any active override scope
    //(innermost first), else {false, 0}.
    std::pair<bool, uint16_t> LookupOverride(const std::string& name) const
    {
        for (auto it = m_OverrideStack.rbegin();
             it != m_OverrideStack.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end())
                return {true, found->second};
        }
        return {false, 0};
    }

    //Returns {true, slot} if `this` is bound in the current override scope
    //(method-call default param emit), else {false, 0}.
    std::pair<bool, uint16_t> LookupThisOverride() const
    {
        if (m_ThisOverrideStack.empty())
            return {false, 0};
        uint16_t slot = m_ThisOverrideStack.back();
        if (slot == UINT16_MAX)
            return {false, 0};
        return {true, slot};
    }
};

} // namespace nlang
