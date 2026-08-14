#pragma once
#include <nlang/compiler/ICodeBackend.h>
#include "CompiledModule.h"
#include "BytecodeEmitter.h"
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

private:
    void GenerateFunction(SnFunction& func, size_t funcIdx);
    void EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                        uint16_t resultOffset);
    void EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter);

    //Phase 9c: per-argument boxing plan for built-in generic class methods
    //(List<T>, Dict<K,V>). Maps callParamBase slot index to {type tag, needs
    //box}. Empty for user methods (no boxing — values pass as heap idxs).
    struct ArgBoxPlan { uint8_t tag; bool needsBox; };

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
    void EmitCallArgs(const SnInvokeExpr& invoke, SnFunction* pCallee,
                      BytecodeEmitter& emitter, size_t slotBase = 0,
                      const std::map<uint16_t, ArgBoxPlan>* pArgPlans = nullptr,
                      uint16_t thisSlot = UINT16_MAX);

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

    //Compilation phases (called by GenerateStatements in order).
    //Each phase corresponds to a distinct compilation pass over the AST.
    //Future evolution: each phase can become an Accessor for multi-backend support.
    void RegisterBuiltinClasses();
    void RegisterStructs(SnNamespace& root);
    void RegisterClasses(SnNamespace& root);
    void ResolveStructClassRefs();
    void RegisterArrayTypes(SnNamespace& root);
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

    //Register an array type from its element type field.
    //Returns the arrayTypeIdx in m_compiledModule.arrayTypes.
    uint16_t RegisterArrayType(SnField* pElemType);

    static uint8_t RuntimeTypeKind(SnField* pType);
    //Option B Step 3: extract a constant-foldable default expression into
    //a DefaultValueDesc for serialization. Returns tag=RTK_Void when the
    //expression isn't foldable (caller-side check rejects for IsImported).
    DefaultValueDesc ExtractDefaultValue(SnExpression* pExpr);
    //Phase 8e-4: RTK_* tag for boxing a primitive-T argument, plus an
    //isPrimitive flag (needed because RTK_Int32 == 0 — same collision as
    //the Phase 8e-3 C1 fix). Shared by List<T> and Dict<K,V> codegen.
    struct BoxingTagResult { uint8_t tag; bool isPrimitive; };
    static BoxingTagResult BoxingTagFor(SnField* pT);
    uint16_t AllocLocal(const std::string& name, uint16_t size,
                        uint8_t typeKind, bool isParam);
    uint16_t FindLocal(const std::string& name) const;
    uint16_t AddStringConstant(const std::string& s);

    //Pick a temp slot distinct from `exclude` so a sub-expression can use it
    //without clobbering `exclude`. With only two temp slots available, the
    //rule is: if exclude == tempSlot, return tempSlot2; otherwise return
    //tempSlot. This composes for nested expressions because each level
    //alternates between tempSlot and tempSlot2.
    //Used by: BinaryExpr (right operand), SubscriptExpr (index), and any
    //other expression that needs one extra slot besides its resultOffset.
    uint16_t PickTempSlot(uint16_t exclude) const;

    CompiledModule m_compiledModule;
    std::unordered_map<SnFunction*, size_t> m_funcIndexMap;
    std::unordered_map<SnEnumDecl*, size_t> m_enumIndexMap;  //Phase 8e-9b: AST enum decl → enumDefIdx (parallel to m_compiledModule.enumNames)
    std::vector<std::vector<std::string>> m_structFieldTypeNames;
    int16_t m_objectClassIdx = -1;  //Phase 8e-1: index of synthesized Object class (-1 until RegisterBuiltinClasses)
    int16_t m_listClassIdx = -1;    //Phase 8e-3: index of List<T> built-in class (-1 until RegisterBuiltinClasses)
    int16_t m_dictClassIdx = -1;    //Phase 8e-4: index of Dict<K,V> built-in class (-1 until RegisterBuiltinClasses)
    int16_t m_exceptionClassIdx = -1;   //Phase 9d: Exception base class
    int16_t m_nullPtrExcClassIdx = -1;  //Phase 9d: NullPointerException
    int16_t m_divZeroExcClassIdx = -1;  //Phase 9d: DivByZeroException
    int16_t m_oobExcClassIdx = -1;      //Phase 9d: IndexOutOfBoundsException
    int16_t m_assertExcClassIdx = -1;   //Phase 9d: AssertionException

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

    //Per-loop code generation context.
    //Reference: EN's Compiler::NestBreaks/NestContinues (Compiler.h:108-111).
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
    //The 4-slot temp pool supports PickTempSlot's depth-chaining for nested
    //binary expressions (e.g. `a == b*c + d` needs 3 distinct slots: outer-left,
    //inner-left-result, inner-right). Pre-8e-1.5 had only 2 slots which caused
    //the outer-left to be clobbered by inner-right intermediates.
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
        { m_B.m_currFunc->evalAreaCursor += m_slots * 4; }
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
