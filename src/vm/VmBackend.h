#pragma once
#include <nlang/compiler/ICodeBackend.h>
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/StdLib.h"
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

namespace nlang {

class SnExpression;
class SnField;
class SnStatement;
class SnFunction;
class SnEnumDecl;
class SnInvokeExpr;
class SnClassDecl;
class SnStructDecl;
class SnFieldExpr;
class SnLiteralExpr;
class SnBinaryExpr;

//Phase 9c: forward-declared so EmitBinding/EmitCallArgs can take
//references without including SnExpressions.h (heavy header dep). Full
//type defined in SnExpressions.h.
struct FormalBinding;

class VmBackend : public ICodeBackend {
public:
    VmBackend();
    ~VmBackend() override;

    void OnModuleCreate(Module& module) override;
    void GenerateTypes(SnNamespace& root) override;
    void GenerateData(SnNamespace& root) override;
    void GenerateStatements(SnNamespace& root) override;
    bool SaveModule(BuildEnvironment& env) override;

    //Phase 9c cross-module import infrastructure: inject compiled modules
    //loaded from .nmod files. Must be called before GenerateStatements.
    //ModuleBuilder transfers ownership here so GenerateStatements can
    //access the imported modules when merging them into the user module.
    void SetImportedModules(std::vector<CompiledModule> mods)
    {
        m_importedModules = std::move(mods);
    }

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
    //VmBackendEmitCall.cpp). Consumed by every emission path.
    static void EmitPResultRefresh(BytecodeEmitter& emitter, uint16_t slot);

    //Shared shape predicates, consumed by both the emission members and
    //the frame-size walkers (VmBackendWalkers*.cpp) so the dispatch
    //decision cannot drift between codegen and walkers.
    static bool IsContainerSubscript(SnExpression& baseExpr);
    static bool IsDelegateInvoke(const SnInvokeExpr& invoke);

    //Flattened member-field offset helpers (definitions live in
    //VmBackendEmitExprMember.cpp), shared by the member-read, assign-store,
    //and new-expression emitters across emission TUs.
    static int FindFieldOffset(SnStructDecl& structDecl,
                               const std::string& fieldName);
    static int FindClassFieldOffset(SnClassDecl& classDecl,
                                    const std::string& fieldName);
    static SnClassDecl* OwningClassOfMemberField(SnField* pField);

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
    void EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                        uint16_t resultOffset);
    void EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter);
    void EmitStatementAnchor(SnStatement& stmt, BytecodeEmitter& emitter);

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

    //Phase 9a: emit a compound-assign arithmetic op (locals[dst] op= locals[src]).
    //op is the underlying binary operator (Add/Sub/Mul/Div/Mod).
    //lhsType determines int/float variant selection.
    //Uses int for op to avoid requiring SnBinaryExpr's full definition here;
    //implementation casts back to SnBinaryExpr::Operator.
    void EmitCompoundOp(int op, BytecodeEmitter& emitter,
                        uint16_t dst, uint16_t src, SnField* lhsType);

    //Phase 11: emit a namespace-qualified stdlib call (math.sqrt(x)).
    //Mirrors the string.equals emission minus the receiver: args stage in
    //an evalArea claim, then bulk-copy to callParamBase from slot 0 — the
    //namespace intrinsic ABI has no this (see StdLib.h).
    void EmitStdLibCall(const StdLibEntry& entry, SnInvokeExpr& invoke,
                        BytecodeEmitter& emitter, uint16_t resultOffset);

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
    void ResolveClassFieldRefs(SnClassDecl* pDecl, CompiledClass& cc);
    void BuildClassFieldTypeDescs(SnClassDecl* pDecl, CompiledClass& cc);
    void ApplyImplicitObjectInheritance();
    void ResolveStructClassRefs();
    void RegisterArrayTypes(SnNamespace& root);
    //RegisterArrayTypes walkers: type expressions, declaration fields and
    //formal params, statements, and namespace/class child nodes.
    void RegisterArrayTypeExpr(SnFieldExpr* pTypeExpr);
    void WalkArrayTypeField(SnField& f);
    void WalkArrayTypeStmt(SnStatement& s);
    void WalkArrayTypeNode(SyntaxNode& n);
    void RegisterEnums(SnNamespace& root);
    void RegisterFunctions(SnNamespace& root);
    void PopulateClassMethods(SnNamespace& root);
    void GenerateAllBytecode(SnNamespace& root);

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
    //Return-type kind for .nmod serialization; array-ness is read from the
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
    uint16_t AllocLocal(const std::string& name, uint16_t size,
                        uint8_t typeKind, bool isParam);
    uint16_t FindLocal(const std::string& name) const;
    uint16_t AddStringConstant(const std::string& s);

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

    CompiledModule m_compiledModule;
    std::unordered_map<SnFunction*, size_t> m_funcIndexMap;
    std::unordered_map<SnEnumDecl*, size_t> m_enumIndexMap;  //Phase 8e-9b: AST enum decl → enumDefIdx (parallel to m_compiledModule.enumNames)
    std::vector<std::vector<std::string>> m_structFieldTypeNames;
    //v1.12: per-struct resolved field types (parallel to
    //m_structFieldTypeNames) — captured at registration, descriptors
    //built in ResolveStructClassRefs once all struct/class tables exist.
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
