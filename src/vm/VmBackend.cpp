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

void VmBackend::RegisterBuiltinClasses() {
    //Register built-in classes (ByteStream, FileStream) as CompiledClass entries
    //with stub CompiledFunction entries for their methods. Each stub has
    //intrinsicId set so OP_CallMethod{,Direct} short-circuits to ExecuteIntrinsic.

    //Phase 8e-1: synthesize the implicit Object base class first.
    //Object is the root of the class hierarchy: every user class with no
    //explicit parent inherits from Object. Object itself has superClassIdx==-1.
    //Methods Equals(Object)→int and GetHashCode()→int are virtual; subclasses
    //override them by name (existing name-based dispatch handles this).
    {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        CompiledClass cc;
        cc.name = "Object";
        cc.superClassIdx = -1;
        cc.fieldCount = 0;
        cc.constructorIdx = 0xFFFF;  //no ctor

        //int Equals(Object other) — virtual, intrinsic dispatch.
        auto equalsFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction equalsFunc;
        equalsFunc.name = "equals";
        equalsFunc.paramCount = 2;  //this + other
        equalsFunc.localsSize = 2 * VALUE_SIZE;
        equalsFunc.returnTypeKind = RTK_Int32;
        equalsFunc.intrinsicId = INTR_Object_Equals;
        m_compiledModule.functions.push_back(std::move(equalsFunc));
        cc.methodIndices.push_back(equalsFuncIdx);

        //int GetHashCode() — virtual, intrinsic dispatch.
        auto getHashCodeFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction ghFunc;
        ghFunc.name = "getHashCode";
        ghFunc.paramCount = 1;  //this only
        ghFunc.localsSize = 1 * VALUE_SIZE;
        ghFunc.returnTypeKind = RTK_Int32;
        ghFunc.intrinsicId = INTR_Object_GetHashCode;
        m_compiledModule.functions.push_back(std::move(ghFunc));
        cc.methodIndices.push_back(getHashCodeFuncIdx);

        //Phase 8e-9b: string toString() — virtual, intrinsic dispatch.
        //Registration order is "紧跟 GetHashCode" (semantic adjacency in the
        //Object class block); the intrinsic *numeric ID* is non-contiguous
        //(61, not 44) because List/Dict intrinsics (44-60) were allocated
        //before this phase. Only the ID is non-contiguous, the registration
        //order in cc.methodIndices remains equals, getHashCode, toString.
        auto toStringFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction tsFunc;
        tsFunc.name = "toString";
        tsFunc.paramCount = 1;  //this only
        tsFunc.localsSize = 1 * VALUE_SIZE;
        tsFunc.returnTypeKind = RTK_String;
        tsFunc.intrinsicId = INTR_Object_toString;
        m_compiledModule.functions.push_back(std::move(tsFunc));
        cc.methodIndices.push_back(toStringFuncIdx);

        m_compiledModule.classes.push_back(std::move(cc));
        m_objectClassIdx = static_cast<int16_t>(classIdx);
    }

    auto registerBuiltin = [&](const std::string& name,
        const std::vector<std::pair<std::string, uint16_t>>& methods,
        uint16_t ctorIntrinsicId, bool hasCtorParams) {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        CompiledClass cc;
        cc.name = name;
        cc.fieldCount = 1;  //hidden __handle field
        cc.fieldNames.push_back("__handle");
        cc.fieldTypeKinds.push_back(RTK_Int32);
        cc.fieldStructIndices.push_back(0xFFFF);
        cc.fieldClassIndices.push_back(0xFFFF);
        cc.fieldAccess.push_back(0);  //private

        //Ctor stub.
        auto ctorFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction ctorFunc;
        ctorFunc.name = name;  //ctor name matches class name
        ctorFunc.paramCount = hasCtorParams ? 3 : 1;  //this + [path, mode]
        ctorFunc.localsSize = ctorFunc.paramCount * VALUE_SIZE;
        ctorFunc.returnTypeKind = RTK_Void;
        ctorFunc.intrinsicId = ctorIntrinsicId;
        m_compiledModule.functions.push_back(std::move(ctorFunc));
        cc.constructorIdx = ctorFuncIdx;

        //Method stubs.
        for (auto& [methName, intrinsicId] : methods) {
            auto methFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
            CompiledFunction methFunc;
            methFunc.name = methName;
            //paramCount: 1 (this) for most, 2 (this + arg) for Write*/Read*/Equals
            bool hasArg = (methName.find("Write") == 0
                || methName.find("Read") == 0
                || methName == "equals");
            methFunc.paramCount = static_cast<uint16_t>(hasArg ? 2 : 1);
            methFunc.localsSize = static_cast<uint16_t>(methFunc.paramCount * VALUE_SIZE);
            //Return type: int for ReadInt/Length/Position/GetHashCode/Equals,
            //float for ReadFloat, string for ReadString, void for Write*/Reset/Close
            if (methName == "readInt" || methName == "length" || methName == "position"
                || methName == "readStruct" || methName == "readObject"
                || methName == "getHashCode" || methName == "equals")
                methFunc.returnTypeKind = RTK_Int32;
            else if (methName == "readFloat")
                methFunc.returnTypeKind = RTK_Float;
            else if (methName == "readString")
                methFunc.returnTypeKind = RTK_String;
            else
                methFunc.returnTypeKind = RTK_Void;  //Write*/Reset/Close
            methFunc.intrinsicId = intrinsicId;
            m_compiledModule.functions.push_back(std::move(methFunc));
            cc.methodIndices.push_back(methFuncIdx);
        }

        m_compiledModule.classes.push_back(std::move(cc));
    };

    //ByteStream methods.
    registerBuiltin("ByteStream", {
        {"writeInt",   INTR_BS_WriteInt},
        {"readInt",    INTR_BS_ReadInt},
        {"writeFloat", INTR_BS_WriteFloat},
        {"readFloat",  INTR_BS_ReadFloat},
        {"writeString",INTR_BS_WriteString},
        {"readString", INTR_BS_ReadString},
        {"writeStruct",INTR_BS_WriteStruct},
        {"readStruct", INTR_BS_ReadStruct},
        {"writeObject",INTR_BS_WriteObject},
        {"readObject", INTR_BS_ReadObject},
        {"length",     INTR_BS_Length},
        {"position",   INTR_BS_Position},
        {"reset",      INTR_BS_Reset},
        {"close",      INTR_BS_Close},
    }, INTR_BS_Ctor, false);

    //FileStream methods.
    registerBuiltin("FileStream", {
        {"writeInt",   INTR_FS_WriteInt},
        {"readInt",    INTR_FS_ReadInt},
        {"writeFloat", INTR_FS_WriteFloat},
        {"readFloat",  INTR_FS_ReadFloat},
        {"writeString",INTR_FS_WriteString},
        {"readString", INTR_FS_ReadString},
        {"writeStruct",INTR_FS_WriteStruct},
        {"readStruct", INTR_FS_ReadStruct},
        {"writeObject",INTR_FS_WriteObject},
        {"readObject", INTR_FS_ReadObject},
        {"length",     INTR_FS_Length},
        {"position",   INTR_FS_Position},
        {"close",      INTR_FS_Close},
    }, INTR_FS_Ctor, true);

    //Phase 8e-3: List<T> — built-in generic, erasure-style. All instantiations
    //(List<int>, List<Point>, ...) share this single CompiledClass. Per-T
    //boxing is decided at call sites by VmBackend based on the outer's
    //generic-type-args (stored on the synthetic SnClassDecl).
    {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        CompiledClass cc;
        cc.name = "List";
        cc.superClassIdx = -1;  //resolved to Object below
        cc.fieldCount = 1;  //hidden __handle field (index into m_listStore)
        cc.fieldNames.push_back("__handle");
        cc.fieldTypeKinds.push_back(RTK_Int32);
        cc.fieldStructIndices.push_back(0xFFFF);
        cc.fieldClassIndices.push_back(0xFFFF);
        cc.fieldAccess.push_back(0);  //private

        //Ctor stub: List(this) — no args beyond this.
        auto ctorFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction ctorFunc;
        ctorFunc.name = "List";
        ctorFunc.paramCount = 1;
        ctorFunc.localsSize = 1 * VALUE_SIZE;
        ctorFunc.returnTypeKind = RTK_Void;
        ctorFunc.intrinsicId = INTR_List_Ctor;
        m_compiledModule.functions.push_back(std::move(ctorFunc));
        cc.constructorIdx = ctorFuncIdx;

        //Method stubs. paramCount includes `this`:
        // - Length/Clear: 1 (this)
        // - Add/Get/RemoveAt/IndexOf/Contains: 2 (this + arg)
        // - Set: 3 (this + idx + value)
        auto addMethod = [&](const char* methName, uint16_t intrinsicId,
            uint16_t paramCount, uint16_t returnTypeKind) {
            auto methFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
            CompiledFunction methFunc;
            methFunc.name = methName;
            methFunc.paramCount = paramCount;
            methFunc.localsSize = static_cast<uint16_t>(paramCount * VALUE_SIZE);
            methFunc.returnTypeKind = returnTypeKind;
            methFunc.intrinsicId = intrinsicId;
            m_compiledModule.functions.push_back(std::move(methFunc));
            cc.methodIndices.push_back(methFuncIdx);
        };
        addMethod("add",      INTR_List_Add,      2, RTK_Void);
        addMethod("get",      INTR_List_Get,      2, RTK_Int32);   //return T, codegen unboxes
        addMethod("set",      INTR_List_Set,      3, RTK_Void);
        addMethod("length",   INTR_List_Length,   1, RTK_Int32);
        addMethod("removeAt", INTR_List_RemoveAt, 2, RTK_Void);
        addMethod("indexOf",  INTR_List_IndexOf,  2, RTK_Int32);
        addMethod("contains", INTR_List_Contains, 2, RTK_Int32);
        addMethod("clear",    INTR_List_Clear,    1, RTK_Void);
        //Phase 9b-pre: List.toString() — formats elements as "[a, b, c]".
        addMethod("toString", INTR_List_toString, 1, RTK_String);

        m_compiledModule.classes.push_back(std::move(cc));
        m_listClassIdx = static_cast<int16_t>(classIdx);
    }

    //Phase 8e-4: Dict<K,V> — built-in generic, erasure-style. All instantiations
    //(Dict<int,int>, Dict<string,Point>, ...) share this single CompiledClass.
    //Side storage is m_dictStore (vector<DictSlot>) indexed by __handle-1.
    //Linear-scan lookup; kind-aware equality via DictKeysEqual at runtime.
    {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        CompiledClass cc;
        cc.name = "Dict";
        cc.superClassIdx = -1;  //resolved to Object below
        cc.fieldCount = 1;  //hidden __handle field (index into m_dictStore)
        cc.fieldNames.push_back("__handle");
        cc.fieldTypeKinds.push_back(RTK_Int32);
        cc.fieldStructIndices.push_back(0xFFFF);
        cc.fieldClassIndices.push_back(0xFFFF);
        cc.fieldAccess.push_back(0);  //private

        //Ctor stub: Dict(this) — no args beyond this.
        auto ctorFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction ctorFunc;
        ctorFunc.name = "Dict";
        ctorFunc.paramCount = 1;
        ctorFunc.localsSize = 1 * VALUE_SIZE;
        ctorFunc.returnTypeKind = RTK_Void;
        ctorFunc.intrinsicId = INTR_Dict_Ctor;
        m_compiledModule.functions.push_back(std::move(ctorFunc));
        cc.constructorIdx = ctorFuncIdx;

        //Method stubs. paramCount includes `this`:
        // - Clear/Count: 1 (this)
        // - Get/ContainsKey/Remove: 2 (this + K)
        // - Set: 3 (this + K + V)
        auto addMethod = [&](const char* methName, uint16_t intrinsicId,
            uint16_t paramCount, uint16_t returnTypeKind) {
            auto methFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
            CompiledFunction methFunc;
            methFunc.name = methName;
            methFunc.paramCount = paramCount;
            methFunc.localsSize = static_cast<uint16_t>(paramCount * VALUE_SIZE);
            methFunc.returnTypeKind = returnTypeKind;
            methFunc.intrinsicId = intrinsicId;
            m_compiledModule.functions.push_back(std::move(methFunc));
            cc.methodIndices.push_back(methFuncIdx);
        };
        addMethod("set",         INTR_Dict_Set,         3, RTK_Void);
        addMethod("get",         INTR_Dict_Get,         2, RTK_Int32);  //return V, codegen unboxes
        addMethod("containsKey", INTR_Dict_ContainsKey, 2, RTK_Int32);
        addMethod("remove",      INTR_Dict_Remove,      2, RTK_Int32);
        addMethod("clear",       INTR_Dict_Clear,       1, RTK_Void);
        addMethod("count",       INTR_Dict_Count,       1, RTK_Int32);
        //Phase 8e-5: Dict.Keys() returns a fresh List<K> heap instance
        //(allocated by the VM intrinsic). paramCount=1 (just this). Return
        //is RTK_Class (heap reference to List<K>) — no boxing on return.
        addMethod("keys",        INTR_Dict_Keys,        1, RTK_Class);
        //Phase 9b-pre: Dict.toString() — formats entries as "{k: v, ...}".
        addMethod("toString",    INTR_Dict_toString,    1, RTK_String);

        m_compiledModule.classes.push_back(std::move(cc));
        m_dictClassIdx = static_cast<int16_t>(classIdx);
    }

    //Phase 9d: Exception class hierarchy (Exception + 4 built-in subclasses).
    //Registered AFTER List/Dict so that backtrace field's fieldClassIndices can
    //directly reference m_listClassIdx (no post-patch needed).
    //Layout (slot numbering from 1; slot[0] is classIdx header):
    //  slot[1] = message (RTK_String, string-object handle)
    //  slot[2] = backtrace (RTK_Class, heap idx to List<string>)
    //Subclasses "flatten" the inherited fields into their own fieldNames /
    //fieldTypeKinds / fieldClassIndices arrays — this matches how user class
    //registration handles inheritance (RegisterClasses walks ancestor chain
    //and copies fields down). AllocClassOnHeap uses cc.fieldCount to size
    //the heap slot, so flattened fields are required for subclass instances
    //to have room for the inherited message/backtrace slots.
    //All 5 ctors share INTR_Exception_Ctor dispatch — ExecuteIntrinsic has
    //one case handling all 5 IDs (subclass ctor IDs collapse to it).
    auto pushExceptionFields = [&](CompiledClass& cc) {
        cc.fieldCount = 2;
        cc.fieldNames.push_back("message");
        cc.fieldTypeKinds.push_back(RTK_String);
        cc.fieldStructIndices.push_back(0xFFFF);
        cc.fieldClassIndices.push_back(0xFFFF);
        cc.fieldAccess.push_back(0);  //accessible via field ref
        cc.fieldNames.push_back("backtrace");
        cc.fieldTypeKinds.push_back(RTK_Class);
        cc.fieldStructIndices.push_back(0xFFFF);
        cc.fieldClassIndices.push_back(static_cast<uint16_t>(m_listClassIdx));
        cc.fieldAccess.push_back(0);
    };
    auto registerExceptionClass = [&](const char* name,
        int16_t superClassIdx, uint16_t ctorIntrinsicId, int16_t* outIdx) {
        auto classIdx = static_cast<uint16_t>(m_compiledModule.classes.size());
        CompiledClass cc;
        cc.name = name;
        cc.superClassIdx = superClassIdx;
        //Both base and subclasses carry the 2 fields (flattened inheritance).
        pushExceptionFields(cc);
        cc.constructorIdx = 0xFFFF;
        //Ctor stub function: (this, message) — paramCount=2.
        auto ctorFuncIdx = static_cast<uint16_t>(m_compiledModule.functions.size());
        CompiledFunction ctorFunc;
        ctorFunc.name = name;  //ctor name matches class name
        ctorFunc.paramCount = 2;
        ctorFunc.localsSize = static_cast<uint16_t>(2 * VALUE_SIZE);
        ctorFunc.returnTypeKind = RTK_Void;
        ctorFunc.intrinsicId = ctorIntrinsicId;
        m_compiledModule.functions.push_back(std::move(ctorFunc));
        cc.constructorIdx = ctorFuncIdx;

        m_compiledModule.classes.push_back(std::move(cc));
        *outIdx = static_cast<int16_t>(classIdx);
    };
    registerExceptionClass("Exception", -1 /*resolved to Object below*/,
        INTR_Exception_Ctor, &m_exceptionClassIdx);
    registerExceptionClass("NullPointerException", m_exceptionClassIdx,
        INTR_NullPointerException_Ctor, &m_nullPtrExcClassIdx);
    registerExceptionClass("DivByZeroException", m_exceptionClassIdx,
        INTR_DivByZeroException_Ctor, &m_divZeroExcClassIdx);
    registerExceptionClass("IndexOutOfBoundsException", m_exceptionClassIdx,
        INTR_IndexOutOfBoundsException_Ctor, &m_oobExcClassIdx);
    registerExceptionClass("AssertionException", m_exceptionClassIdx,
        INTR_AssertionException_Ctor, &m_assertExcClassIdx);
    registerExceptionClass("IOException", m_exceptionClassIdx,
        INTR_IOException_Ctor, &m_ioExcClassIdx);
    //Patch Exception's superClassIdx to Object (set in the common Object-resolve
    //loop below; here we just leave -1 which gets resolved next).
}

