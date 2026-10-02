#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/compiler/TranslationUnit.h>
#include <nlang/runtime/Module.h>
#include <nlang/runtime/NodeConsts.h>
#include "builder/ModuleRegistry.h"
#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <functional>
#include <map>
#include <unordered_set>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

VmBackend::VmBackend()
    : m_EmitVisitor(*this, NVK_CustomTraverse)
{
}
VmBackend::~VmBackend() = default;

void VmBackend::OnModuleCreate(Module& module) {
    m_compiledModule.name = module.Name().ToString();
    //Transitional (phase 6): the module path is spelled as the output
    //module name until per-unit compilation gives every unit its own
    //dotted path (Task 3).
    m_compiledModule.modulePath = m_compiledModule.name;
}

void VmBackend::GenerateTypes(SnNamespace& root) {
    //VmBackend uses multi-pass compilation in GenerateStatements (register types,
    //then functions, then generate code). Type and data registration happen there,
    //not in these separate phases. LLVM backend uses these phases differently
    //because LLVM IR supports forward references.
}

void VmBackend::GenerateData(SnNamespace& root) {
    //See GenerateTypes comment.
}

void VmBackend::GenerateStatements(SnNamespace& root) {
    RegisterBuiltinClasses();
    //Phase 9c cross-module: Phase A merges imported classes/structs/arrayTypes
    //(and builds stringMap) BEFORE user RegisterStructs/Classes/ArrayTypes run,
    //so their internal FindClass/FindStruct/FindArray queries find imported types.
    MergeImportedClassesStructsArrays();
    RegisterStructs(root);
    RegisterClasses(root);
    ResolveStructClassRefs();
    RegisterArrayTypes(root);
    RegisterEnums(root);
    RegisterFunctions(root);
    //Phase 9c cross-module: Phase B merges imported enums/functions/bytecode
    //AFTER RegisterEnums/RegisterFunctions (their clear() would erase Phase B
    //data if it ran earlier), and completes class metadata remap.
    MergeImportedFinalize();
    PopulateClassMethods(root);
    GenerateAllBytecode(root);
}

void VmBackend::ForEachDeclNode(SnField& parent,
    const std::function<void(SnField&)>& fn) {
    //Enum: value members are not containers; visit its methods as functions.
    if (parent.Kind() == NK_EnumDecl) {
        for (auto& method : static_cast<SnEnumDecl&>(parent).Methods())
            fn(method);
        return;
    }
    //Only namespace/class/interface own a member list to descend. Structs
    //hold fields only; fields/params/values are leaves here.
    if (parent.Kind() != NK_Namespace
        && parent.Kind() != NK_ClassDecl
        && parent.Kind() != NK_InterfaceDecl)
        return;
    auto& members =
        static_cast<SnFunctionParentField&>(parent).Members();
    for (auto& child : members) {
        fn(child);
        ForEachDeclNode(child, fn);
    }
}

void VmBackend::GenerateBytecodeRecursive(SnField& parent) {
    //Enum methods (enum is not a SnFunctionParentField).
    if (parent.Kind() == NK_EnumDecl) {
        for (auto& method : static_cast<SnEnumDecl&>(parent).Methods()) {
            if (!method.Body() && !method.ContainFlags(NF_Native))
                continue;
            auto it = m_funcIndexMap.find(&method);
            if (it != m_funcIndexMap.end())
                GenerateFunction(method, it->second);
        }
        return;
    }
    if (parent.Kind() != NK_Namespace
        && parent.Kind() != NK_ClassDecl
        && parent.Kind() != NK_InterfaceDecl)
        return;
    SnClassDecl* prevClass = m_pCurrClass;
    if (parent.Kind() == NK_ClassDecl)
        m_pCurrClass = static_cast<SnClassDecl*>(&parent);
    auto& members =
        static_cast<SnFunctionParentField&>(parent).Members();
    for (auto& child : members) {
        if (child.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(child);
            if (func.Body() || func.ContainFlags(NF_Native)) {
                auto it = m_funcIndexMap.find(&func);
                if (it != m_funcIndexMap.end())
                    GenerateFunction(func, it->second);
            }
        }
        GenerateBytecodeRecursive(child);
    }
    m_pCurrClass = prevClass;
}

