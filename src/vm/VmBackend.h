#pragma once
#include <nlang/compiler/ICodeBackend.h>
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/TypeDesc.h"
#include "BytecodeEmitter.h"
//Intrinsic binding table + package/name projections (compile-time
//declaration metadata; moved compiler-side with the concurrency import
//redesign so pure-compiler link targets resolve).
#include "builder/BindingTable.h"
//Macro-generated per-node-kind Visit methods; VmBackend doubles as the
//accessor (Access overloads below) for emission dispatch.
#include <nlang/compiler/SyntaxNodeVisitor.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
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
class SnInterfaceDecl;
class SnSubscriptAssignStmt;
class SnStructDecl;
class SnClassField;
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
//Compile-time generic instance bookkeeping (the declared templates'
//placeholder and instance tables; nlang/compiler/GenericInstanceRegistry.h).
class GenericInstanceRegistry;

class VmBackend : public ICodeBackend {
public:
    VmBackend();
    ~VmBackend() override;

    void OnModuleCreate(Module& module) override;

    //Write an arbitrary compiled module as this build's .ncu artifact.
    //Phase 6: BuildArtifacts single-file mode saves the ENTRY UNIT IMAGE
    //through here (project mode packs the .npkg instead and never calls
    //this) — the backend's own m_compiledModule is a moved-out husk
    //after GenerateUnits.
    bool WriteModuleArtifact(BuildEnvironment& env,
                             const CompiledModule& module,
                             const std::string& entryKey);

    //Phase 6 per-unit codegen: reset the per-unit tables and select the
    //unit whose members the registration walks will see.
    void BeginUnit(uint32_t unitIdx, const std::string& modulePath);

    //The per-unit registration sequence (builtin classes → structs → classes →
    //array types → enums → functions → member tables → interfaces →
    //enum methods), in the order that lays out the unit's tables. Run
    //for every real unit before its bytecode pass, and by the library
    //shadow registration below (table layout only — no bytecode).
    void RegisterUnitTables(SnNamespace& root);

    //Stage-2 emit gate: rank of the expression's resolved array token,
    //0 when the expression is not array-typed. The temporary ND gates
    //(task 2) reject rank >= 2 until task 4 lands the ND emission.
    static uint8_t EmitGateRankOf(const SnExpression& expr);

    //Declared runtime kind for a type field — the one source local
    //descriptors, field kinds and signature leaves classify through.
    //v0.12.3: placeholder types classify as RTK_TypeParam (T slots;
    //see Types.cpp). Public for unit tests, same as EmitGateRankOf.
    static uint8_t RuntimeTypeKind(SnField* pType);

    //Own-unit test for the per-unit walks (Register.cpp).
    bool IsOwnUnit(const SnField& member) const;
    //Same-build cross-unit callee support: ShadowRegisterUnits runs
    //the per-unit registration sequence over EVERY translation unit
    //once (registration only, no bytecode, no image) and archives each
    //one's function-table map; ProducerIdxOfCrossUnitFunc then reads
    //the authoritative producer-side index for the import record
    //ordinal (nlink single-selects same-name same-arity overloads of
    //inlined library sources and sibling user units alike). 0xFFFF =
    //no archived record.
    void ShadowRegisterUnits(SnNamespace& root);
    uint16_t ProducerIdxOfCrossUnitFunc(const SnFunction& callee);
    //Same-build cross-unit static support, the static-slot mirror of
    //the function archive above: StaticSlotIndexOf replays the
    //producer's staticSlots index for a foreign-unit static field (an
    //inlined library source's class static or interface runtime
    //constant — nodes that carry no wire rider, unlike rebuilt .ncu
    //stubs). 0xFFFF = no archived slot.
    uint16_t ProducerIdxOfCrossUnitStatic(
        const SnClassField& field) const;
    //The one query every rider-gated site asks: the wire rider when the
    //node carries one, else the archive replay above (0xFFFF = neither,
    //the producer folded the constant or owns no slot row). Both call
    //sites route through this so they cannot drift apart again.
    uint16_t EffectiveProducerSlotIdx(const SnClassField& field) const;

    //Cross-unit reference slots (SymbolSlots.hpp): resolve a bound
    //declaration to its unit-local table slot — own declarations via the
    //existing lookups, cross-unit ones via placeholder records + import
    //entries.
    uint32_t FunctionSlotFor(SnFunction& callee);
    //FunctionSlotFor's own-unit arm: table lookup with the dictionary-
    //model clone-to-owner fallback (see Slots.cpp).
    uint32_t OwnFunctionSlotFor(SnFunction& callee);
    uint32_t ClassSlotFor(SnClassDecl& decl);
    //v0.12.6 Task 7 (D11): the interface-table index of an own-unit
    //interface (RegisterInterfaces fills the map) — kind-1 gate
    //operands address the interface owner through it.
    uint16_t InterfaceSlotFor(SnInterfaceDecl& decl);
    uint32_t StructSlotFor(SnStructDecl& decl);
    uint32_t EnumSlotFor(SnEnumDecl& decl);

    //Ctor slot for a class that already has its class slot. 0.9.11 T6
    //invariant: reached only when the resolver recorded no selected ctor
    //for the call — the builtin/generic intrinsic single-ctor shapes and
    //types without any ctor. Own/builtin records carry
    //constructorIndices (the builtin writers; [0] = the intrinsic ctor);
    //the cross-unit AST scan is a defensive dead branch for user types
    //since T6 (their calls always carry an AST selection slotted
    //through FunctionSlotFor). 0xFFFF = no ctor.
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

    //Inject the library declaration index (stdlib/*.n signatures) before
    //GenerateUnits. Codegen reads param/return types from it; the
    //pointer is borrowed, not owned.
    void SetLibraryIndex(const langservice::SymbolIndex* pIndex)
    {
        m_pLibraryIndex = pIndex;
    }

    //Phase 5: the package/qualified-name seam lives on the compile-time
    //registry. Codegen must build the SAME string the resolver shows the
    //user, so it borrows the registry (never owns it; it outlives codegen
    //inside one build). The env pointer is the diagnostic channel:
    //VmBackend has no env of its own, so the entry-point scan and the D5
    //native-package diagnostics need it injected.
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
    //v0.13.1 D4: typeName emission — folded spelling via the string
    //pool, bare type parameter via OP_TypeNameArg (the receiver's
    //descriptor), recipe instantiations via OP_TypeNameCompose.
    void Access(SnTypeNameExpr&);
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
    //GenerateFunction's native arm (split for the 50-line function
    //budget): signature fill via FillNativeFunctionRecord, then the
    //binding-table intrinsic stamp.
    void EmitNativeFunctionRecord(SnFunction& func,
        CompiledFunction& compiledFunc);
    //EmitNativeFunctionRecord's stamping tail (binding table, §4.5).
    void StampIntrinsicChannel(SnFunction& func,
        CompiledFunction& compiledFunc);    //StampIntrinsicChannel's owner resolution (context walk, with the
    //declared-generic instance-copy arm resolving through the template).
    uint32_t IntrinsicChannelOwnerOf(const SnFunction& func) const;

    //v0.12.6 lazy static initialization (spec §6.2): gate+call prefix
    //for a first-active-use site. Emits nothing when the class needs no
    //initializer. EmitStaticFieldGate is the field-bound convenience
    //the static access arms call (gate the DECLARING class, D4; an
    //interface parent routes to the interface gate, D11).
    void EmitClassInitGate(SnClassDecl& decl, BytecodeEmitter& emitter);
    void EmitInterfaceInitGate(SnInterfaceDecl& decl,
                               BytecodeEmitter& emitter);
    void EmitStaticFieldGate(SnField* field, BytecodeEmitter& emitter);
    //GenerateFunction's static-init arm plus its protocol body (super
    //gate inside the try, assignments, catch-all Fail+Rethrow, Done).
    //The interface flavor is the same protocol with kind=1 and no
    //super chain (Task 7, D11).
    void GenerateStaticInitFunction(SnFunction& func, size_t funcIdx);
    void EmitStaticInitBody(SnClassDecl& decl, SnFunction& func,
        BytecodeEmitter& emitter);
    void EmitInterfaceStaticInitBody(SnInterfaceDecl& decl, SnFunction& func,
        BytecodeEmitter& emitter);
    //Shared protocol core of the two bodies above (Fail/Done carry the
    //owner index and kind; the super gate is classes-only).
    void EmitStaticInitProtocolBody(uint16_t ownerIdx, uint16_t kind,
        SnFunction& func, SnClassDecl* super, BytecodeEmitter& emitter);

