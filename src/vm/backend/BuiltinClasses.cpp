/*---
    BuiltinClasses.cpp — 内建类注册表：Object/流族/List/Dict/异常族的数据表与单一登记循环。
    RegisterBuiltinClasses 表驱动化（2026-09-26 可维护性重构，零行为变化：
    行序即注册序，类/函数索引由它派生，golden 对拍覆盖）。
---*/
#include "VmBackend.h"
#include <iterator>
#include <utility>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

namespace {

//One method stub: a CompiledFunction with intrinsicId set so
//OP_CallMethod{,Direct} short-circuits to ExecuteIntrinsic.
//paramCount includes the `this` slot; localsSize derives as
//paramCount * VALUE_SIZE in the push helper.
struct BuiltinMethod {
    const char* name;
    uint16_t intrinsicId;
    uint16_t paramCount;
    uint16_t returnTypeKind;  //RTK_*
};

//fieldClassIndices source for a BuiltinField row. BFCLASS_List resolves
//to m_listClassIdx at registration time — List is registered before every
//row that references it (table order guarantees availability).
static constexpr uint16_t BFCLASS_None = 0xFFFF;  //no class reference
static constexpr uint16_t BFCLASS_List = 0xFFFE;  //m_listClassIdx

struct BuiltinField {
    const char* name;
    uint16_t typeKind;    //RTK_*
    uint16_t classRef;    //BFCLASS_*
};

//superClassIdx source. BSR_ImplicitObject writes -1, which
//ApplyImplicitObjectInheritance later resolves to Object (its name guard
//keeps the Object row itself at -1). BSR_ExceptionBase copies
//m_exceptionClassIdx, already captured by the earlier Exception row.
enum BuiltinSuperRef : int8_t {
    BSR_ImplicitObject,
    BSR_ExceptionBase,
};

//Which VmBackend index member captures this class's CompiledClass index
//(the tail switch in RegisterBuiltinClasses). BIDX_None captures nothing.
enum BuiltinIdxMember : uint8_t {
    BIDX_None, BIDX_Object, BIDX_List, BIDX_Dict, BIDX_Exception,
    BIDX_NullPtrExc, BIDX_DivZeroExc, BIDX_OobExc, BIDX_AssertExc, BIDX_IoExc,
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
//virtual intrinsics every class inherits. Method order here is the
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

//Flattened message/backtrace pair carried by Exception and every
//subclass. Layout (slot numbering from 1; slot[0] is the classIdx
//header): slot[1] = message (string-object handle), slot[2] = backtrace
//(heap idx to List<string>). Subclasses flatten the inherited fields
//into their own arrays — AllocClassOnHeap sizes the heap slot by
//fieldCount, so flattened fields are required for subclass instances.
const BuiltinField s_ExceptionFields[] = {
    {"message",   RTK_String, BFCLASS_None},
    {"backtrace", RTK_Class,  BFCLASS_List},
};

//ByteStream/FileStream methods. paramCount is 1 (`this` only) for every
//stream method: the old name-inference tested capital-W/R "Write"/"Read"
//prefixes against the camelCase names, so its "takes an argument" branch
//never fired, and the value argument travels via callParamBase anyway
//(intrinsic ABI per StdLib.h reads slots directly). Return kinds: readInt/
//readStruct/readObject/length/position → int, readFloat → float,
//readString → string, the rest void.
const BuiltinMethod s_ByteStreamMethods[] = {
    {"writeInt",    INTR_BS_WriteInt,    1, RTK_Void},
    {"readInt",     INTR_BS_ReadInt,     1, RTK_Int32},
    {"writeFloat",  INTR_BS_WriteFloat,  1, RTK_Void},
    {"readFloat",   INTR_BS_ReadFloat,   1, RTK_Float},
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
    {"List",       BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        true, INTR_List_Ctor, 1,
        s_ListMethods, std::size(s_ListMethods), BIDX_List},
    {"Dict",       BSR_ImplicitObject, s_HandleFields, std::size(s_HandleFields),
        true, INTR_Dict_Ctor, 1,
        s_DictMethods, std::size(s_DictMethods), BIDX_Dict},
    //Exception family: registered after List/Dict so the backtrace field
    //ref resolves. All ctors share the (this, message) shape; dispatch
    //collapses subclass ctor IDs to the INTR_Exception_Ctor case.
    {"Exception",                 BSR_ImplicitObject, s_ExceptionFields,
        std::size(s_ExceptionFields), true, INTR_Exception_Ctor, 2,
        nullptr, 0, BIDX_Exception},
    {"NullPointerException",      BSR_ExceptionBase, s_ExceptionFields,
        std::size(s_ExceptionFields), true, INTR_NullPointerException_Ctor, 2,
        nullptr, 0, BIDX_NullPtrExc},
    {"DivByZeroException",        BSR_ExceptionBase, s_ExceptionFields,
        std::size(s_ExceptionFields), true, INTR_DivByZeroException_Ctor, 2,
        nullptr, 0, BIDX_DivZeroExc},
    {"IndexOutOfBoundsException", BSR_ExceptionBase, s_ExceptionFields,
        std::size(s_ExceptionFields), true, INTR_IndexOutOfBoundsException_Ctor,
        2, nullptr, 0, BIDX_OobExc},
    {"AssertionException",        BSR_ExceptionBase, s_ExceptionFields,
        std::size(s_ExceptionFields), true, INTR_AssertionException_Ctor, 2,
        nullptr, 0, BIDX_AssertExc},
    {"IOException",               BSR_ExceptionBase, s_ExceptionFields,
        std::size(s_ExceptionFields), true, INTR_IOException_Ctor, 2,
        nullptr, 0, BIDX_IoExc},
};

//One BuiltinField row → the CompiledClass field arrays.
void AppendBuiltinField(CompiledClass& cc, const BuiltinField& f,
                        int16_t listClassIdx) {
    cc.fieldNames.push_back(f.name);
    cc.fieldTypeKinds.push_back(f.typeKind);
    cc.fieldStructIndices.push_back(0xFFFF);
    cc.fieldClassIndices.push_back(f.classRef == BFCLASS_List
        ? static_cast<uint16_t>(listClassIdx) : BFCLASS_None);
    cc.fieldAccess.push_back(0);  //private / accessible via field ref
}

//One BuiltinMethod row → a CompiledFunction stub appended to the module;
//returns its function index for cc.methodIndices / cc.constructorIdx.
//Ctor stubs reuse this with a synthesized row (class name + RTK_Void).
uint16_t PushBuiltinMethodFunc(CompiledModule& module,
                               const BuiltinMethod& m) {
    auto funcIdx = static_cast<uint16_t>(module.functions.size());
    CompiledFunction func;
    func.name = m.name;
    func.paramCount = m.paramCount;
    func.localsSize = static_cast<uint16_t>(m.paramCount * VALUE_SIZE);
    func.returnTypeKind = m.returnTypeKind;
    func.intrinsicId = m.intrinsicId;
    module.functions.push_back(std::move(func));
    return funcIdx;
}

} //namespace

void VmBackend::RegisterBuiltinClasses() {
    //Every row expands to one CompiledClass plus a ctor stub (except
    //Object) and method stubs, each with intrinsicId set so
    //OP_CallMethod{,Direct} short-circuits to ExecuteIntrinsic.
    for (const BuiltinClassDecl& decl : s_BuiltinClassDecls) {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        CompiledClass cc;
        cc.name = decl.name;
        cc.superClassIdx = (decl.superRef == BSR_ExceptionBase)
            ? m_exceptionClassIdx : -1;
        for (size_t i = 0; i < decl.fieldCount; ++i)
            AppendBuiltinField(cc, decl.fields[i], m_listClassIdx);
        cc.fieldCount = static_cast<uint16_t>(decl.fieldCount);
        cc.constructorIdx = 0xFFFF;  //no ctor (Object) unless pushed below
        if (decl.hasCtor) {
            cc.constructorIdx = PushBuiltinMethodFunc(m_compiledModule,
                BuiltinMethod{decl.name, decl.ctorIntrinsicId,
                              decl.ctorParamCount, RTK_Void});
        }
        for (size_t i = 0; i < decl.methodCount; ++i)
            cc.methodIndices.push_back(PushBuiltinMethodFunc(
                m_compiledModule, decl.methods[i]));
        m_compiledModule.classes.push_back(std::move(cc));
        switch (decl.idxMember) {
            case BIDX_Object:     m_objectClassIdx     = static_cast<int16_t>(classIdx); break;
            case BIDX_List:       m_listClassIdx       = static_cast<int16_t>(classIdx); break;
            case BIDX_Dict:       m_dictClassIdx       = static_cast<int16_t>(classIdx); break;
            case BIDX_Exception:  m_exceptionClassIdx  = static_cast<int16_t>(classIdx); break;
            case BIDX_NullPtrExc: m_nullPtrExcClassIdx = static_cast<int16_t>(classIdx); break;
            case BIDX_DivZeroExc: m_divZeroExcClassIdx = static_cast<int16_t>(classIdx); break;
            case BIDX_OobExc:     m_oobExcClassIdx     = static_cast<int16_t>(classIdx); break;
            case BIDX_AssertExc:  m_assertExcClassIdx  = static_cast<int16_t>(classIdx); break;
            case BIDX_IoExc:      m_ioExcClassIdx      = static_cast<int16_t>(classIdx); break;
            default: break;  //BIDX_None — ByteStream/FileStream
        }
    }
}

} //namespace nlang