//v1.12 type descriptors: capture the true formal/return types for
//cross-module stub reconstruction. Runs before the native branch so
//body-less native declarations serialize their signature too. Methods
//contribute one descriptor per AST formal — the implicit this slot
//has none, mirroring defaultValues' sizing (stub consumers never see
//method records; the count field keeps the wire self-describing).
void VmBackend::CollectSignatureTypeDescs(SnFunction& func,
                                          CompiledFunction& compiledFunc) {
    for (auto& param : func.Params()) {
        ParamTypeDesc ptd;
        if (param.ContainFlags(NF_Out))
            ptd.flags |= PTDF_Out;
        ptd.type = BuildTypeDesc(param.EvalDataType(), LeafSlotResolvers());
        compiledFunc.paramTypeDescs.push_back(std::move(ptd));
    }
    if (func.HasReturn() && func.ReturnType())
        compiledFunc.returnTypeDesc = BuildTypeDesc(
            func.ReturnType()->Field(), LeafSlotResolvers());
}

//Phase 9f: native function declaration (`native int f(...);`). No
//bytecode — the VM dispatches by name through the host-registered
//native table (VmExecutor::RegisterNative). The record carries only
//the signature: the native reads args directly from the caller's
//callParamBase cells and writes the return into pResult.
void VmBackend::FillNativeFunctionRecord(SnFunction& func,
                                         CompiledFunction& compiledFunc) {
    compiledFunc.isNative = true;
    bool isMethod = func.Parent()
        && (func.Parent()->Kind() == NK_ClassDecl
            || func.Parent()->Kind() == NK_EnumDecl);
    compiledFunc.paramCount = static_cast<uint16_t>(
        func.Params().size() + (isMethod ? 1 : 0));
    compiledFunc.localsSize = compiledFunc.paramCount * VALUE_SIZE;
    if (func.HasReturn() && func.ReturnType()) {
        compiledFunc.returnTypeKind = SerializedReturnKind(func);
    } else {
        compiledFunc.returnTypeKind = RTK_Void;
    }
    //Defaults are signature metadata and must be serialized here too:
    //a cross-module consumer's stub (CreateFunctionStub) rebuilds
    //them from defaultValues — same reasoning as Option B. Without
    //this, `native int f(int a, int b = 22)` works in-module (AST
    //path) but loses the default after import.
    for (auto& param : func.Params()) {
        auto dv = ExtractDefaultValue(param.Value());
        if (param.Value() && !dv.hasDefault())
            dv.tag = RTK_Unfoldable;
        compiledFunc.defaultValues.push_back(dv);
    }
}

//Allocate slot 0 (`this` for class/enum methods) and one slot per formal,
//then collect the constant-foldable default values for serialization.
void VmBackend::AllocParamsAndDefaults(SnFunction& func, FuncContext& ctx,
                                       CompiledFunction& compiledFunc) {
    // If this is a class or enum method, allocate slot 0 for 'this'.
    bool isMethod = func.Parent()
        && (func.Parent()->Kind() == NK_ClassDecl
            || func.Parent()->Kind() == NK_EnumDecl);
    if (isMethod) {
        //Phase 12: an enum method's `this` is the enum VALUE (int32), not
        //a heap reference — RTK_Class here would make GC root scanning
        //treat the integer as a heap index (plan 12b round-1 MAJOR 2/3).
        uint8_t thisKind =
            (func.Parent()->Kind() == NK_EnumDecl) ? RTK_Int32 : RTK_Class;
        AllocLocal("__this", VALUE_SIZE, thisKind, true);
    }

    // Allocate slots for parameters
    for (auto& param : func.Params()) {
        AllocLocal(param.Name(), VALUE_SIZE,
                   RuntimeTypeKind(param.EvalDataType()),
                   true);
    }
    compiledFunc.paramCount = static_cast<uint16_t>(
        func.Params().size() + (isMethod ? 1 : 0));

    //Option B: extract constant-foldable default values for each formal.
    //If a formal has a default expression but ExtractDefaultValue can't
    //fold it (e.g. `b = helper()` or `b = a + 1`), stamp RTK_Unfoldable
    //so the consumer side can emit a precise "cross-module default must
    //be literal" error rather than silently treating it as "no default".
    //In-module callers ignore compiledFunc.defaultValues entirely; they
    //use the AST default expression directly via StatementResolver.
    for (auto& param : func.Params()) {
        auto dv = ExtractDefaultValue(param.Value());
        if (param.Value() && !dv.hasDefault())
            dv.tag = RTK_Unfoldable;
        compiledFunc.defaultValues.push_back(dv);
    }
}