    //v0.12.6 Task 7 (spec §8): static locals. Registration runs at the
    //declaration statement (one module slot per declarator, ownerKind=2,
    //no frame slot); the decomposed init assign (NF_Generated) emits the
    //once-guard below; every later read/store is a plain static-slot
    //access (the declaration is lexically upstream of all uses).
    void RegisterStaticLocalDeclarators(SnLocalDeclStmt& decl,
        SnField* evalType);
    void EmitStaticLocalInitSequence(SnAssignStmt& assign, uint16_t slotIdx,
        BytecodeEmitter& emitter);

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
    //FieldOffsets.cpp), shared by the member-read, assign-store,
    //and new-expression emitters across emission TUs. Non-static: the
    //handle-class skip consults the owner via the registry, the same
    //predicate CollectOwnClassFields inserts the hidden __handle with.
    static int FindFieldOffset(SnStructDecl& structDecl,
                               const std::string& fieldName);
    int FindClassFieldOffset(SnClassDecl& classDecl,
                             const std::string& fieldName);
    //FindClassFieldOffset arm: one ancestor in the root→parent walk —
    //returns the field offset when the field lives there, else -1; `off`
    //always advances past that ancestor's data fields.
    int ClassAncestorFieldOffset(SnClassDecl& ancestor,
                                 const std::string& fieldName,
                                 uint16_t& off);
    //v0.12.6 D14: the declared-type companion of FindClassFieldOffset —
    //same root→parent→own search (statics skipped); returns the field's
    //declared type node or null. The store sites consult it for the
    //struct deep-copy decision (manual 4.7.4).
    SnField* FindClassFieldDecl(SnClassDecl& classDecl,
                                const std::string& fieldName);
    //0.9.11 D2: generalized to struct fields — returns the owning type
    //decl (SnClassDecl or SnStructDecl) for a member field of either,
    //null otherwise; the caller dispatches the offset helper by the
    //field's kind.
    static SnField* OwningTypeOfMemberField(SnField* pField);

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
    //Compile-time constant reads: enum member and (0.9.10 D3) interface
    //constant. Both fan out to an inline literal with no receiver.
    bool EmitMemberConstantRead(SnField* field, BytecodeEmitter& emitter,
                                uint16_t resultOffset);
    //0.9.10 D3: interface constant literal — the initializer is folded
    //at the use site into a Const* op writing pResult (the caller
    //assigns it to the result slot, like the enum-member arm).
    //v0.12.6: serves interface constants AND compile-time-folded class
    //static consts — the same inline-literal emission for both forms.
    void EmitFoldedConstantRead(SnClassField& field,
                                BytecodeEmitter& emitter);
    bool EmitMemberBuiltinDispatch(SnMemberExpr& member,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    //0.9.10: type-name-qualified static call (Util.twice(21), I.stat()).
    bool EmitMemberStaticCall(SnMemberExpr& member,
                              BytecodeEmitter& emitter,
                              uint16_t resultOffset);
    //0.12.6: type-name-qualified static field read (Counter.total, the
    //D4 inherited Derived.tag). Keyed on the bound field, never the
    //receiver — the receiver names a type and has no value to emit.
    bool EmitMemberStaticFieldRead(SnMemberExpr& member, SnField* field,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    //Static face on a builtin generic type name (Future.selectAny):
    //no receiver — args stage from claim slot 0 and dispatch straight
    //to OP_CallIntrinsic (the face table's row carries the id).
    bool EmitMemberStaticFaceCall(SnMemberExpr& member,
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
    //v0.12.4 D5: copy-on-read seal — after a field load, when the field
    //is declared by a const aggregate and typed as a List/Dict
    //instantiation, emit the copy intrinsic over the loaded handle so
    //the read yields a fresh container (method calls and subscript
    //stores through the read mutate the copy). No walker change: the
    //sequence claims no eval-area slots (the zero-arg receiver-intrinsic
    //shape, string getHashCode family). Definition in EmitExprMember.cpp.
    void EmitSealedFieldCopyIfNeeded(SnField* pField,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset);
    //Array length() builtin method.
    bool EmitMemberArrayRankMethod(SnMemberExpr& member,
        BytecodeEmitter& emitter, uint16_t resultOffset);
    bool EmitMemberArrayShapeMethod(SnMemberExpr& member,
        BytecodeEmitter& emitter, uint16_t resultOffset);
    bool EmitMemberArrayLengthMethod(SnMemberExpr& member,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset);
    //v0.12.7: Array protocol methods equals(Object)/getHashCode() —
    //identity semantics via the Object protocol intrinsics, staged in
    //the string protocol-call shape. Definition in
    //EmitExprMemberArray.cpp.
    bool EmitMemberArrayProtocolMethod(SnMemberExpr& member,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset);
    //v0.12.7 Task 3: boundary rank check — an annotated target (interned
    //token) receiving an unannotated array value (the Array<E> spelling)
    //verifies the record's rank at the assign/argument/return boundaries.
    //Emits OP_CheckArrayRank in place on the staged slot; no-op when the
    //target is not an annotated token or the value already carries the
    //annotation (same-rank annotated flow is compile-checked, mismatched
    //annotated flow is a compile error). Definition in EmitExprCast.cpp.
    void EmitArrayRankBoundary(const SnField* targetType,
                               const SnExpression* pSource,
                               uint16_t slot, BytecodeEmitter& emitter);
    //v0.12.7 Task 5: the Object→array downcast (`o as int[]`) — registers
    //the target's array type and emits OP_CheckArrayType in place on the
    //staged slot. False = the target is not an array token (the caller
    //falls through to the class OP_CheckCast arm). Definition in
    //EmitArrayBoundary.cpp.
    bool EmitAsArrayDowncastOp(const SnField* targetType, uint16_t slot,
                               BytecodeEmitter& emitter);
    //Bound method reference in value position (c.foo) — func handle.
    void EmitMemberFuncHandleRef(SnMemberExpr& member, SnField* field,
                                 BytecodeEmitter& emitter,
                                 uint16_t resultOffset);
    //Receiverless function reference in value position (lib.seven or a
    //static method) — the bare static func handle.
    void EmitReceiverlessFuncHandleRef(SnField& func, BytecodeEmitter& emitter,
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
    //Phase 8e-4 boxing plan for generic method calls, derived
    //from the declared template signatures (phase-2 D6).
    void PlanGenericMethodBoxing(SnClassDecl& classDecl,
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
    CallSlotStats ComputeCallSlotStats(SnFunction& sn);

    //GenerateFunction decomposition (2026-09-25): each phase stamps one
    //slice of the CompiledFunction record / frame layout, in call order.
    //FuncContext is defined below in the private section (forward-declared
    //here so these declarations can refer to it).
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
    //K2 slice 5: pack the per-slot band classes into the mask (see
    //CompiledFunction::evalAreaHandleMask).
    void BuildEvalAreaHandleMask(CompiledFunction& cf,
                                 const FuncContext& ctx);
    //K2 slice 5: root class for one RTK kind byte (the serialized form
    //the locals table and the return kind arrive in).
    static uint8_t EvalSlotClassOfKind(uint8_t rtk);

    //Recursive frame-depth walkers behind ComputeCallSlotStats.
    //visited guards recursion through callee default expressions.
    //isMethodContext=true when the InvokeExpr is the Inner() of a
    //MemberExpr (method-call shape reserving slot 0 for `this`).
    static uint16_t ExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited,
        bool isMethodContext = false);
    //Instance method: the NK_SuperCallStmt arm reads m_pCurrClass to
    //find the parent ctor's formal count (P3) — same shape as the
    //MaxArgs walker family.
    uint16_t StmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //ExprPeakDepth arms — one expression shape family each; the node-kind
    //dispatch stays in ExprPeakDepth.
    //NK_InvokeExpr: call claim + arg/default-expression depth.
    static uint16_t InvokeExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited, bool isMethodContext);
    //0.9.0: the ctor-invoke arm — NewExpr claim shape {this, formals...}
    //plus default-expression depth (walker symmetry with NewExprPeakDepth).
    static uint16_t CtorInvokePeakDepth(SnInvokeExpr& invoke,
        const std::unordered_set<SnFunction*>& visited);
    //NK_BinaryExpr (also unary: Right()==nullptr).
    static uint16_t BinaryExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SubscriptExpr: 2-slot claim over the deeper operand.
    static uint16_t SubscriptExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SliceItemExpr (v0.12.7 Task 11): no own claim — the deepest of
    //its present parts (they stage into the parent subscript's claim).
    static uint16_t SliceItemExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_MemberExpr: receiver + method-call inner (string-method trailing
    //arg reservation included).
    static uint16_t MemberExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_NewExpr: {this, formals...} claim + arg/default depth.
    static uint16_t NewExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //Shared tail of the three ctor-call depth arms (NewExpr /
    //ctor-invoke / super()): max(argDepth, parent-ctor default-
    //expression depth) under the visited-set recursion guard.
    static uint16_t CtorDefaultExprDepth(SnFunction* pCtorFn,
        const std::unordered_set<SnFunction*>& visited,
        uint16_t argDepth);
    //MaxArgs-side counterpart of CtorDefaultExprDepth: reserve the
    //nested-call demand inside a callee's default expressions (a nested
    //call bulk-copies its argument slots into the CALLER's callParamBase
    //while the default stages). Serves all four call-emitting arms —
    //function/method invokes plus the three ctor shapes. Not static —
    //the recursion guard is the member set below (the MaxArgs walker
    //has no visited parameter to thread).
    void CalleeDefaultMaxArgs(SnFunction* pCalleeFn, uint16_t& maxArgs);
    //NK_NewArrayExpr: 1-slot size-expression claim.
    static uint16_t NewArrayExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //NK_InitListExpr: per-entry claim (Dict 3 / List 2 / other 1).
    static uint16_t InitListExprPeakDepth(SnExpression& expr,
        const std::unordered_set<SnFunction*>& visited);
    //StmtPeakDepth arms — one statement shape each. All instance
    //methods (the super arm reads m_pCurrClass; recursive arms reach
    //back into the instance dispatcher).
    //NK_Paragraph: max over child statements.
    uint16_t ParagraphPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ReturnStmt: result-expression depth.
    uint16_t ReturnStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_AssignStmt: lvalue + RHS depth + member/identifier claim.
    uint16_t AssignStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_IfStmt: condition claim + then/else branches.
    uint16_t IfStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_WhileStmt: condition claim + body.
    uint16_t WhileStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_DoStmt: condition claim + body.
    uint16_t DoStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ForStmt: init/InitExtras/cond/fini/body.
    uint16_t ForStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SwitchStmt: condition + case labels + case/default bodies.
    uint16_t SwitchStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ForeachStmt: iterable + body.
    uint16_t ForeachStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_AssertStmt: condition claim.
    uint16_t AssertStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_TryStmt: try/catch/finally bodies.
    uint16_t TryStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SuperCallStmt: {this, formals...} claim + arg/default depth.
    //Instance method: the parent class comes from m_pCurrClass.
    uint16_t SuperCallStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_ThrowStmt: thrown-expression claim + depth.
    uint16_t ThrowStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_CompoundAssignStmt: uniform 3-slot claim + receiver/RHS depth.
    uint16_t CompoundAssignStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //NK_SubscriptAssignStmt: 3-slot claim + array/index/value depth.
    uint16_t SubscriptAssignStmtPeakDepth(SnStatement& stmt,
        const std::unordered_set<SnFunction*>& visited);
    //MaxArgs walker behind ComputeCallSlotStats (callParamBase sizing):
    //statement/expression recursion collecting the largest call claim
    //into maxArgs; isMethodContext as in ExprPeakDepth. Instance
    //methods (the super arm reads m_pCurrClass — same as the
    //StmtPeakDepth family).
    void MaxArgsWalkStmt(SnStatement& stmt, uint16_t& maxArgs);
    void MaxArgsWalkExpr(SnExpression& expr, uint16_t& maxArgs,
        bool isMethodContext = false);
    //MaxArgsWalkStmt arms.
    //NK_AssignStmt: Left() lvalue + RHS (subscript receivers).
    void MaxArgsAssignStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_ForStmt: init/InitExtras/cond/fini/body.
    void MaxArgsForStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_SwitchStmt: condition + labels + case/default bodies.
    void MaxArgsSwitchStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_ForeachStmt: expansion prelude reserves 2 slots + iterable/body.
    void MaxArgsForeachStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_TryStmt: try/catch/finally bodies.
    void MaxArgsTryStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_SuperCallStmt: 1 + totalFormals claim + args (parent ctor from
    //m_pCurrClass).
    void MaxArgsSuperCallStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_CompoundAssignStmt: Left() receiver + RHS.
    void MaxArgsCompoundAssignStmt(SnStatement& stmt, uint16_t& maxArgs);
    //NK_SubscriptAssignStmt: container set() reserves 3 slots + children.
    void MaxArgsSubscriptAssignStmt(SnStatement& stmt, uint16_t& maxArgs);
    //MaxArgsWalkExpr arms.
    //NK_InvokeExpr: the call's own claim, then nested calls in its args.
    void MaxArgsInvokeExpr(SnExpression& expr, uint16_t& maxArgs,
        bool isMethodContext);
    //0.9.0: the ctor-invoke arm — bulk-copies {this, formals...} into
    //callParamBase like MaxArgsNewExpr (walker symmetry).
    void MaxArgsCtorInvoke(SnInvokeExpr& invoke, uint16_t& maxArgs);
    //NK_SubscriptExpr: container get() reserves 2 slots.
    void MaxArgsSubscriptExpr(SnExpression& expr, uint16_t& maxArgs);
    //NK_MemberExpr: method-context flag for a method-call inner.
    void MaxArgsMemberExpr(SnExpression& expr, uint16_t& maxArgs);
    //NK_NewExpr: ctor claim = 1 (this) + totalFormals.
    void MaxArgsNewExpr(SnExpression& expr, uint16_t& maxArgs);
    //NK_InitListExpr: implicit Dict set(3) / List add(2) claims.
    void MaxArgsInitListExpr(SnExpression& expr, uint16_t& maxArgs);
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
    //v0.12.6 D14: the declared-type twin of ResolveMemberFieldOffset —
    //the field's type node for a class (chain walk) or struct (flat
    //scan) element/member target, null on a miss.
    SnField* DeclaredMemberFieldType(SyntaxNode* elemType,
                                     const std::string& fieldName);
    void EmitAssignBareIdentifier(SnAssignStmt& assign, SnField* field,
                                  BytecodeEmitter& emitter);
    void EmitAssignToLocal(SnAssignStmt& assign, uint16_t offset,
                           SnField* varType, BytecodeEmitter& emitter);
    void EmitAssignToThisField(SnAssignStmt& assign, int fieldOff,
                               SnField* fieldType,
                               BytecodeEmitter& emitter);
    void EmitAssignMemberField(SnAssignStmt& assign,
                               SnMemberExpr& memberExpr,
                               BytecodeEmitter& emitter);
    bool EmitAssignSubscriptElementStore(SnAssignStmt& assign,
                                         SnMemberExpr& memberExpr,
                                         BytecodeEmitter& emitter);
    void EmitAssignContainerElementFieldStore(SnAssignStmt& assign,
        SnSubscriptExpr& sub, SyntaxNode* elemType, uint16_t fieldOff,
        SnField* fieldType, BytecodeEmitter& emitter);
    void EmitAssignArrayElementFieldStore(SnAssignStmt& assign,
        SnSubscriptExpr& sub, SyntaxNode* elemType, uint16_t fieldOff,
        SnField* fieldType, BytecodeEmitter& emitter);
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

    //Phase 8e-4: RTK_* tag for boxing a primitive-T argument, plus an
    //isPrimitive flag (needed because RTK_Int32 == 0 — same collision as
    //the Phase 8e-3 C1 fix). Shared by List<T>/Dict<K,V> codegen and the
    //0.8.4 compound-subscript path. Defined here (before the compound
    //declarations that name it) rather than at its former later spot.
    struct BoxingTagResult { uint8_t tag; bool isPrimitive; };

    //Compound assign (EmitStmtCompound.cpp): read-modify-write on the
    //bare local / implicit this-field / member field targets. op is the
    //underlying binary operator; int avoids requiring SnBinaryExpr's
    //full definition here (same as EmitCompoundOp).
    void EmitCompoundAssignBareIdentifier(SnCompoundAssignStmt& ca,
        SnField* field, int op, BytecodeEmitter& emitter);
    void EmitCompoundAssignThisField(SnCompoundAssignStmt& ca,
        SnField* field, int fieldOff, int op, BytecodeEmitter& emitter);
    void EmitCompoundAssignMemberField(SnCompoundAssignStmt& ca,
        SnMemberExpr& memberExpr, int op, BytecodeEmitter& emitter);
    //v0.12.6 D3: static read-modify-write (bare or class-qualified) —
    //slot load, RHS, compound op, slot store; old value and RHS park in
    //an exclusive claim across the RHS emission.
    void EmitCompoundAssignStaticField(SnCompoundAssignStmt& ca,
        uint16_t slotIdx, SnField* lhsType, int op,
        BytecodeEmitter& emitter);
    //0.8.4: bare-subscript LHS (arr[i] += v, li[i] -= v, d[k] *= v) —
    //read-modify-write with base and index each evaluated exactly once
    //(JLS 15.26.1), parked in an EvalAreaClaim(4) [base, index, old, rhs].
    void EmitCompoundAssignSubscript(SnCompoundAssignStmt& ca,
        SnSubscriptExpr& sub, int op, BytecodeEmitter& emitter);
    //0.8.4: container-subscript compound assign (li[i] op= v, d[k] op= v) —
    //sugar over get()/set() on the already-parked [base, index, result]
    //claim slots, so base and index are not re-emitted.
    void EmitCompoundSubscriptContainer(SnCompoundAssignStmt& ca,
        SnSubscriptExpr& sub, SnField* elemType, int op,
        uint16_t claimBase, uint16_t indexSlot, uint16_t oldValSlot,
        uint16_t rhsSlot, BytecodeEmitter& emitter);
    //0.8.4: array-subscript compound assign (arr[i] op= v) — direct
    //element ops over the parked claim slots, no call frames.
    void EmitCompoundSubscriptArray(SnCompoundAssignStmt& ca,
        SnSubscriptExpr& sub, SnField* elemType, int op,
        uint16_t claimBase, uint16_t indexSlot, uint16_t oldValSlot,
        uint16_t rhsSlot, BytecodeEmitter& emitter);
    //0.8.4: get() call over the parked [base, index] claim slots
    //(mirrors EmitContainerGetCall without re-emitting base/index).
    void EmitSubscriptGetFromClaim(uint16_t claimBase,
        uint16_t indexSlot, uint16_t oldValSlot, BoxingTagResult valBox,
        BytecodeEmitter& emitter);
    //0.8.4: set() call over the parked [base, index, result] claim slots.
    void EmitSubscriptSetFromClaim(uint16_t claimBase,
        uint16_t indexSlot, uint16_t oldValSlot, BoxingTagResult valBox,
        BytecodeEmitter& emitter);

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
    //0.7.5 string arm: code-point iteration — OP_StrForeachStep at the
    //loop head decodes+advances; continue targets the step (the head),
    //break the loop end. Hidden locals mirror the index-based expansion
    //(iter handle / byte offset / continue flag).
    void EmitStringForeach(SnForeachStmt& fe, BytecodeEmitter& emitter);

    //Switch (EmitStmtSwitch.cpp): per-clause emission and the
    //clause-exit fixup. EmitSwitchCaseClause appends to clauseExits the
    //jumps the fixup must patch (OP_Case placeholder + last label's
    //miss) and reports the implicit no-fallthrough exit via
    //bodyExitJump. EmitSwitchOneLabelCompare returns the label's
    //miss-jump offset (caller picks next-label vs clause-exit target);
    //EmitSwitchLabelNormalize applies its 0.7.5 cross-width PrimCast
    //wrap. 0.7.5: the per-switch compare descriptor — string
    //discriminants keep the dedicated OP_Eq_str; scalar discriminants
    //ride the kind-immediate OP_Cmp Equal (enum ≡ int32, never the raw
    //NK_EnumDecl kind — RtkOfKind would reject it).
    struct SwitchCompare {
        NodeKind kind = NK_Int32;
        bool isString = false;
    };
    static SwitchCompare SwitchCompareOf(SnSwitchStmt& switchStmt);
    void EmitSwitchCaseClause(SnCaseClause& clause, uint16_t switchSlot,
                              const SwitchCompare& compare,
                              std::vector<size_t>& clauseExits,
                              size_t& bodyExitJump,
                              BytecodeEmitter& emitter);
    void EmitSwitchLabelCompares(SnCaseClause& clause,
                                 uint16_t switchSlot,
                                 const SwitchCompare& compare,
                                 std::vector<size_t>& clauseExits,
                                 BytecodeEmitter& emitter);
    size_t EmitSwitchOneLabelCompare(SnExpression& label,
                                     uint16_t switchSlot,
                                     const SwitchCompare& compare,
                                     BytecodeEmitter& emitter);
    void EmitSwitchLabelNormalize(SnExpression& label,
                                  const SwitchCompare& compare,
                                  uint16_t condSlot,
                                  BytecodeEmitter& emitter);
    //0.7.5: in-place PrimCast of a staged scalar slot — shared by the
    //switch-label normalize and the default-argument fill (EmitBinding).
    void EmitScalarSlotCast(NodeKind from, NodeKind to, uint16_t slot,
                            BytecodeEmitter& emitter);
    void EmitSwitchClauseExits(
        const std::vector<size_t>& caseStartOffsets,
        const std::vector<std::vector<size_t>>& exitJumps,
        bool hasDefault, size_t locCaseEnd, size_t locEnd,
        BytecodeEmitter& emitter);

    //Try/catch/finally (EmitStmtTry.cpp). EmitTryCatchClauses
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
    //per spill, `OP_VarLocal callParamBase+slotIdx*kFrameSlotBytes;
    //OP_Assign localOffset`.
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
    //B_Default arm of EmitBinding: the override scope, the default
    //expression, and the 0.7.5 in-place kind normalize to the formal.
    void EmitDefaultBinding(const FormalBinding* pBindings,
                            size_t bindingIdx, uint16_t slotIdx,
                            size_t slotBase, uint16_t base,
                            uint16_t paramOffset,
                            BytecodeEmitter& emitter, uint16_t thisSlot);

    //Struct value-semantics tail shared by every deep-copy site: copy
    //the heap subtree src → dst via OP_CopyStruct. Per-unit: a
    //cross-unit struct slots as an import placeholder (StructSlotFor).
    void EmitStructDeepCopy(uint16_t dst, uint16_t src,
                            SnStructDecl& structDecl,
                            BytecodeEmitter& emitter);
    //v0.12.6 D14: struct-aware field store — when the field's declared
    //type is a struct, deep-copy the staged value in place before the
    //store (a struct moving into a field is copied, manual 4.7.4);
    //every other kind stores the cell as-is. valueSlot already holds
    //the right side's value (OP_CopyStruct reads the source cell before
    //writing the destination, so dst == src is safe).
    void EmitStructAwareStoreField(SnField* fieldType,
                                   uint16_t objSlot, uint16_t fieldOff,
                                   uint16_t valueSlot,
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

    //RAII registration of "pFn's defaults are mid-emission", shared by the
    //three default-emission sites (function/method bindings, ctor
    //invocation, super forwarding). Re-entry of the same function means a
    //recursive default — a default expression requiring the same defaults
    //again — which would overflow the compile stack; the guard turns it
    //into a compile-time error. armed=false registers nothing (and erases
    //nothing: an unarmed entry must never drop an enclosing frame's
    //in-progress registration for the same function).
    struct DefaultEmitGuard {
        VmBackend* pBackend; SnFunction* pFn; bool armed;
        DefaultEmitGuard(VmBackend& backend, SnFunction* pFn, bool armed);
        ~DefaultEmitGuard();
    };

    //The registration predicate every ctor scan shares (0.9.11 v2.6):
    //a ctor participates in the unit table when it has a body, carries
    //the native flag, or is an imported stub — stub members are
    //registered by construction (the import record IS the registration;
    //no bytecode is emitted for them). The predicate's single definition
    //is the AST-level SnFunction::HasImplementation; this delegates to it
    //so the resolver's ctor-candidate filter (ExprResolverNew.cpp) and the
    //backend share one source. Every consumer (CtorSlotFor's placeholder
    //scan, FunctionSlotFor's ordinal scan, RegisteredCtor, and the frame
    //walkers through them) must use this one predicate: divergent filters
    //split the walker claim (1) from the codegen claim (1+formals) and
    //trip the observedPeakCursor tripwire far from the cause.
    static bool IsRegisteredCtor(const SnFunction& ctor);

    //v2.6 import-record owner metadata for a cross-unit callee
    //(Slots.cpp): the owning class/struct's qualified key + table kind,
    //and the callee's ordinal in the owner's member table — the
    //declaration-order registered-ctor list for a constructor, the
    //method list (ctor entries skipped) for methods and statics
    //(0.9.12 D-new-8). The defaults are the namespace-level shape
    //(kind 0, ordinal 0xFFFF = no ordinal).
    struct OwnerMemberMeta {
        std::string ownerClassKey;
        uint8_t ownerKind = 0;
        uint16_t ownerOrdinal = 0xFFFF;
    };
    OwnerMemberMeta CrossUnitOwnerMeta(const SnFunction& callee) const;
    //FunctionSlotFor's namespace-branch tail: replays the record's
    //ordinal from the stub's producer index (see Slots.cpp).
    void ReplayNamespaceBranchOrdinal(OwnerMemberMeta &meta,
        SnFunction &callee);

    //0.9.11 D2/T6: the AST-selected constructor (SnNewExpr/SnInvokeExpr/
    //SnSuperCallStmt CtorFunction()) when it is registered
    //(IsRegisteredCtor) — else null. The resolver's overload selection
    //replaced every first-scan path: a body-less non-native ctor
    //allocates without running (the class path's CtorSlotFor 0xFFFF
    //shape). Static: the frame-size walkers consume it too.
    static SnFunction* RegisteredCtor(SnFunction* pCtorFn);

    //0.9.11 T6 (D10): the super-call tail of Access(SnSuperCallStmt&) —
    //the resolver-selected parent ctor drives the invocation
    //(EmitStmtTry.cpp).
    void EmitSuperCtorCall(SnSuperCallStmt& sc, SnClassDecl* pParent);

    //Build the defaults-aware FormalBinding array shared by the ctor
    //invocation and super() forwarding: the caller arguments bind the
    //leading formals positionally, each omitted trailing formal stages
    //its default expression. Returns the ctor's total formal count (the
    //caller-arg count when pCtorFn is null — nothing to default there).
    static size_t BuildCtorBindings(SnFunction* pCtorFn,
        const std::vector<SnExpression*>& callerArgs,
        std::vector<FormalBinding>& bindings);

    //True when any binding stages a default expression — the shared
    //DefaultEmitGuard arms only then (an all-supplied call must not
    //claim the guard and reject a sibling default expansion).
    static bool BindingsEmitDefaults(
        const std::vector<FormalBinding>& bindings);

    //Emit one ctor-call binding set into claim[1..N]: defaults-aware via
    //EmitBinding when the AST ctor is known, legacy positional emission
    //when bindings is empty (builtin/generic/imported target — their
    //signatures never take user structs, so no deep-copy tail applies).
    //Shared by the NewExpr/invoke lowering and super() (thisSlot
    //UINT16_MAX vs local 0).
    void EmitCtorCallBindings(const std::vector<FormalBinding>& bindings,
        const std::vector<SnExpression*>& callerArgs, uint16_t claimBase,
        BytecodeEmitter& emitter, uint16_t thisSlot);

    //0.9.11 D2: which allocator the ctor-call tail emits. A class
    //allocates through OP_New(typeIdx, descIdx); a struct through
    //OP_AllocStruct(typeIdx, fieldCount). Everything else in the ctor
    //lowering (claim layout {this, formals...}, this staging,
    //callParamBase copy, OP_CallMethodDirect) is shared.
    struct CtorAlloc {
        bool isStruct = false;
        uint16_t typeIdx = 0;     //class slot or struct slot
        uint16_t fieldCount = 0;  //struct only (OP_AllocStruct operand)
        //v0.13.1 D1: the instantiation descriptor row for OP_New's third
        //operand — 0xFFFF (kNoInstDescriptor) when the construction
        //carries no descriptor (non-instantiation types; runtime-recipe
        //constructions until that path lands).
        uint16_t descIdx = 0xFFFF;
    };
    static constexpr uint16_t kNoInstDescriptor = 0xFFFF;

    //v0.13.1: the descriptor-table row for a concrete instantiation's
    //construction — registers (and dedups by spelling) a static row and
    //returns its index; kNoInstDescriptor for non-instantiations and
    //for arguments containing type parameters (those compose at run
    //time through the recipe path). Interns the descriptor so the
    //compile-side and load-side pools agree.
    uint16_t DescriptorIndexFor(SnClassDecl& inst);
    //v0.13.1: recipe row for a typeName operand containing a type
    //parameter (see DescriptorSpelling.cpp); the row composes at run
    //time through OP_TypeNameCompose.
    uint16_t RecipeIndexFor(const SnField& operand);
    //v0.13.1: an array allocation's element-instantiation row — 0xFFFF
    //unless the element type is a generic instantiation.
    uint16_t ElemDescriptorIndexFor(SnField* pElemType);

    //Ctor-call tail after the bindings: allocate the object into
    //allocSlot (OP_New / OP_AllocStruct per alloc), stage it as claim[0]
    //(= this), bulk-copy the claim to callParamBase, and invoke the ctor.
    void EmitCtorCallTail(uint16_t allocSlot, uint16_t claimBase,
        uint16_t slotCount, const CtorAlloc& alloc, uint16_t ctorIdx,
        BytecodeEmitter& emitter);

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

    //Registration phases (run in order per unit inside GenerateUnits).
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
    void CollectOwnClassFields(SnClassDecl& sn, CompiledClass& cc);
    void ResolveClassMetadata(std::unordered_map<std::string, SnClassDecl*>& declMap);
    //classIdx (not a CompiledClass&) because slot resolution can append
    //import placeholders to m_compiledModule.classes mid-call — a held
    //element reference would dangle across the reallocation, so the
    //helpers re-index per statement.
    void ResolveClassFieldRefs(SnClassDecl* pDecl, size_t classIdx);
    void BuildClassFieldTypeDescs(SnClassDecl* pDecl, size_t classIdx);
    //0.12.6 D1: static-field slot allocation (module declaration order)
    //and the deferred type-descriptor fill for those slots — descriptors
    //wait for the all-classes-registered phase, like the sibling above.
    void AllocateStaticFieldSlot(SnClassField& cf, CompiledClass& cc);
    void BuildStaticFieldTypeDescs(SnClassDecl* pDecl, size_t classIdx);
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
    //PopulateClassMethods worker: one class's method/ctor tables from its
    //AST members in declaration order (keys on m_funcIndexMap).
    void AppendClassMemberMethods(SnClassDecl& sn, CompiledClass& cc);
    //0.9.11 D2: struct counterpart — fills CompiledStruct.constructorIndices
    //in declaration order (structs declare constructors only).
    void PopulateStructMethods(SnNamespace& root);
    //0.10.14 (v2.9): per-enum member-method index/access tables
    //(parallel to enumNames/enumKeys; keyed on m_funcIndexMap). Defined
    //in backend/RegisterEnums.cpp beside RegisterEnums.
    void RegisterEnumMethods(SnNamespace& root);
    //0.9.10 D5: per-unit interface statics table (name = qualified key,
    //static method indices into the function table). Defined in
    //backend/RegisterInterfaces.cpp.
    void RegisterInterfaces(SnNamespace& root);
    //RegisterInterfaces worker: contentless placeholder rows for foreign
    //interfaces (v0.12.6 Task 8 — unit-local indices for kind-1 gates
    //against imported interfaces; nlink's merge prefers the provider's
    //rich row at the same merged index).
    void RegisterForeignInterfacePlaceholders(SnNamespace& root);
    //RegisterInterfaces worker: the constants loop (fold split rides
    //StaticConstFoldsAtCompileTime; runtime constants take ownerKind=1
    //module slots).
    void RegisterInterfaceConstants(SnInterfaceDecl& sn,
                                    CompiledInterface& ci);
    //RegisterInterfaces worker: one interface's instance-method signature
    //records from its AST members (uses the leaf-slot resolvers and the
    //default-value encoders, hence a member).
    void AppendInterfaceSignatures(SnInterfaceDecl& sn, CompiledInterface& ci);
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
    //Every entry candidate in the tree (no owner filter) — the shared
    //collection behind FindEntryCandidate and the pre-check below.
    std::vector<SnFunction*> CollectEntryCandidates(SnNamespace& root);
    //Reporter shared by the two ambiguity gates: keys + source files.
    void LogAmbiguousEntries(const std::vector<SnFunction*>& candidates);
    //Phase 6 per-unit driver pre-check: two or more project units each
    //owning an executable main is ambiguous — the per-unit owner gate in
    //FindEntryCandidate hides each sibling unit's candidate, so the unit
    //set is judged as a whole here. Returns true (after logging) when the
    //build must fail.
    bool RejectAmbiguousUnitEntries(SnNamespace& root);
    //Phase 5 D5, per declaration: returns true (after logging) when a
    //native's package is multi-segment — the host DLL is chosen by the
    //first dot segment (phase 6 lifts this). The per-unit driver calls it
    //through the whole-tree walk below.
    bool RejectMultiSegmentNativePackage(SnFunction& func);
    //Phase 6 per-unit driver pre-check: RejectMultiSegmentNativePackage
    //for every function in the tree. The walk (not per-function codegen)
    //is the call site because library TUs never reach codegen — an
    //inlined library declaring a dotted native must still be refused at
    //compile time, not surface as a load-time failure.
    bool RejectMultiSegmentNatives(SnNamespace& root);

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
    //v0.12.6: shared generation dispatch — the synthesized "$staticInit"
    //member routes to its protocol emitter, everything else to the
    //ordinary body emitter.
    void DispatchFunctionGeneration(SnFunction& func, size_t funcIdx);

    //Register an array type from its element type field.
    //Returns the arrayTypeIdx in m_compiledModule.arrayTypes.
    uint16_t RegisterArrayType(SnField* pElemType, uint8_t rank = 1);

    //Return-type kind for .ncu serialization; array-ness is read from the
    //return TYPE EXPRESSION, not from Field() (which resolves to the
    //element field and would degrade `int[]` to RTK_Int32).
    static uint16_t SerializedReturnKind(SnFunction& func);
    //Option B Step 3: extract a constant-foldable default expression into
    //a DefaultValueDesc for serialization. Returns tag=RTK_Void when the
    //expression isn't foldable (caller-side check rejects for IsImported).
    //0.9.11 T4: declaredKind (the formal's or field's RuntimeTypeKind)
    //re-stamps the folded literal into the declared slot's carrier — the
    //raw-slot consumers (default-arg fill, struct field materialization)
    //write the cell once and the slot is read back by its declared kind.
    DefaultValueDesc ExtractDefaultValue(SnExpression* pExpr,
                                         uint8_t declaredKind);
    //ExtractDefaultValue arms: direct literal, and OP_Neg over a literal.
    void ExtractLiteralDefault(SnLiteralExpr* lit, DefaultValueDesc& dv);
    void ExtractNegatedLiteralDefault(SnBinaryExpr* bin, DefaultValueDesc& dv);
    //v2.3: encode a formal's default expression into the ExprDesc wire
    //grammar (CompiledModule.h). True = dv carries RTK_Expr + exprBytes;
    //false = outside the subset, caller keeps the RTK_Unfoldable stamp.
    bool EncodeDefaultExpr(SnFunction& func, SnExpression* pExpr,
                           DefaultValueDesc& dv);
    //Recursive ExprDesc encoder. allowCall gates the call arm — nested
    //calls in call arguments are out of the subset (spec decision D3).
    bool EncodeExprDesc(SnExpression& expr,
                        const std::vector<SnFormalParam*>& formals,
                        std::vector<uint8_t>& out, bool allowCall, int depth);
    //EncodeExprDesc arms: arithmetic (negation + five binaries) and the
    //producing-module call.
    bool EncodeBinaryDesc(SnBinaryExpr& bin,
                          const std::vector<SnFormalParam*>& formals,
                          std::vector<uint8_t>& out, bool allowCall,
                          int depth);
    bool EncodeCallDesc(SnInvokeExpr& invoke,
                        const std::vector<SnFormalParam*>& formals,
                        std::vector<uint8_t>& out, int depth);
    //Literal arm: the same kind set ExtractLiteralDefault folds.
    bool EncodeLiteralDesc(SnLiteralExpr& lit, std::vector<uint8_t>& out);
    static BoxingTagResult BoxingTagFor(SnField* pT);
    //The boxing tag for one placeholder-typed signature slot (formal or
    //return); the type argument the placeholder binds to decides it.
    static BoxingTagResult PlaceholderBoxingTag(
        const GenericInstanceRegistry& declaredGenerics,
        SnField* pPlaceholder, const std::vector<SnField*>& typeArgs);
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
    //Definitions in backend/EmitExprIdent.cpp (2026-11-02 size-guard
    //split out of EmitExprCore.cpp).
    void EmitIdentifierOverrideRead(BytecodeEmitter& emitter,
                                    uint16_t resultOffset,
                                    uint16_t overrideSlot);
    void EmitIdentifierEnumMemberRead(BytecodeEmitter& emitter,
                                      uint16_t resultOffset, SnField* field);
    void EmitIdentifierFuncHandleRead(BytecodeEmitter& emitter,
                                      uint16_t resultOffset, SnField* field);
    void EmitIdentifierFieldRead(BytecodeEmitter& emitter,
                                 uint16_t resultOffset, SnField* field);
    //v0.12.6 D3: inline-literal read of a compile-time-folded static
    //const; false = not a folded const (caller falls through).
    bool TryEmitFoldedStaticConstRead(SnField* field,
                                      BytecodeEmitter& emitter,
                                      uint16_t resultOffset);

    //SnLiteralExpr arm: registry-driven scalar const emission (the
    //caller has already verified typeKind is a registry row).
    void EmitScalarLiteral(SnLiteralExpr& lit, NodeKind typeKind,
                           BytecodeEmitter& emitter, uint16_t resultOffset);

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
    //The staged-invoke dispatch tail: delegate handle or the
    //Round-11-guarded free-function call.
    void EmitInvokeDispatch(SnInvokeExpr& invoke, SnFunction* callee,
                            BytecodeEmitter& emitter, uint16_t resultOffset,
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

    //SnAsExpr arms: box / unbox / downcast / explicit narrowing, and
    //the `f as string` render.
    void EmitAsBoxOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                     uint16_t resultOffset);
    void EmitAsUnboxOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                       uint16_t resultOffset);
    void EmitAsDowncastOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                          uint16_t resultOffset);
    bool EmitAsExplicitNarrowOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                                uint16_t resultOffset);
    bool EmitAsFuncToString(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                            uint16_t resultOffset);

    //SnSubscriptExpr arms: List/Dict get() sugar — classification, then
    //the claimed receive/index/call emission. EmitStringByteAt is the
    //0.7.5 string s[i] byte-read arm (OP_StrByteAt on a 2-slot claim).
    void EmitContainerSubscriptGet(SnSubscriptExpr& sub,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset);
    void EmitContainerGetCall(SnSubscriptExpr& sub, BoxingTagResult keyBox,
                              BoxingTagResult valBox,
                              BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitStringByteAt(SnSubscriptExpr& sub, BytecodeEmitter& emitter,
                          uint16_t resultOffset);

    //Plain array element read (OP_LoadElement on a 2-slot claim) — the
    //fall-through arm of Access(SnSubscriptExpr&) after the creation,
    //string, and container shapes dispatch away.
    void EmitArraySubscriptRead(SnSubscriptExpr& sub,
                                BytecodeEmitter& emitter,
                                uint16_t resultOffset);
    //v0.12.7 Task 11: true when any subscript index is a slice item —
    //the read re-routes from the element load to the view mint.
    static bool HasSliceAxis(const SnSubscriptExpr& sub);
    //v0.12.7 Task 11: the slice-subscript view mint — receiver, one
    //start/stop/step slot per axis and the presence-mask words stage in
    //one exclusive claim; OP_MakeArrayView normalizes and allocates.
    void EmitArrayViewRead(SnSubscriptExpr& sub,
                           BytecodeEmitter& emitter,
                           uint16_t resultOffset);
    //Staging half of EmitArrayViewRead (split for the function-size
    //guard): one axis's parameter slots + its presence bits into the
    //mask word array (layout at the implementation).
    void EmitViewAxisParams(SnSubscriptExpr& sub, size_t d,
                            uint16_t paramsBase, BytecodeEmitter& emitter,
                            uint32_t* mask);
    //v0.12.7: shared static/dyn element-op choice — true when the base's
    //static type carries no rank annotation (the Array<E> spelling), so
    //the record rank verifies at run time via the Dyn pair. Shared by
    //the read, store, compound, and field-store element emitters.
    static bool SubscriptNeedsDynOps(const SnExpression& arrExpr);
    //v0.12.7: `Array(dims)` dynamic-rank construction — the elem type
    //comes from the declared instantiation's type argument.
    void EmitArrayDynCtor(SnClassDecl& instance,
                          SnExpressionList& params,
                          uint16_t resultOffset);

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
    void EmitBinaryOpCode(SnBinaryExpr& bin, NodeKind numKind, bool isString,
                          bool isFunc, bool isStruct, BytecodeEmitter& emitter,
                          uint16_t resultOffset, uint16_t rightSlot);
    void EmitBinaryArithmeticOp(SnBinaryExpr& bin, NodeKind numKind,
                                bool isString,
                                BytecodeEmitter& emitter, uint16_t resultOffset,
                                uint16_t rightSlot);
    void EmitBinaryRelationalOp(SnBinaryExpr& bin, NodeKind numKind,
                                bool isString,
                                BytecodeEmitter& emitter, uint16_t resultOffset,
                                uint16_t rightSlot);
    void EmitBinaryEqualityOp(SnBinaryExpr& bin, NodeKind numKind,
                              bool isString, bool isFunc, bool isStruct,
                              BytecodeEmitter& emitter,
                              uint16_t resultOffset, uint16_t rightSlot);

    //New-object emission, shared by the SnNewExpr arm and the 0.9.0
    //ctor-invoke twin (`Foo(args)` without new, invoke.CtorClass()):
    //Round-12 class-resolution guard, class-slot resolution, and the
    //ctor invocation ({this, args} claim → OP_New → OP_CallMethodDirect)
    //or the plain alloc when the class has no ctor. pSkipArg drops the
    //class-name child the NewExpr arg view carries (null for invokes —
    //Params() has no such child). pCtorFn is the AST-selected ctor
    //(0.9.11 T6): registered → its function slot; null (builtin/generic,
    //no AST ctor) → the CtorSlotFor table path; selected but unregistered
    //→ plain alloc (the mirror of EmitStructCtor's third branch).
    void EmitNewObject(SnClassDecl* pClassDecl, SnFunction* pCtorFn,
                       SnExpressionList& ctorArgs, SnExpression* pSkipArg,
                       uint16_t resultOffset);
    //Ctor-selection tail of EmitNewObject: registered selection → its
    //function slot, table fallback, else plain OP_New alloc.
    void EmitNewCtorDispatch(SnClassDecl* pClassDecl, SnFunction* pCtorFn,
                             SnExpressionList& ctorArgs,
                             SnExpression* pSkipArg, uint16_t classIdx,
                             uint16_t resultOffset);
    //0.9.11 D2/D3: struct construction lowering for the explicit call
    //forms (the direct-init `S name(args);` spelling decomposes into a
    //NewExpr, bare/qualified `S(args)` into the ctor-invoke twin).
    //pCtorFn is the AST-selected constructor; null or unregistered
    //(body-less, non-native) mirrors the class no-ctor path: plain
    //OP_AllocStruct, arguments unevaluated, no call (D3 keeps bare
    //declarations, init lists, and copies constructor-free — only these
    //call forms reach here).
    void EmitStructCtor(SnStructDecl& structDecl, SnFunction* pCtorFn,
                        SnExpressionList& ctorArgs, SnExpression* pSkipArg,
                        uint16_t resultOffset);
    //Ctor call lowering: one FormalBinding per formal (caller args first,
    //then the defaults of the omitted trailing formals) emitted through
    //the shared EmitBinding — the same mixed-form loop as EmitCallArgs.
    //pCtorFn is the registered AST ctor (null for the builtin/generic
    //table path — the legacy positional shape, nothing to default);
    //alloc selects the allocator opcode; thisSlot is UINT16_MAX (the
    //object under construction does not exist yet, so a ctor default must
    //not reference `this`).
    void EmitCtorInvocation(SnFunction* pCtorFn,
                            SnExpressionList& ctorArgs, SnExpression* pSkipArg,
                            BytecodeEmitter& emitter, uint16_t ctorIdx,
                            const CtorAlloc& alloc, uint16_t resultOffset);

    //New-array emission, shared by the SnNewArrayExpr arm and the 0.9.0
    //type-subscript twin (`int[n]` / `Foo[n]`, subscript.NewArrayElem()):
    //registers the element type (idempotent) and stages the size in an
    //evalArea claim before OP_AllocArray.
    void EmitNewArray(SnField* pElemType, SnExpression& sizeExpr,
                      uint16_t resultOffset);
    //Stage-2: ND creation — dims evaluated once each, left to right,
    //staged into consecutive eval-area slots for OP_AllocArrayN.
    void EmitNewArrayND(SnField* pElemType,
        const std::vector<SnExpression*>& dims, uint16_t resultOffset,
        BytecodeEmitter& emitter);

    //SnInitListExpr arms: one per target shape (array / List<T> /
    //Dict<K,V> / user class / struct). EmitNewObjectAndNoArgCtor is the
    //OP_New + optional no-arg-ctor sequence shared by the class-shaped
    //arms; pCtorFn is the zero-formal ctor the resolver selected for the
    //init list (0.9.11 T6; null on the List/Dict arms and on the table
    //path, where classDecl feeds CtorSlotFor — placeholders have no
    //table ctor). The per-entry helpers stage values in evalArea claims.
    void EmitInitListArray(SnInitListExpr& initList, SnField* pElemField,
                           BytecodeEmitter& emitter, uint16_t resultOffset);
    void EmitInitListArrayEntry(const InitEntry& entry, int32_t entryIndex,
                                SnField* pElemField, BytecodeEmitter& emitter,
                                uint16_t resultOffset, uint16_t valueSlot);
    //Stage-2 ND: the rectangular-literal lowering (rank>=2 targets).
    void EmitInitListArrayND(SnInitListExpr& initList,
                             SnArrayTypeToken& token,
                             BytecodeEmitter& emitter,
                             uint16_t resultOffset);
    //v0.13.1 D5: the array opcode emission choke points (definitions
    //in EmitArrayOpcodes.cpp) — each owns its opcode's complete operand
    //layout and derives the element descriptor word from the element
    //field, so allocation sites cannot hand-write a stale sequence.
    void EmitAllocArrayN(uint16_t resultOffset, uint16_t arrayTypeIdx,
                         uint8_t rank, uint16_t dimsBase,
                         SnField* pElemField, BytecodeEmitter& emitter);
    void EmitAssembleArrayN(uint16_t resultOffset, uint16_t arrayTypeIdx,
                            uint16_t rowsBase, uint16_t rowCount,
                            SnField* pElemField, BytecodeEmitter& emitter);
    void EmitAllocArrayDyn(uint16_t resultOffset, uint16_t arrayTypeIdx,
                           uint16_t dimsSlot, SnField* pElemField,
                           BytecodeEmitter& emitter);
    void EmitCheckArrayTypeOp(uint16_t slot, uint16_t arrayTypeIdx,
                              SnField* pElemField, BytecodeEmitter& emitter);
    void EmitNDInitListLeaves(SnInitListExpr& node,
                              SnArrayTypeToken& token,
                              std::vector<int32_t>& coords,
                              uint16_t coordBase, uint16_t valueSlot,
                              BytecodeEmitter& emitter,
                              uint16_t resultOffset);
    //Walker-side mirror of the ND literal band: rows never emit, so
    //this is the deepest leaf claim (the band is max(rank, 1 + this)).
    static uint16_t NDLeafPeakDepth(SnInitListExpr& init,
                             const std::unordered_set<SnFunction*>& visited);
    //Stage-3 D3: true when any above-leaf entry is not a bracket row
    //(a plain expression, or a brace-form row the resolver let
    //through) - the dynamic OP_AssembleArrayN arm owns such literals.
    //Shared by the emitter dispatch and the walker peak-depth mirror
    //so the two cannot drift.
    static bool NDInitListHasExpressionRow(SnInitListExpr& node,
                             uint8_t aboveLeaf);
    //Depth of the dynamic arm (see EmitInitListArrayNDynamic).
    static uint16_t NDInitListDynamicPeakDepth(SnInitListExpr& init,
                             const std::unordered_set<SnFunction*>& visited);
    //Stage-3 D3 dynamic arm: rows evaluate into one claim band, then
    //OP_AssembleArrayN builds the record at runtime.
    void EmitInitListArrayNDynamic(SnInitListExpr& initList,
                             SnArrayTypeToken& token,
                             BytecodeEmitter& emitter,
                             uint16_t resultOffset);
    void EmitNewObjectAndNoArgCtor(SnClassDecl& classDecl,
                                   SnFunction* pCtorFn, uint16_t classIdx,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset,
                                   const int32_t* pExtraConstSlot = nullptr);
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
    //0.10.10: stage one entry slot (a Dict key or a List/Dict value) and
    //box it in place when its declared kind is primitive — the shared
    //shape of the "add"/"set" argument slots.
    void EmitInitListStageBoxed(SnExpression& expr, uint16_t off,
                                const BoxingTagResult& box,
                                BytecodeEmitter& emitter);
    void EmitInitListClassForm(SnInitListExpr& initList, SnClassDecl& classDecl,
                               BytecodeEmitter& emitter, uint16_t resultOffset);
    //K1 Slice 9: `Channel<T> c = {};` — construct only (no entries).
    void EmitInitListChannelForm(SnInitListExpr& initList,
                                 SnClassDecl& classDecl,
                                 BytecodeEmitter& emitter, uint16_t resultOffset);
    //Class-shaped dispatcher by base name (List/Dict/Channel erasure;
    //default arm = user class init form).
    void EmitInitListClassTarget(SnInitListExpr& initList,
                                 SnClassDecl& classDecl,
                                 BytecodeEmitter& emitter,
                                 uint16_t resultOffset);
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
    //  4. static class field — module slot (0.12.6 D1/D3: no instance
    //     involved; locals keep shadowing per the bare-pool precedence)
    //  5. NotFound — resolver/codegen model divergence (internal error)
    //Single source of truth shared by the IdentifierExpr read, AssignStmt
    //write, and CompoundAssignStmt paths so the three stay in lockstep.
    struct BareIdTarget {
        enum Kind { Local, ThisField, StaticSlot, NotFound };
        Kind kind = NotFound;
        uint16_t localOffset = 0;   //Local
        uint16_t slotIdx = 0;       //StaticSlot: module slot index
        //ThisField: owning type decl (SnClassDecl or SnStructDecl —
        //0.9.11 D2). Consumers read fieldOff only; the owner exists to
        //keep the binding auditable.
        SnField* owner = nullptr;
        int fieldOff = -1;          //ThisField: flattened field offset
    };
    BareIdTarget ResolveBareIdentifier(SnField* field);
    //0.12.6: the module slot a static class field owns (declaration
    //order, assigned at class registration). Throws on a static field
    //the registration walk never saw — an internal invariant break.
    //v0.12.6 Task 8: non-const — an imported stub field (producer slot
    //rider) mints its placeholder import record on first use, so the
    //lookup mutates the unit table and the field→slot cache.
    uint16_t StaticSlotIndexOf(SnClassField& field);
    //Task 7: the same lookup for a static local, keyed by name in the
    //current function's context (function-scoped by construction).
    uint16_t StaticLocalSlotIndexOf(const SnField& field) const;

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
    //0.12.6 D1: static class field -> its module slot index (assigned
    //in declaration order at class registration; the emit-side mirror
    //of CompiledModule::staticSlots for OP_LoadStatic/OP_StoreStatic).
    std::unordered_map<SnClassField*, uint16_t> m_staticSlotByField;
    //v0.12.6 Task 7 (D11): interface decl -> its interface-table index,
    //filled at RegisterInterfaces; kind-1 gates address the interface
    //owner through it.
    std::unordered_map<SnInterfaceDecl*, uint16_t> m_ifaceIndexByDecl;
    //Archived per-unit function tables (indexed by module idx), the
    //producer-index source for cross-unit import records. Filled by
    //ShadowRegisterUnits.
    std::vector<std::unordered_map<SnFunction*, uint16_t>>
        m_unitFuncIdxByModule;
    //The static-slot sibling of the archive above: per-unit static
    //field -> producer staticSlots index, harvested from the same
    //shadow registration pass (staticSlots order is declaration order
    //within one unit — no prelude interleaving, the prelude declares
    //no statics — so the replay matches the producer image exactly).
    std::vector<std::unordered_map<SnClassField*, uint16_t>>
        m_unitStaticSlotIdxByModule;

    //Phase 6 per-unit mode: the registration walks only see members owned
    //by the selected unit, and cross-unit references become import slots.
    //NO_UNIT = no unit selected — every walk runs inside one BeginUnit
    //scope, so the sentinel only marks the pre-BeginUnit construction
    //state (a real unit index can never reach it). The value matches
    //ModuleRegistry::NO_OWNER numerically; benign by construction:
    //IsOwnUnit treats NO_OWNER as own by design, and the entry scan
    //filters NO_OWNER owners before it ever compares to m_currentUnitIdx.
    static constexpr uint32_t NO_UNIT = 0xFFFFFFFFu;
    uint32_t m_currentUnitIdx = NO_UNIT;
    std::unordered_map<SnEnumDecl*, size_t> m_enumIndexMap;  //Phase 8e-9b: AST enum decl → enumDefIdx (parallel to m_compiledModule.enumNames)
    //v1.12: per-struct resolved field types (parallel to the own-struct
    //entries) — captured at registration, descriptors and field
    //struct/class indices built from the nodes once all tables exist
    //(names alone cannot carry cross-unit provenance for the slots).
    std::vector<std::vector<SnField*>> m_structFieldTypes;
    //Runtime minters (INTR_Dict_Keys, INTR_String_Split) allocate List
    //instances through this index; since phase 2 the declared erasure
    //record is resolved by name in CacheBuiltinClassIndexes.
    int16_t m_listClassIdx = -1;
    int16_t m_dictClassIdx = -1;    //Phase 8e-4: index of Dict<K,V> built-in class (-1 until RegisterBuiltinClasses)

    //Shadowed prelude names (builtin-decls source-of-truth design §6.8):
    //bare names a NON-prelude top-level declaration claims. The prelude's
    //same-name declarations retire from every registration site
    //(innermost-wins); user declarations bind first anyway (project TUs
    //precede the prelude TU on the root), this set keeps the VM tables
    //from carrying the shadowed prelude records. Computed once per build
    //in GenerateUnits, before the per-unit loop.
    std::set<std::string> m_shadowedPreludeNames;

    //Borrowed library declaration index (SetLibraryIndex); null until
    //ModuleBuilder injects it. Codegen reads stdlib signatures from it.
    const langservice::SymbolIndex* m_pLibraryIndex = nullptr;
    //Phase 5: borrowed compile-time registry + diagnostic channel
    //(SetModuleRegistry; one injection point carries both). The registry
    //outlives codegen inside one build; m_pEnv is where the entry
    //scan's ambiguity diagnostic goes (VmBackend has no env of its own).
    const ModuleRegistry* m_pRegistry = nullptr;
    BuildEnvironment* m_pEnv = nullptr;

    //Functions whose default-parameter expressions are currently being
    //emitted (EmitCallArgs recursion guard — round-9, finding 3). Re-entry
    //means the default expansion recurses infinitely (compile-time stack
    //overflow pre-fix); the emitter rejects it with a clean error.
    std::unordered_set<SnFunction*> m_defaultEmitting;

    //Callee default expressions currently walked by CalleeDefaultMaxArgs
    //(recursion guard). The frame-size walker runs before DefaultEmitGuard
    //rejects a recursive default at codegen time, so without this set a
    //self-referential default would descend forever here.
    std::unordered_set<SnFunction*> m_calleeDefaultMaxArgsWalking;

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
    //Layout of the local variable frame (all slots are uniform
    //kFrameSlotBytes = 8-byte cells):
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
        //v0.12.6 Task 7: a static local's name -> module slot index.
        //Function-scoped by construction (reads live in the declaring
        //function only), so the name key is unambiguous per context;
        //the declaration statement registers before any use can emit.
        std::unordered_map<std::string, uint16_t> staticLocalSlots;
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
        //K2 slice 5: every EvalAreaClaim appends its slice + root class;
        //BuildEvalAreaHandleMask OR-merges them per slot (the same slot
        //serves many claims across the function — union, not last-wins).
        struct ClaimClassRecord {
            uint16_t baseOffset;  //absolute frame offset of the slice
            uint16_t slots;
            uint8_t  slotClass;   //EvalSlotClass
        };
        std::vector<ClaimClassRecord> claimClasses;
        //Phase 8e-5: per-function counter for foreach hidden-local uniquification.
        //AllocLocal dedupes by name (VmBackend.cpp:2194); without uniquification,
        //nested foreach loops would collide on __foreach_iter / __foreach_i / __foreach_n.
        uint16_t foreachCounter = 0;
        //v0.12.7 Task 3: the declared return type of the function being
        //emitted (non-owning; null for void) — the return-boundary rank
        //check reads the annotation from it. Static-init contexts leave
        //the default (their synthesized functions are void).
        SnField* pDeclaredReturnType = nullptr;
    };
    FuncContext* m_currFunc = nullptr;

    //Phase 9d-2: the class whose methods are currently being generated.
    //super(...) statements look up the parent class ctor through it.
    //Set in GenerateAllBytecode's class loop; nullptr elsewhere.
    SnClassDecl* m_pCurrClass = nullptr;
    //v0.12.6 Task 7 (D11): the interface whose members are currently
    //being generated — the interface $staticInit arm reads it the way
    //the class arm reads m_pCurrClass. Set in the same walk; nullptr
    //elsewhere.
    SnInterfaceDecl* m_pCurrInterface = nullptr;

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
        //slotClass (K2 slice 5): the EvalSlotClass of every value this
        //claim parks. Dual (default) keeps the conservative both-domain
        //test. A site may pass a narrower class ONLY when it can
        //enumerate everything that transits the slice: nested emissions
        //use claim slots as their resultOffset, so a subtree argument
        //parks all its intermediates there (typing from the subtree's
        //final type alone is unsound). Overlapping claims OR-merge at
        //finalize — union, not last-wins.
        EvalAreaClaim(VmBackend& b, uint16_t slots,
                      uint8_t slotClass = ESC_Dual)
            : m_B(b), m_slots(slots)
        {
            auto& ctx = *m_B.m_currFunc;
            ctx.evalAreaCursor += m_slots * kFrameSlotBytes;
            if (ctx.evalAreaCursor > ctx.observedPeakCursor)
                ctx.observedPeakCursor = ctx.evalAreaCursor;
            ctx.claimClasses.push_back({ base(), m_slots, slotClass });
        }
        ~EvalAreaClaim()
        { m_B.m_currFunc->evalAreaCursor -= m_slots * kFrameSlotBytes; }
        uint16_t base() const
        { return m_B.m_currFunc->evalAreaBase
              + m_B.m_currFunc->evalAreaCursor
              - m_slots * kFrameSlotBytes; }
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

    //--- prelude shadowing + handle-class certification (definitions in
    //backend/DeclPredicates.cpp; consumed by the registration walks and
    //the FieldOffsets mirror. The intrinsic binding table itself lives
    //compiler-side — builder/BindingTable.h).

    //One whole-tree pass collecting the shadowed prelude names (see the
    //m_shadowedPreludeNames member comment). Top-level members only:
    //prelude declarations are all root-level, and method-level shadowing
    //is per-class dispatch, not a bare-name question.
    void ComputeShadowedPreludeNames(SnNamespace& root);

    //True when THIS declaration is a prelude one whose bare name a
    //foreign declaration claims — the registration sites skip it (the
    //foreign declaration is the visible winner).
    bool IsShadowedPreludeDecl(const SnField& node) const;

    //True when this class declaration is a prelude builtin whose record
    //leads with the hidden __handle field (binding-table marker, §6.4).
    //Owner-gated: a user class sharing the name after shadowing is not
    //the builtin. Record insertion (CollectOwnClassFields) and the
    //FieldOffsets mirror consult the SAME predicate so the layouts
    //cannot diverge.
    bool IsHandleClassDecl(const SnField& decl) const;
};

//Intrinsic binding table (builtin-decls source-of-truth design §4.5):
//package-partitioned name→id rows for declarations implemented inside
//the VM — declared in builder/BindingTable.h since the concurrency
//import redesign moved the table to the compiler (it is compile-time
//declaration metadata; the runtime never reads it).

//v0.12.2 (the string-method signature table is retired): the number of
//argument slots a string-method call must stage. The one synthetic case
//is substring's 1-arg form —
//OP_CallIntrinsic carries no argument count, so the missing end slot
//is filled with receiver.length() (emitted as OP_StrLen; the declared
//overloads are the interface, the intrinsic still reads both slots).
//Shared by the emitter and both frame walkers — the three sides keep
//ONE predicate (former table fields trailingDefault/maxArgs).
inline uint16_t StringMethodStagedArgCount(const std::string& name,
    uint16_t argCount)
{
    return (name == "substring" && argCount < 2) ? 2 : argCount;
}

} // namespace nlang
