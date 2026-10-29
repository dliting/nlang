/*---
    BuiltinClasses.cpp — 内建类注册表：流族（ByteStream/FileStream）的
    数据表与单一登记循环。RegisterBuiltinClasses 表驱动化（2026-09-26
    可维护性重构，零行为变化：行序即注册序，类/函数索引由它派生，
    golden 对拍覆盖）。
    Object 与异常族随内建类真源化退役：由 stdlib/builtins.n 的声明类
    提供；List/Dict/Channel/Future 随真源化阶段 2 退役——其记录来自
    声明发射＋绑定表盖章（backend/BindingTable.cpp）。
---*/
#include "VmBackend.h"
#include <nlang/runtime/NodeConsts.h>
#include <iterator>
#include <utility>

namespace nlang {


namespace {

//One method stub: a CompiledFunction with intrinsicId set so
//OP_CallMethod{,Direct} short-circuits to ExecuteIntrinsic.
//paramCount includes the `this` slot; localsSize derives as
//paramCount * kFrameSlotBytes in the push helper.
struct BuiltinMethod {
    const char* name;
    uint16_t intrinsicId;
    uint16_t paramCount;
    uint16_t returnTypeKind;  //RTK_*
};

//fieldClassIndices for a BuiltinField row. Every surviving row's fields
//are plain handles — no class references remain (the List-typed
//backtrace moved to the declared Exception in stdlib/builtins.n).
static constexpr uint16_t BFCLASS_None = 0xFFFF;  //no class reference

struct BuiltinField {
    const char* name;
    uint16_t typeKind;    //RTK_*
    uint16_t classRef;    //BFCLASS_*
};

//Superclass source. BSR_ImplicitObject writes -1, which
//ApplyImplicitObjectInheritance later resolves to the declared prelude
//Object record (its name guard keeps classes NAMED "Object" at -1).
//Every other declared class lives in stdlib/builtins.n — its records
//arrive through the normal declared-class emission, not this table.
enum BuiltinSuperRef : int8_t {
    BSR_ImplicitObject,
};

struct BuiltinClassDecl {
    const char* name;
    BuiltinSuperRef superRef;
    const BuiltinField* fields;
    size_t fieldCount;
    bool hasCtor;             //no ctor stub when false
    uint16_t ctorIntrinsicId;
    uint16_t ctorParamCount;  //includes the `this` slot
    const BuiltinMethod* methods;
    size_t methodCount;
};

//Hidden __handle field shared by the stream/container builtins (index
//into their VM side stores).
const BuiltinField s_HandleFields[] = {
    {"__handle", RTK_Int32, BFCLASS_None},
};

//ByteStream/FileStream methods. paramCount is 1 (`this` only) for every
//stream method: the old name-inference tested capital-W/R "Write"/"Read"
//prefixes against the camelCase names, so its "takes an argument" branch
//never fired, and the value argument travels via callParamBase anyway
//(intrinsics read their argument slots directly, not through declared
//parameters). Return kinds: readInt/
//readStruct/readObject/length/position → int, readFloat → float,
//readLong → long, readDouble → double, readString → string, the rest void.
const BuiltinMethod s_ByteStreamMethods[] = {
    {"writeInt",    INTR_BS_WriteInt,    1, RTK_Void},
    {"readInt",     INTR_BS_ReadInt,     1, RTK_Int32},
    {"writeFloat",  INTR_BS_WriteFloat,  1, RTK_Void},
    {"readFloat",   INTR_BS_ReadFloat,   1, RTK_Float},
    {"writeLong",   INTR_BS_WriteLong,   1, RTK_Void},
    {"readLong",    INTR_BS_ReadLong,    1, RTK_Long},
    {"writeDouble", INTR_BS_WriteDouble, 1, RTK_Void},
    {"readDouble",  INTR_BS_ReadDouble,  1, RTK_Double},
    {"writeString", INTR_BS_WriteString, 1, RTK_Void},
    {"readString",  INTR_BS_ReadString,  1, RTK_String},
    {"writeStruct", INTR_BS_WriteStruct, 1, RTK_Void},
    {"readStruct",  INTR_BS_ReadStruct,  1, RTK_Int32},
    {"writeObject", INTR_BS_WriteObject, 1, RTK_Void},
    {"readObject",  INTR_BS_ReadObject,  1, RTK_Int32},
    {"length",      INTR_BS_Length,      1, RTK_Int32},
    {"position",    INTR_BS_Position,    1, RTK_Int32},
    {"reset",       INTR_BS_Reset,       1, RTK_Void},
    {"close",       INTR_BS_Close,       1, RTK_Void},
};

const BuiltinMethod s_FileStreamMethods[] = {
    {"writeInt",    INTR_FS_WriteInt,    1, RTK_Void},
    {"readInt",     INTR_FS_ReadInt,     1, RTK_Int32},
    {"writeFloat",  INTR_FS_WriteFloat,  1, RTK_Void},
    {"readFloat",   INTR_FS_ReadFloat,   1, RTK_Float},
    {"writeLong",   INTR_FS_WriteLong,   1, RTK_Void},
    {"readLong",    INTR_FS_ReadLong,    1, RTK_Long},
    {"writeDouble", INTR_FS_WriteDouble, 1, RTK_Void},
    {"readDouble",  INTR_FS_ReadDouble,  1, RTK_Double},
    {"writeString", INTR_FS_WriteString, 1, RTK_Void},
    {"readString",  INTR_FS_ReadString,  1, RTK_String},
    {"writeStruct", INTR_FS_WriteStruct, 1, RTK_Void},
    {"readStruct",  INTR_FS_ReadStruct,  1, RTK_Int32},
    {"writeObject", INTR_FS_WriteObject, 1, RTK_Void},
    {"readObject",  INTR_FS_ReadObject,  1, RTK_Int32},
    {"length",      INTR_FS_Length,      1, RTK_Int32},
    {"position",    INTR_FS_Position,    1, RTK_Int32},
    {"close",       INTR_FS_Close,       1, RTK_Void},
};

//The builtin class registration table. Row order IS registration order —
//class and function indices derive from it, so any reorder changes the
//serialized .ncu (golden compare guards this).
const BuiltinClassDecl s_BuiltinClassDecls[] = {
    {"ByteStream", BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        true, INTR_BS_Ctor, 1,
        s_ByteStreamMethods, std::size(s_ByteStreamMethods)},
    {"FileStream", BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        true, INTR_FS_Ctor, 3,
        s_FileStreamMethods, std::size(s_FileStreamMethods)},
};

//Phase 2 of the builtin-decls source-of-truth migration: the generic
//four (List/Dict/Channel/Future) left for stdlib/builtins.n — their
//records come from the declared emission + binding-table stamping.
static_assert(std::size(s_BuiltinClassDecls) == 2,
    "the builtin registry retired to ByteStream/FileStream only; a new "
    "row means a new prelude class the source-of-truth design did not "
    "cover");

//One BuiltinField row → the CompiledClass field arrays. Every surviving
//row's fields are plain int handles — no class-typed builtin fields
//remain (backtrace moved to the declared Exception in builtins.n).
void AppendBuiltinField(CompiledClass& cc, const BuiltinField& f) {
    cc.fieldNames.push_back(f.name);
    cc.fieldTypeKinds.push_back(f.typeKind);
    cc.fieldStructIndices.push_back(0xFFFF);
    cc.fieldClassIndices.push_back(BFCLASS_None);
    cc.fieldAccess.push_back(0);  //private / accessible via field ref
}

//One BuiltinMethod row → a CompiledFunction stub appended to the module;
//returns its function index for cc.methodIndices / cc.constructorIndices.
//Ctor stubs reuse this with a synthesized row (class name + RTK_Void).
uint16_t PushBuiltinMethodFunc(CompiledModule& module,
                               const BuiltinMethod& m) {
    auto funcIdx = static_cast<uint16_t>(module.functions.size());
    CompiledFunction func;
    func.name = m.name;
    func.paramCount = m.paramCount;
    func.localsSize = static_cast<uint16_t>(m.paramCount * kFrameSlotBytes);
    func.returnTypeKind = m.returnTypeKind;
    func.intrinsicId = m.intrinsicId;
    module.functions.push_back(std::move(func));
    return funcIdx;
}

//One BuiltinClassDecl row → its CompiledClass record (fields, ctor stub,
//method stubs). 0.9.11 (v2.6): plural ctor record — a builtin declares at
//most one ctor, so the vector holds zero or one index (no ctor: Future).
//0.9.12 (v2.7): the access/virtuality byte arrays stay parallel to the
//index arrays for wire symmetry. Built-in members are intrinsics, not
//source declarations: they are public and never AST-virtual (their
//runtime name dispatch comes from the no-AST-callee codegen path, not the
//NF_Virtual flag), and a consumer never rebuilds a built-in stub — the
//local registration wins — so these bytes travel but are never read.
CompiledClass BuildBuiltinClass(CompiledModule& module,
                                const BuiltinClassDecl& decl) {
    CompiledClass cc;
    cc.name = decl.name;
    cc.superClassIdx = -1;  //BSR_ImplicitObject: resolved to Object later
    for (size_t i = 0; i < decl.fieldCount; ++i)
        AppendBuiltinField(cc, decl.fields[i]);
    cc.fieldCount = static_cast<uint16_t>(decl.fieldCount);
    if (decl.hasCtor) {
        cc.constructorIndices.push_back(PushBuiltinMethodFunc(
            module,
            BuiltinMethod{decl.name, decl.ctorIntrinsicId,
                          decl.ctorParamCount, RTK_Void}));
        cc.constructorAccess.push_back(
            static_cast<uint8_t>(FA_Public));
    }
    for (size_t i = 0; i < decl.methodCount; ++i) {
        cc.methodIndices.push_back(PushBuiltinMethodFunc(
            module, decl.methods[i]));
        cc.methodAccess.push_back(static_cast<uint8_t>(FA_Public));
        cc.methodVirtual.push_back(0);
    }
    return cc;
}

} //namespace

void VmBackend::RegisterBuiltinClasses() {
    //Every row expands to one CompiledClass plus a ctor stub and method
    //stubs, each with intrinsicId set so OP_CallMethod{,Direct}
    //short-circuits to ExecuteIntrinsic.
    for (const BuiltinClassDecl& decl : s_BuiltinClassDecls) {
        m_compiledModule.classes.push_back(BuildBuiltinClass(
            m_compiledModule, decl));
    }
}

} //namespace nlang