//Reserve the frame tail: return-value slot, the 4-slot temp pool, and the
//call parameter area + evalArea. Returns the walker's call-slot stats so
//the finalize-time drift check can compare against the reservation.
VmBackend::CallSlotStats VmBackend::ReserveReturnAndCallSlots(
    SnFunction& func, FuncContext& ctx,
    CompiledFunction& compiledFunc) {
    // Return type
    if (func.HasReturn() && func.ReturnType()) {
        compiledFunc.returnTypeKind = SerializedReturnKind(func);
        ctx.returnSlot = ctx.nextOffset;
        ctx.nextOffset += VALUE_SIZE;
    } else {
        //Void functions carry RTK_Void so cross-module stubs rebuild
        //without a return type (CreateFunctionStub: RTK_Void →
        //HasReturn() == false). The previous default of 0 (RTK_Int32)
        //made an imported void stub claim an int return value.
        compiledFunc.returnTypeKind = RTK_Void;
    }

    // Temporary slots pool (4 slots of pure scratch, each consumed
    // immediately after emission — struct deep-copy in EmitBinding, const
    // staging before AllocArray. Live operands across nested emission go
    // through EvalAreaClaim; nothing parks here by design).
    ctx.tempSlot = ctx.nextOffset;
    ctx.nextOffset += VALUE_SIZE;
    ctx.tempSlot2 = ctx.nextOffset;
    ctx.nextOffset += VALUE_SIZE;
    ctx.tempSlot3 = ctx.nextOffset;
    ctx.nextOffset += VALUE_SIZE;
    ctx.tempSlot4 = ctx.nextOffset;
    ctx.nextOffset += VALUE_SIZE;

    //Call parameter area + evalArea. callParamBase is the final landing
    //zone consumed by OP_CallFunc; evalArea is a disjoint staging area
    //where bindings emit (cursor-based, stack-disciplined). Sizes are
    //computed by ComputeCallSlotStats.
    auto stats = ComputeCallSlotStats(func);
    //Phase 9c follow-up: walker now tracks all implicit calls (InvokeExpr,
    //NewExpr ctor, List/Dict init implicit method calls). Use computed
    //value directly; min 1 (defensive — ensures callParamBase always
    //exists even for leaf functions).
    ctx.callParamSlots = stats.maxArgs > 1 ? stats.maxArgs : 1;
    ctx.callParamBase  = ctx.nextOffset;
    ctx.nextOffset    += ctx.callParamSlots * VALUE_SIZE;
    ctx.evalAreaBase   = ctx.nextOffset;
    ctx.nextOffset    += stats.peakDepth * VALUE_SIZE;
    return stats;
}

void VmBackend::EmitBodyAndImplicitReturn(SnFunction& func, FuncContext& ctx,
                                          BytecodeEmitter& emitter) {
    // Generate bytecode for body
    if (func.Body()) {
        for (auto& stmt : func.Body()->Statements()) {
            EmitStatement(stmt, emitter);
        }
    }

    // Append implicit return (fallback for functions that don't hit an
    // explicit return statement — e.g. void functions, or fall-through).
    if (func.HasReturn()) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(ctx.returnSlot);
    }
    emitter.Emit(OpCode::OP_Return);
}

