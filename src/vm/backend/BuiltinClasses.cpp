/*---
    BuiltinClasses.cpp — 内建类注册表：Object/流族/List/Dict/异常族的数据表与单一登记循环。
    RegisterBuiltinClasses 表驱动化（2026-09-26 可维护性重构，零行为变化：
    行序即注册序，类/函数索引由它派生，golden 对拍覆盖）。
---*/
#include "VmBackend.h"
#include <nlang/runtime/BuiltinGenericNames.h>
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
//ApplyImplicitObjectInheritance later resolves to Object (its name guard
//keeps the Object row itself at -1). The declared Exception family lives
//in stdlib/builtins.n — its records arrive through the normal declared-
//class emission, not this table.
enum BuiltinSuperRef : int8_t {
    BSR_ImplicitObject,
};

//Which VmBackend index member captures this class's CompiledClass index
//(the tail switch in RegisterBuiltinClasses). BIDX_None captures nothing.
//The Exception family left the table — declared in stdlib/builtins.n.
enum BuiltinIdxMember : uint8_t {
    BIDX_None, BIDX_Object, BIDX_List, BIDX_Dict,
};

struct BuiltinClassDecl {
    const char* name;
    BuiltinSuperRef superRef;
    const BuiltinField* fields;
    size_t fieldCount;
    bool hasCtor;             //Object alone has no ctor function
    uint16_t ctorIntrinsicId;
    uint16_t ctorParamCount;  //includes the `this` slot
    const BuiltinMethod* methods;
    size_t methodCount;
    BuiltinIdxMember idxMember;
};