void VmBackend::GenerateAllBytecode(SnNamespace& root) {
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            //Phase 9f: native declarations are body-less by contract but
            //still need a generated (minimal) function record.
            if (!func.Body() && !func.ContainFlags(NF_Native))
                continue;
            auto it = m_funcIndexMap.find(&func);
            if (it != m_funcIndexMap.end())
                GenerateFunction(func, it->second);
        } else if (member.Kind() == NK_EnumDecl) {
            //Phase 12: generate enum method bodies (separate kind-
            //filtered list; SnEnumDecl is not a SnFunctionParentField).
            for (auto& method : static_cast<SnEnumDecl&>(member).Methods()) {
                if (!method.Body() && !method.ContainFlags(NF_Native))
                    continue;
                auto it = m_funcIndexMap.find(&method);
                if (it != m_funcIndexMap.end())
                    GenerateFunction(method, it->second);
            }
        } else if (CanBeFuncParentEx(member.Kind())) {
            //Phase 9d-2: super(...) emission needs the enclosing class.
            SnClassDecl* prevClass = m_pCurrClass;
            if (member.Kind() == NK_ClassDecl)
                m_pCurrClass = static_cast<SnClassDecl*>(&member);
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_Function) {
                    auto& func = static_cast<SnFunction&>(child);
                    if (!func.Body() && !func.ContainFlags(NF_Native))
                        continue;
                    auto it = m_funcIndexMap.find(&func);
                    if (it != m_funcIndexMap.end())
                        GenerateFunction(func, it->second);
                }
            }
            m_pCurrClass = prevClass;
        }
    }
}