//Walker-drift tripwire: the frame reserved stats.peakDepth slots for
//the evalArea, and every claim during emission must have fit inside.
//observedPeakCursor > reserved means the walker under-predicted some
//codegen claim — the emitted bytecode would stack-walk past the frame
//at runtime. Fail the build here instead (Phase 10 audit round-8
//closing move: drift is a compiler error, never a silent OOB).
//Scope note: this guards the evalArea only. callParamBase (sized from
//the MaxArgsWalker, no observed counterpart) can still under-count
//silently — keep MaxArgsWalker symmetric with every call-emitting path.
void VmBackend::CheckEvalAreaWalkerDrift(SnFunction& func, FuncContext& ctx,
                                         const CallSlotStats& stats) {
    if (ctx.observedPeakCursor > stats.peakDepth * VALUE_SIZE) {
        throw std::runtime_error(
            "VmBackend: evalArea walker drift in function '"
            + func.Name() + "': claims need "
            + std::to_string(ctx.observedPeakCursor / VALUE_SIZE)
            + " slots but frame reserved "
            + std::to_string(stats.peakDepth));
    }
}

//Emit code leaving the expression's value in frame slot `resultOffset`.
//
//SLOT CONTRACT (Phase 10 audit closing invariant — see the staging-bug
//family history in ExprPeakDepth/FuncContext comments):
//  - `resultOffset` may be any frame slot (user local, temp, claim slot,
//    callParamBase area). Callees must treat it as write-only for the
//    final value; intermediate operands never park in temps across a
//    nested EmitExpression call.
//  - Any operand that must survive a nested emission (the RIGHT operand
//    of a binary, an initializer entry, a receiver, a staged RHS...) is
//    parked in an exclusive EvalAreaClaim slot — never a temp. Temps are
//    pure scratch, consumed by the very next opcode (EmitBinding's
//    CopyStruct, const staging before AllocArray); nothing may read a
//    temp after a nested emission.
//  - Every EvalAreaClaim must be mirrored by ExprPeakDepth/StmtPeakDepth
//    (over-reserving is the safe direction); the finalize-time
//    observedPeakCursor check turns any drift into a build error.
void VmBackend::EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                                uint16_t resultOffset) {
    m_pCurrEmitter = &emitter;
    m_resultOffset = resultOffset;
    expr.Accept(m_EmitVisitor);
}

//Round-13: type-reference expressions (ArrayTypeExpr, GenericTypeExpr)
//are compile-time-only — they carry type information but never produce
//runtime values. Reaching EmitExpression means a caller passed one as a
//value-producing expression (round-13 root cause: SnNewExpr::Args() is
//a view over ALL children and includes the AddChild'ed class name).
//This is an internal invariant break — surface it instead of emitting
//garbage or silently skipping (which masks resolver/AST bugs).
void VmBackend::EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter) {
    m_pCurrEmitter = &emitter;
    NodeKind kind = stmt.Kind();

    //Emit a line marker at every statement so the VM can produce source
    //location info in backtraces and runtime errors. Skipped for
    //paragraphs (they are containers, not statements with their own
    //source location) and local declarations: the decomposition pattern
    //inserts an auto-created AssignStmt per initializer at the same
    //source line, so a declaration anchor would double every
    //`int x = init;` stop (ndb steps one stop per user statement; each
    //declarator's initializer keeps its own AssignStmt anchor, and a
    //bare `int x;` emits nothing executable beyond struct slot
    //allocation, which cannot raise).
    //Loop statements skip the prologue too: their anchor is emitted at
    //the back-edge landing inside each loop emitter (EmitStatementAnchor
    //there), so the loop line fires every iteration instead of only on
    //entry — an empty-body loop otherwise has zero checkpoints per
    //iteration and a break on its line can never re-hit.
    if (kind != NK_Paragraph && kind != NK_LocalDeclStmt
        && kind != NK_WhileStmt && kind != NK_DoStmt && kind != NK_ForStmt) {
        EmitStatementAnchor(stmt, emitter);
    }

    stmt.Accept(m_EmitVisitor);
}