//Object — root of the class hierarchy: no fields, no ctor; the three
//protocol intrinsics every class inherits (name-dispatched at runtime
//through the no-AST-callee codegen path — they are not AST-declared
//virtuals, so the methodVirtual wire byte stays 0). Method order here is the
//cc.methodIndices order. (toString's intrinsic *numeric ID* is
//non-contiguous — 61, not 44 — because List/Dict intrinsics were
//allocated before that phase; only the ID, not the order.)
const BuiltinMethod s_ObjectMethods[] = {
    {"equals",      INTR_Object_Equals,      2, RTK_Int32},
    {"getHashCode", INTR_Object_GetHashCode, 1, RTK_Int32},
    {"toString",    INTR_Object_toString,    1, RTK_String},
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
//(intrinsic ABI per StdLib.h reads slots directly). Return kinds: readInt/
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

//List<T>/Dict<K,V> — erasure-style generics: all instantiations share
//this single CompiledClass; per-T boxing is decided at call sites from
//the outer's generic type args (stored on the synthetic SnClassDecl).
const BuiltinMethod s_ListMethods[] = {
    {"add",      INTR_List_Add,      2, RTK_Void},
    {"get",      INTR_List_Get,      2, RTK_Int32},  //returns T, codegen unboxes
    {"set",      INTR_List_Set,      3, RTK_Void},
    {"length",   INTR_List_Length,   1, RTK_Int32},
    {"removeAt", INTR_List_RemoveAt, 2, RTK_Void},
    {"indexOf",  INTR_List_IndexOf,  2, RTK_Int32},
    {"contains", INTR_List_Contains, 2, RTK_Int32},
    {"clear",    INTR_List_Clear,    1, RTK_Void},
    {"toString", INTR_List_toString, 1, RTK_String},
};

const BuiltinMethod s_DictMethods[] = {
    {"set",         INTR_Dict_Set,         3, RTK_Void},
    {"get",         INTR_Dict_Get,         2, RTK_Int32},  //returns V, codegen unboxes
    {"containsKey", INTR_Dict_ContainsKey, 2, RTK_Int32},
    {"remove",      INTR_Dict_Remove,      2, RTK_Int32},
    {"clear",       INTR_Dict_Clear,       1, RTK_Void},
    {"count",       INTR_Dict_Count,       1, RTK_Int32},
    {"keys",        INTR_Dict_Keys,        1, RTK_Class},  //fresh List<K> heap ref
    {"toString",    INTR_Dict_toString,    1, RTK_String},
};

//K1 concurrency builtins. Channel takes the __handle shape as the
//containers (index into the VM-side sync stores). Channel's methods are
//generic-container surface (the element type is erased at runtime — every
//element is a heap idx; recv's RTK_Int32 is the placeholder return kind,
//the call site's boxing plan unboxes per T).
//Mutex/Semaphore and Thread left the table: their classes are declared
//in stdlib/builtins.n, their intrinsic stubs come from the declared
//records stamped by the binding table (backend/BindingTable.cpp).
const BuiltinMethod s_ChannelMethods[] = {
    {"send",  INTR_Channel_Send,  2, RTK_Void},   //this + boxed value
    {"recv",  INTR_Channel_Recv,  1, RTK_Int32},  //this; returns T
    {"close", INTR_Channel_Close, 1, RTK_Void},
};

//K3: Future<T> — the typed-spawn result handle. __handle = context id
//(the declared Thread takes the same hidden slot); no ctor —
//thread.spawn mints instances. get's RTK_Int32 is the placeholder
//return kind (the call site's boxing plan unboxes per T, like recv).
const BuiltinMethod s_FutureMethods[] = {
    {"get",     INTR_Future_Get,    1, RTK_Int32},  //this; returns T
    {"isDone",  INTR_Future_IsDone, 1, RTK_Bool},
};

//The builtin class registration table. Row order IS registration order —
//class and function indices derive from it, so any reorder changes the
//serialized .ncu (golden compare guards this).
const BuiltinClassDecl s_BuiltinClassDecls[] = {
    {"Object",     BSR_ImplicitObject, nullptr, 0, false, 0, 0,
        s_ObjectMethods, std::size(s_ObjectMethods), BIDX_Object},
    {"ByteStream", BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        true, INTR_BS_Ctor, 1,
        s_ByteStreamMethods, std::size(s_ByteStreamMethods), BIDX_None},
    {"FileStream", BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        true, INTR_FS_Ctor, 3,
        s_FileStreamMethods, std::size(s_FileStreamMethods), BIDX_None},
    {kBuiltinListTypeName, BSR_ImplicitObject,
        s_HandleFields, std::size(s_HandleFields),
        true, INTR_List_Ctor, 1,
        s_ListMethods, std::size(s_ListMethods), BIDX_List},
    {kBuiltinDictTypeName, BSR_ImplicitObject,
        s_HandleFields, std::size(s_HandleFields),
        true, INTR_Dict_Ctor, 1,
        s_DictMethods, std::size(s_DictMethods), BIDX_Dict},
    //K1 concurrency builtins — appended at the END (existing indices are
    //stable; a reorder changes the serialized .ncu).
    {"Channel", BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        //K3 task 4: ctor is (this, capacity) — capacity 0 = unbounded;
        //the init-list arm stages the 0 explicitly.
        true, INTR_Channel_Ctor, 2,
        s_ChannelMethods, std::size(s_ChannelMethods), BIDX_None},
    {"Future",  BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        false, 0, 0,
        s_FutureMethods, std::size(s_FutureMethods), BIDX_None},
};

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
//most one ctor, so the vector holds zero or one index (no ctor: Object).
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
    //Every row expands to one CompiledClass plus a ctor stub (except
    //Object) and method stubs, each with intrinsicId set so
    //OP_CallMethod{,Direct} short-circuits to ExecuteIntrinsic.
    for (const BuiltinClassDecl& decl : s_BuiltinClassDecls) {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        m_compiledModule.classes.push_back(BuildBuiltinClass(
            m_compiledModule, decl));
        switch (decl.idxMember) {
            case BIDX_Object:     m_objectClassIdx     = static_cast<int16_t>(classIdx); break;
            case BIDX_List:       m_listClassIdx       = static_cast<int16_t>(classIdx); break;
            case BIDX_Dict:       m_dictClassIdx       = static_cast<int16_t>(classIdx); break;
            default: break;  //BIDX_None — ByteStream/FileStream/Channel/Future
        }
    }
}

} //namespace nlang