//Returns field offset in bytes, or -1 if not found.
void VmBackend::GenerateFunction(SnFunction& func, size_t funcIdx) {
    CompiledFunction& compiledFunc = m_compiledModule.functions[funcIdx];

    //v1.12 type descriptors: capture the true formal/return types for
    //cross-module stub reconstruction. Runs before the native branch so
    //body-less native declarations serialize their signature too. Methods
    //contribute one descriptor per AST formal — the implicit this slot
    //has none, mirroring defaultValues' sizing (stub consumers never see
    //method records; the count field keeps the wire self-describing).
    for (auto& param : func.Params()) {
        ParamTypeDesc ptd;
        if (param.ContainFlags(NF_Out))
            ptd.flags |= PTDF_Out;
        ptd.type = BuildTypeDesc(param.EvalDataType(), m_compiledModule);
        compiledFunc.paramTypeDescs.push_back(std::move(ptd));
    }
    if (func.HasReturn() && func.ReturnType())
        compiledFunc.returnTypeDesc = BuildTypeDesc(
            func.ReturnType()->Field(), m_compiledModule);

    //Phase 9f: native function declaration (`native int f(...);`). No
    //bytecode — the VM dispatches by name through the host-registered
    //native table (VmExecutor::RegisterNative). The record carries only
    //the signature: the native reads args directly from the caller's
    //callParamBase cells and writes the return into pResult.
    if (func.ContainFlags(NF_Native)) {
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
        return;
    }

    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;

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

    // Generate bytecode for body
    BytecodeEmitter emitter;
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

    compiledFunc.bytecode = emitter.TakeBytes();
    compiledFunc.localsSize = ctx.nextOffset;

    //Walker-drift tripwire: the frame reserved stats.peakDepth slots for
    //the evalArea, and every claim during emission must have fit inside.
    //observedPeakCursor > reserved means the walker under-predicted some
    //codegen claim — the emitted bytecode would stack-walk past the frame
    //at runtime. Fail the build here instead (Phase 10 audit round-8
    //closing move: drift is a compiler error, never a silent OOB).
    //Scope note: this guards the evalArea only. callParamBase (sized from
    //the MaxArgsWalker, no observed counterpart) can still under-count
    //silently — keep MaxArgsWalker symmetric with every call-emitting path.
    if (ctx.observedPeakCursor > stats.peakDepth * VALUE_SIZE) {
        throw std::runtime_error(
            "VmBackend: evalArea walker drift in function '"
            + func.Name() + "': claims need "
            + std::to_string(ctx.observedPeakCursor / VALUE_SIZE)
            + " slots but frame reserved "
            + std::to_string(stats.peakDepth));
    }

    m_currFunc = nullptr;
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
    std::string sFilePath;
    namespace bf = std::filesystem;
    const BuildParams& setting = env.Params();
    const std::string sFileName = setting.m_sOutputModule + ".nmod";

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
    //single .nmod writer, shared with unit tests so hand-written byte
    //layouts cannot drift from the reader (ModuleLoader).
    if (!WriteCompiledModule(fs, m_compiledModule)) {
        env.Log(CLL_Fatal, "Failed to write module: %s.", sFilePath.c_str());
        return false;
    }
    return true;
}

} // namespace nlang