void VmBackend::Access(SyntaxNode& sn) {
    throw std::runtime_error(
        "NLang backend: non-emissible node reached codegen: "
        + std::to_string(static_cast<int>(sn.Kind())));
}

uint16_t VmBackend::AllocLocal(const std::string& name, uint16_t size,
                                uint8_t typeKind, bool isParam) {
    assert(m_currFunc);
    auto it = m_currFunc->localOffsets.find(name);
    if (it != m_currFunc->localOffsets.end())
        return it->second;  // reuse existing slot (flat frame, no block scoping)

    uint16_t offset = m_currFunc->nextOffset;
    m_currFunc->localOffsets[name] = offset;
    m_currFunc->nextOffset += size;

    LocalDescriptor desc;
    desc.offset = offset;
    desc.size = size;
    desc.typeKind = typeKind;
    desc.isParam = isParam ? 1 : 0;
    desc.name = name;
    m_currFunc->func->locals.push_back(std::move(desc));

    return offset;
}

uint16_t VmBackend::FindLocal(const std::string& name) const {
    assert(m_currFunc);
    auto it = m_currFunc->localOffsets.find(name);
    if (it != m_currFunc->localOffsets.end())
        return it->second;
    throw std::runtime_error("NLang backend: local variable not found: " + name);
}

VmBackend::BareIdTarget VmBackend::ResolveBareIdentifier(SnField* field) {
    BareIdTarget t;
    if (!field)
        return t;
    auto it = m_currFunc->localOffsets.find(field->Name());
    if (it != m_currFunc->localOffsets.end()) {
        t.kind = BareIdTarget::Local;
        t.localOffset = it->second;
        return t;
    }
    if (auto* pOwner = OwningClassOfMemberField(field)) {
        int off = FindClassFieldOffset(*pOwner, field->Name());
        if (off >= 0) {
            t.kind = BareIdTarget::ThisField;
            t.owner = pOwner;
            t.fieldOff = off;
            return t;
        }
    }
    return t;
}

bool VmBackend::SaveModule(BuildEnvironment& env) {
    return WriteModuleArtifact(env, m_compiledModule, m_entryKey);
}

bool VmBackend::WriteModuleArtifact(BuildEnvironment& env,
                                    const CompiledModule& module,
                                    const std::string& entryKey) {
    std::string sFilePath;
    namespace bf = std::filesystem;
    const BuildParams& setting = env.Params();
    const std::string sFileName = setting.m_sOutputModule + NCU_EXTENSION;

    bf::path modulePath;
    if (setting.m_sOutputDir.empty()) {
        modulePath = bf::current_path() / sFileName;
    } else {
        modulePath = bf::path(setting.m_sOutputDir);
        if (!bf::exists(modulePath)) {
            bf::create_directory(modulePath);
        }
        modulePath /= sFileName;
    }
    sFilePath = modulePath.string();

    env.Log(CLL_Info, "Output module file: %s ...", sFilePath.c_str());

    std::ofstream fs(sFilePath, std::ios::binary);
    if (!fs.is_open()) {
        env.Log(CLL_Fatal, "Failed to open file: %s.", sFilePath.c_str());
        return false;
    }

    //Serialization lives in WriteCompiledModule (ModuleSaver.cpp) — the
    //single .ncu writer, shared with unit tests so hand-written byte
    //layouts cannot drift from the reader (ModuleLoader).
    if (!WriteCompiledModule(fs, module, entryKey)) {
        env.Log(CLL_Fatal, "Failed to write module: %s.", sFilePath.c_str());
        return false;
    }
    return true;
}

} // namespace nlang
