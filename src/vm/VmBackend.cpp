#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
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

VmBackend::VmBackend() = default;
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
    //  slot[1] = message (RTK_String, string pool idx)
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
    //Patch Exception's superClassIdx to Object (set in the common Object-resolve
    //loop below; here we just leave -1 which gets resolved next).
}

void VmBackend::RegisterStructs(SnNamespace& root) {
    auto registerStruct = [&](SnStructDecl& sn) {
        CompiledStruct cs;
        cs.name = sn.Name();
        cs.fieldCount = static_cast<uint16_t>(sn.FieldCount());
        std::vector<std::string> typeNames;
        for (auto& field : sn.Members()) {
            cs.fieldNames.push_back(field.Name());
            auto* fieldType = field.EvalDataType();
            uint16_t ftk = RuntimeTypeKind(fieldType);
            cs.fieldTypeKinds.push_back(ftk);
            cs.fieldStructIndices.push_back(0xFFFF);
            cs.fieldClassIndices.push_back(0xFFFF);
            if ((ftk == RTK_Struct || ftk == RTK_Class) && fieldType)
                typeNames.push_back(fieldType->Name());
            else
                typeNames.push_back("");
        }
        m_compiledModule.structs.push_back(std::move(cs));
        m_structFieldTypeNames.push_back(std::move(typeNames));
    };
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_StructDecl) {
            if (member.IsImported()) continue;  //Phase 9c R3-F: skip stubs
            registerStruct(static_cast<SnStructDecl&>(member));
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_StructDecl) {
                    if (child.IsImported()) continue;  //Phase 9c R3-F
                    registerStruct(static_cast<SnStructDecl&>(child));
                }
            }
        }
    }
    //Resolve fieldStructIndices now that all structs are registered.
    //fieldClassIndices are resolved later by ResolveStructClassRefs
    //(after RegisterClasses, since classes are not yet registered here).
    for (size_t si = 0; si < m_compiledModule.structs.size(); ++si) {
        auto& cs = m_compiledModule.structs[si];
        auto& typeNames = m_structFieldTypeNames[si];
        for (size_t i = 0; i < typeNames.size(); ++i) {
            if (!typeNames[i].empty() && cs.fieldTypeKinds[i] == RTK_Struct) {
                int idx = m_compiledModule.FindStruct(typeNames[i]);
                if (idx >= 0)
                    cs.fieldStructIndices[i] = static_cast<uint16_t>(idx);
            }
        }
    }
}

void VmBackend::RegisterClasses(SnNamespace& root) {
    //Map from class name to SnClassDecl* for post-registration resolution.
    std::unordered_map<std::string, SnClassDecl*> declMap;
    auto registerClass = [&](SnClassDecl& sn) {
        CompiledClass cc;
        cc.name = sn.Name();
        cc.superClassIdx = -1;
        //Collect inherited fields: walk from root ancestor to direct parent,
        //collecting each ancestor's own fields (not their inherited fields).
        std::vector<SnClassDecl*> ancestors;
        auto* pSuper = sn.SuperClass();
        while (pSuper) {
            ancestors.push_back(pSuper);
            pSuper = pSuper->SuperClass();
        }
        //Add fields from root ancestor first (reversed order)
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
            //Phase 9d: a built-in Exception ancestor's synthetic decl has
            //no Members(), but its runtime layout carries 2 fields
            //(message=slot[1], backtrace=slot[2]). Inject them so user
            //subclasses of Exception get the correct flattened layout
            //(matching FindClassFieldOffset and the ctor intrinsic).
            if ((*it)->IsBuiltinClass()) {
                const auto& an = (*it)->Name();
                if (an == "Exception" || an == "NullPointerException"
                    || an == "DivByZeroException"
                    || an == "IndexOutOfBoundsException"
                    || an == "AssertionException") {
                    cc.fieldNames.push_back("message");
                    cc.fieldTypeKinds.push_back(RTK_String);
                    cc.fieldStructIndices.push_back(0xFFFF);
                    cc.fieldClassIndices.push_back(0xFFFF);
                    cc.fieldAccess.push_back(0);
                    cc.fieldNames.push_back("backtrace");
                    cc.fieldTypeKinds.push_back(RTK_Class);
                    cc.fieldStructIndices.push_back(0xFFFF);
                    cc.fieldClassIndices.push_back(
                        static_cast<uint16_t>(m_listClassIdx));
                    cc.fieldAccess.push_back(0);
                    continue;
                }
            }
            for (auto& member : (*it)->Members()) {
                if (member.Kind() == NK_ClassField) {
                    auto& cf = static_cast<SnClassField&>(member);
                    cc.fieldNames.push_back(cf.Name());
                    auto* fieldType = cf.EvalDataType();
                    uint16_t ftk = fieldType ? RuntimeTypeKind(fieldType) : RTK_Int32;
                    cc.fieldTypeKinds.push_back(ftk);
                    cc.fieldStructIndices.push_back(0xFFFF);
                    cc.fieldClassIndices.push_back(0xFFFF);
                    cc.fieldAccess.push_back(static_cast<uint8_t>(cf.Access()));
                }
            }
        }
        //Collect own fields
        for (auto& member : sn.Members()) {
            if (member.Kind() == NK_ClassField) {
                auto& cf = static_cast<SnClassField&>(member);
                cc.fieldNames.push_back(cf.Name());
                auto* fieldType = cf.EvalDataType();
                uint16_t ftk = fieldType ? RuntimeTypeKind(fieldType) : RTK_Int32;
                cc.fieldTypeKinds.push_back(ftk);
                cc.fieldStructIndices.push_back(0xFFFF);
                cc.fieldClassIndices.push_back(0xFFFF);
                cc.fieldAccess.push_back(static_cast<uint8_t>(cf.Access()));
            }
        }
        cc.fieldCount = static_cast<uint16_t>(cc.fieldNames.size());
        declMap[sn.Name()] = &sn;
        m_compiledModule.classes.push_back(std::move(cc));
    };
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_ClassDecl) {
            if (member.IsImported()) continue;  //Phase 9c R3-F: skip stubs
            registerClass(static_cast<SnClassDecl&>(member));
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_ClassDecl) {
                    if (child.IsImported()) continue;  //Phase 9c R3-F
                    registerClass(static_cast<SnClassDecl&>(child));
                }
            }
        }
    }
    //Resolve superClassIdx and fieldClassIndices (requires all classes registered).
    for (auto& cc : m_compiledModule.classes) {
        auto it = declMap.find(cc.name);
        if (it == declMap.end()) continue;
        auto* pDecl = it->second;
        if (pDecl->SuperClass()) {
            int idx = m_compiledModule.FindClass(pDecl->SuperClass()->Name());
            cc.superClassIdx = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
        }
        for (size_t i = 0; i < cc.fieldNames.size(); ++i) {
            auto* pField = pDecl->FindField(cc.fieldNames[i]);
            if (pField && pField->Kind() == NK_ClassField) {
                auto* ft = pField->EvalDataType();
                if (ft) {
                    if (cc.fieldTypeKinds[i] == RTK_Class) {
                        int idx = m_compiledModule.FindClass(ft->Name());
                        if (idx >= 0)
                            cc.fieldClassIndices[i] = static_cast<uint16_t>(idx);
                    } else if (cc.fieldTypeKinds[i] == RTK_Struct) {
                        int idx = m_compiledModule.FindStruct(ft->Name());
                        if (idx >= 0)
                            cc.fieldStructIndices[i] = static_cast<uint16_t>(idx);
                    }
                }
            }
        }
    }

    //Phase 8e-1: implicit Object inheritance. Every user class with no explicit
    //parent inherits from Object. The only class that keeps superClassIdx==-1
    //is Object itself (already registered in RegisterBuiltinClasses).
    //ByteStream/FileStream (built-in) get Object as their parent too.
    if (m_objectClassIdx >= 0) {
        for (auto& cc : m_compiledModule.classes) {
            if (cc.superClassIdx == -1 && cc.name != "Object") {
                cc.superClassIdx = m_objectClassIdx;
            }
        }
    }
}

void VmBackend::ResolveStructClassRefs() {
    for (size_t si = 0; si < m_compiledModule.structs.size(); ++si) {
        auto& cs = m_compiledModule.structs[si];
        auto& typeNames = m_structFieldTypeNames[si];
        for (size_t i = 0; i < typeNames.size(); ++i) {
            if (!typeNames[i].empty() && cs.fieldTypeKinds[i] == RTK_Class) {
                int idx = m_compiledModule.FindClass(typeNames[i]);
                if (idx >= 0)
                    cs.fieldClassIndices[i] = static_cast<uint16_t>(idx);
            }
        }
    }
}

uint16_t VmBackend::RegisterArrayType(SnField* pElemType) {
    uint8_t elemKind = RuntimeTypeKind(pElemType);
    uint16_t elemTypeIdx = 0xFFFF;
    if (elemKind == RTK_Struct && pElemType) {
        int idx = m_compiledModule.FindStruct(pElemType->Name());
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    } else if (elemKind == RTK_Class && pElemType) {
        int idx = m_compiledModule.FindClass(pElemType->Name());
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    }
    int existing = m_compiledModule.FindArray(elemKind, elemTypeIdx);
    if (existing >= 0)
        return static_cast<uint16_t>(existing);
    CompiledArrayType at;
    at.elemKind = elemKind;
    at.elemTypeIdx = elemTypeIdx;
    m_compiledModule.arrayTypes.push_back(at);
    return static_cast<uint16_t>(m_compiledModule.arrayTypes.size() - 1);
}

//Recursively walk the AST collecting array type usages.
//Array types appear in: LocalDeclStmt.Type() when IsArrayType(), NewArrayExpr,
//struct/class fields, function params, return types. For Phase 4 simplicity,
//we scan LocalDeclStmt, struct fields, class fields, formal params, and
//NewArrayExpr. The latter is registered lazily at codegen time.
void VmBackend::RegisterArrayTypes(SnNamespace& root) {
    auto processType = [&](SnFieldExpr* pTypeExpr) {
        if (pTypeExpr && pTypeExpr->IsArrayType()) {
            //Walk through nested SnArrayTypeExpr nodes to find the element type.
            auto* pCur = pTypeExpr;
            while (pCur->Kind() == NK_ArrayTypeExpr) {
                pCur = static_cast<SnArrayTypeExpr*>(pCur)->ElementType();
            }
            //pCur is now the base type (SnNameExpr for primitives/classes).
            if (auto* pNameExpr = dynamic_cast<SnNameExpr*>(pCur)) {
                if (pNameExpr->Field())
                    RegisterArrayType(pNameExpr->Field());
            }
        }
    };
    std::function<void(SnField&)> walkField = [&](SnField& f) {
        if (f.Kind() == NK_StructField) {
            auto& sf = static_cast<SnStructField&>(f);
            if (sf.Type())
                processType(sf.Type());
        }
        if (f.Kind() == NK_ClassField) {
            auto& cf = static_cast<SnClassField&>(f);
            if (cf.Type())
                processType(cf.Type());
        }
        if (f.Kind() == NK_FormalParam) {
            auto& fp = static_cast<SnFormalParam&>(f);
            if (fp.Type())
                processType(fp.Type());
        }
    };
    std::function<void(SnStatement&)> walkStmt = [&](SnStatement& s) {
        if (s.Kind() == NK_Paragraph) {
            for (auto& child : static_cast<SnParagraph&>(s).Statements())
                walkStmt(child);
        } else if (s.Kind() == NK_LocalDeclStmt) {
            auto& decl = static_cast<SnLocalDeclStmt&>(s);
            if (decl.Type() && decl.Type()->IsArrayType())
                processType(decl.Type());
        } else if (s.Kind() == NK_IfStmt) {
            auto& ifStmt = static_cast<SnIfStmt&>(s);
            if (ifStmt.ThenStmt()) walkStmt(*ifStmt.ThenStmt());
            if (ifStmt.ElseStmt()) walkStmt(*ifStmt.ElseStmt());
        } else if (s.Kind() == NK_WhileStmt) {
            walkStmt(*static_cast<SnWhileStmt&>(s).Body());
        } else if (s.Kind() == NK_DoStmt) {
            walkStmt(*static_cast<SnDoStmt&>(s).Body());
        } else if (s.Kind() == NK_ForStmt) {
            auto& forStmt = static_cast<SnForStmt&>(s);
            if (forStmt.Init()) walkStmt(*forStmt.Init());
            if (forStmt.Body()) walkStmt(*forStmt.Body());
        }
    };
    std::function<void(SyntaxNode&)> walkNode = [&](SyntaxNode& n) {
        if (n.IsImported()) return;  //Phase 9c R3-F: skip stub trees
        if (n.Kind() == NK_StructDecl) {
            for (auto& member : static_cast<SnStructDecl&>(n).Members())
                walkField(member);
        } else if (n.Kind() == NK_ClassDecl) {
            for (auto& member : static_cast<SnClassDecl&>(n).Members())
                walkField(member);
        } else if (n.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(n);
            for (auto& param : func.Params())
                walkField(param);
            if (func.Body())
                walkStmt(*func.Body());
        }
    };
    for (auto& member : root.Members()) {
        walkNode(member);
        if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members())
                walkNode(child);
        }
    }
}

//Phase 8e-9b: collect every enum declaration's value-name table into
//m_compiledModule.enumNames, and remember each SnEnumDecl*'s defIdx for
//later codegen (SnCastExpr src-type lookup). Enum declarations may live
//at namespace top level or nested inside class/struct scopes (handled by
//CanBeFuncParentEx, same pattern as RegisterStructs/RegisterClasses).
void VmBackend::RegisterEnums(SnNamespace& root) {
    m_enumIndexMap.clear();
    m_compiledModule.enumNames.clear();
    auto registerEnum = [&](SnEnumDecl& sn) {
        auto defIdx = m_compiledModule.enumNames.size();
        std::vector<std::string> names;
        for (auto& member : sn.Members()) {
            if (member.Kind() == NK_EnumMember) {
                auto& em = static_cast<SnEnumMember&>(member);
                //Enum values are sequential starting at 0; if user provides
                //explicit values that skip numbers, the corresponding slots
                //are filled with empty strings (OP_Enum_to_str will throw
                //"enum value out of range" at runtime if hit). NLang grammar
                //currently only supports implicit sequential values.
                while (names.size() <= static_cast<size_t>(em.Value()))
                    names.push_back(std::string());
                names[static_cast<size_t>(em.Value())] = em.Name();
            }
        }
        m_compiledModule.enumNames.push_back(std::move(names));
        m_enumIndexMap[&sn] = defIdx;
    };
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_EnumDecl) {
            if (member.IsImported()) continue;  //Phase 9c R3-F: skip stubs
            registerEnum(static_cast<SnEnumDecl&>(member));
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_EnumDecl) {
                    if (child.IsImported()) continue;  //Phase 9c R3-F
                    registerEnum(static_cast<SnEnumDecl&>(child));
                }
            }
        }
    }
}

void VmBackend::RegisterFunctions(SnNamespace& root) {
    m_funcIndexMap.clear();
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            //Phase 9f: native declarations register like normal
            //functions (the record carries isNative + param signature).
            if (!func.Body() && !func.ContainFlags(NF_Native))
                continue;
            CompiledFunction cf;
            cf.name = func.Name();
            m_compiledModule.functions.push_back(std::move(cf));
            m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_Function) {
                    auto& func = static_cast<SnFunction&>(child);
                    if (!func.Body() && !func.ContainFlags(NF_Native))
                        continue;
                    CompiledFunction cf;
                    cf.name = func.Name();
                    m_compiledModule.functions.push_back(std::move(cf));
                    m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
                }
            }
        }
    }
}

void VmBackend::PopulateClassMethods(SnNamespace& root) {
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_ClassDecl) {
            if (member.IsImported()) continue;  //Phase 9c R6-1: stub has no AST methods; merged cc.methodIndices from Phase B must be preserved
            auto& sn = static_cast<SnClassDecl&>(member);
            int ccIdx = m_compiledModule.FindClass(sn.Name());
            if (ccIdx < 0) continue;
            auto& cc = m_compiledModule.classes[static_cast<size_t>(ccIdx)];
            cc.methodIndices.clear();
            cc.constructorIdx = 0xFFFF;
            for (auto& child : sn.Members()) {
                if (child.Kind() == NK_Function) {
                    auto it = m_funcIndexMap.find(static_cast<SnFunction*>(&child));
                    if (it != m_funcIndexMap.end()) {
                        uint16_t funcIdx = static_cast<uint16_t>(it->second);
                        cc.methodIndices.push_back(funcIdx);
                        if (child.Name() == sn.Name())
                            cc.constructorIdx = funcIdx;
                    }
                }
            }
        }
    }
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

//Phase 9c cross-module: returns the total byte size of an instruction
//(opcode + all operands). Used by RemapBytecode to walk a bytecode buffer.
//All operands are uint16 (2 bytes) except OP_Box/OP_Unbox (1-byte tag),
//OP_ConstInt32/OP_ConstFloat/OP_Jump (4 / 4 / 2-byte immediate), and
//OP_JumpIfNot (2 + 2 = 4). The 11 remap-relevant opcodes are tagged in
//the second switch below.
static size_t InstructionStride(OpCode op) {
    switch (op) {
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ConstZero:
        case OpCode::OP_CastIntToFloat:
        case OpCode::OP_CastFloatToInt:
        case OpCode::OP_Int32_to_str:
        case OpCode::OP_Float_to_str:
        case OpCode::OP_Array_to_str:
        case OpCode::OP_ParaEnd:
        case OpCode::OP_Rethrow:
        case OpCode::OP_PopHandler:
            return 1;  // no operands
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:
            return 1 + 1;  // uint8 tag
        case OpCode::OP_Jump:
        case OpCode::OP_Case:
            return 1 + 2;  // int16 / uint16
        case OpCode::OP_ConstInt32:
        case OpCode::OP_ConstFloat:
            return 1 + 4;
        case OpCode::OP_ConstString:
        case OpCode::OP_AssertFail:
        case OpCode::OP_VarLocal:
        case OpCode::OP_Assign:
        case OpCode::OP_Enum_to_str:
        case OpCode::OP_Neg_i32:
        case OpCode::OP_Neg_f32:
        case OpCode::OP_LogicalNot:
        case OpCode::OP_Switch:
        case OpCode::OP_DebugInfo:
        case OpCode::OP_NullCheck:
        case OpCode::OP_CheckCast:
        case OpCode::OP_Throw:
            return 1 + 2;  // one uint16 operand
        case OpCode::OP_JumpIfNot:
        case OpCode::OP_Add_i32:
        case OpCode::OP_Sub_i32:
        case OpCode::OP_Mul_i32:
        case OpCode::OP_Div_i32:
        case OpCode::OP_Mod_i32:
        case OpCode::OP_Add_f32:
        case OpCode::OP_Sub_f32:
        case OpCode::OP_Mul_f32:
        case OpCode::OP_Div_f32:
        case OpCode::OP_Less_i32:
        case OpCode::OP_LessEqual_i32:
        case OpCode::OP_Greater_i32:
        case OpCode::OP_GreaterEqual_i32:
        case OpCode::OP_Equal_i32:
        case OpCode::OP_NotEqual_i32:
        case OpCode::OP_Less_f32:
        case OpCode::OP_LessEqual_f32:
        case OpCode::OP_Greater_f32:
        case OpCode::OP_GreaterEqual_f32:
        case OpCode::OP_Equal_f32:
        case OpCode::OP_NotEqual_f32:
        case OpCode::OP_LogicalAnd:
        case OpCode::OP_LogicalOr:
        case OpCode::OP_Concat_str:
        case OpCode::OP_Eq_str:
        case OpCode::OP_Ne_str:
        case OpCode::OP_StrLen:
        case OpCode::OP_CallFunc:
        case OpCode::OP_CallMethod:
        case OpCode::OP_CallMethodDirect:
        case OpCode::OP_CallIntrinsic:
        case OpCode::OP_New:
        case OpCode::OP_ArrayLength:
            return 1 + 2 + 2;  // two uint16 operands
        case OpCode::OP_CallFuncOut:
        case OpCode::OP_CallMethodDirectOut:
            return 1 + 2 + 2 + 4;  // uint16 + uint16 + uint32 outMask (Phase 9e)
        case OpCode::OP_AllocStruct:
        case OpCode::OP_LoadField:
        case OpCode::OP_StoreField:
        case OpCode::OP_CopyStruct:
        case OpCode::OP_AllocArray:
        case OpCode::OP_LoadElement:
        case OpCode::OP_StoreElement:
            return 1 + 2 + 2 + 2;  // three uint16 operands
        default:
            //Unknown opcode — should never happen. Returning 1 lets the
            //walker make progress (likely produces garbage but doesn't
            //loop forever); the merged module will fail at runtime.
            assert(false && "unknown opcode in RemapBytecode");
            return 1;
    }
}

//Phase 9c cross-module: walk bytecode and patch the 11 cross-module-indexed
//operand kinds (see cross-module-import-infrastructure.md Layer 4 table).
//Other operands (local offsets, jump targets, intrinsic IDs, type tags,
//debug line numbers) are module-local and do not need remapping.
void VmBackend::RemapBytecode(std::vector<uint8_t>& bc, const PerModuleRemap& pm) {
    size_t pos = 0;
    auto patchU16 = [&bc](size_t off, const std::unordered_map<uint32_t, uint32_t>& m) {
        uint16_t oldv = static_cast<uint16_t>(bc[off])
                      | (static_cast<uint16_t>(bc[off + 1]) << 8);
        auto it = m.find(oldv);
        if (it == m.end())
            return;  // not in the remap table — leave as-is (best effort)
        uint16_t newv = static_cast<uint16_t>(it->second);
        bc[off]     = static_cast<uint8_t>(newv & 0xFF);
        bc[off + 1] = static_cast<uint8_t>((newv >> 8) & 0xFF);
    };
    while (pos < bc.size()) {
        OpCode op = static_cast<OpCode>(bc[pos]);
        switch (op) {
            case OpCode::OP_ConstString:
            case OpCode::OP_AssertFail:
            case OpCode::OP_CallMethod:
                patchU16(pos + 1, pm.stringMap);
                break;
            case OpCode::OP_CallFunc:
            case OpCode::OP_CallMethodDirect:
            case OpCode::OP_CallFuncOut:
            case OpCode::OP_CallMethodDirectOut:
                patchU16(pos + 1, pm.functionMap);
                break;
            case OpCode::OP_New:
            case OpCode::OP_CheckCast:
                patchU16(pos + 1, pm.classMap);
                break;
            case OpCode::OP_AllocStruct:
                patchU16(pos + 1, pm.structMap);
                break;
            case OpCode::OP_CopyStruct:
                // dst, src, structIdx — third operand
                patchU16(pos + 5, pm.structMap);
                break;
            case OpCode::OP_AllocArray:
                patchU16(pos + 1, pm.arrayTypeMap);
                break;
            case OpCode::OP_Enum_to_str:
                patchU16(pos + 1, pm.enumMap);
                break;
            default:
                break;
        }
        pos += InstructionStride(op);
    }
}

//Phase 9c cross-module Phase A.
//Push imported strings/classes/structs/arrayTypes into m_compiledModule,
//build per-module remap tables (stringMap/classMap/structMap/arrayTypeMap),
//and apply partial metadata remap (superClassIdx + fieldClassIndices +
//fieldStructIndices for classes; fieldClassIndices + fieldStructIndices
//for structs; elemTypeIdx for arrayTypes). methodIndices/constructorIdx
//are deferred to Phase B (needs functionMap).
void VmBackend::MergeImportedClassesStructsArrays() {
    m_importRemaps.clear();
    m_importRemaps.reserve(m_importedModules.size());

    //Stage A.1: per-module build stringMap + classMap/structMap/arrayTypeMap
    //by pushing (deduped) entries into m_compiledModule.
    for (auto& im : m_importedModules) {
        PerModuleRemap pm;

        for (uint32_t i = 0; i < im.stringConstants.size(); ++i)
            pm.stringMap[i] = AddStringConstant(im.stringConstants[i]);

        for (uint32_t i = 0; i < im.classes.size(); ++i) {
            int existing = m_compiledModule.FindClass(im.classes[i].name);
            if (existing >= 0) {
                pm.classMap[i] = static_cast<uint32_t>(existing);
                continue;  // dedup to existing (e.g. user Object / built-in)
            }
            pm.classMap[i] = m_compiledModule.classes.size();
            m_compiledModule.classes.push_back(im.classes[i]);
            pm.classWasPushed.insert(i);
        }

        for (uint32_t i = 0; i < im.structs.size(); ++i) {
            int existing = m_compiledModule.FindStruct(im.structs[i].name);
            if (existing >= 0) {
                pm.structMap[i] = static_cast<uint32_t>(existing);
                continue;
            }
            pm.structMap[i] = m_compiledModule.structs.size();
            m_compiledModule.structs.push_back(im.structs[i]);
            //Push empty typeNames to keep m_structFieldTypeNames parallel
            //with m_compiledModule.structs. ResolveStructClassRefs' inner
            //loop iterates typeNames[i].size() so empty → no-op.
            m_structFieldTypeNames.push_back({});
            pm.structWasPushed.insert(i);
        }

        for (uint32_t i = 0; i < im.arrayTypes.size(); ++i) {
            //Push without dedup — RegisterArrayTypes' FindArray will dedup
            //when user code references the same type. Multiple identical
            //entries here are harmless (just slightly wasteful).
            pm.arrayTypeMap[i] = m_compiledModule.arrayTypes.size();
            m_compiledModule.arrayTypes.push_back(im.arrayTypes[i]);
        }

        m_importRemaps.push_back(std::move(pm));
    }

    //Stage A.2: partial metadata remap on pushed entries only (R8-1).
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];

        for (uint32_t i = 0; i < im.classes.size(); ++i) {
            if (pm.classWasPushed.find(i) == pm.classWasPushed.end()) continue;
            uint32_t targetIdx = pm.classMap[i];
            auto& cc = m_compiledModule.classes[targetIdx];
            if (cc.superClassIdx >= 0) {
                auto it = pm.classMap.find(static_cast<uint32_t>(cc.superClassIdx));
                if (it != pm.classMap.end())
                    cc.superClassIdx = static_cast<int16_t>(it->second);
            }
            for (auto& idx : cc.fieldStructIndices)
                if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.structMap.at(idx));
            for (auto& idx : cc.fieldClassIndices)
                if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.classMap.at(idx));
        }

        for (uint32_t i = 0; i < im.structs.size(); ++i) {
            if (pm.structWasPushed.find(i) == pm.structWasPushed.end()) continue;
            uint32_t targetIdx = pm.structMap[i];
            auto& cs = m_compiledModule.structs[targetIdx];
            for (auto& idx : cs.fieldStructIndices)
                if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.structMap.at(idx));
            for (auto& idx : cs.fieldClassIndices)
                if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.classMap.at(idx));
        }

        for (uint32_t i = 0; i < im.arrayTypes.size(); ++i) {
            uint32_t targetIdx = pm.arrayTypeMap[i];
            auto& at = m_compiledModule.arrayTypes[targetIdx];
            if (at.elemTypeIdx == 0xFFFF) continue;
            if (at.elemKind == RTK_Struct)
                at.elemTypeIdx = static_cast<uint16_t>(pm.structMap.at(at.elemTypeIdx));
            else if (at.elemKind == RTK_Class)
                at.elemTypeIdx = static_cast<uint16_t>(pm.classMap.at(at.elemTypeIdx));
        }
    }
}

//Phase 9c cross-module Phase B.
//Push imported enumNames + function placeholders, copy + remap bytecode,
//complete class metadata (methodIndices + constructorIdx), and fill
//m_funcIndexMap[stub] via m_importedFuncSourceIdx side-table.
void VmBackend::MergeImportedFinalize() {
    //Stage B.1: build enumMap + functionMap by pushing placeholders.
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];

        for (uint32_t i = 0; i < im.enumNames.size(); ++i) {
            pm.enumMap[i] = m_compiledModule.enumNames.size();
            m_compiledModule.enumNames.push_back(im.enumNames[i]);
        }

        for (uint32_t i = 0; i < im.functions.size(); ++i) {
            pm.functionMap[i] = m_compiledModule.functions.size();
            CompiledFunction placeholder;
            placeholder.name = im.functions[i].name;
            placeholder.paramCount = im.functions[i].paramCount;
            placeholder.localsSize = im.functions[i].localsSize;
            placeholder.returnTypeKind = im.functions[i].returnTypeKind;
            placeholder.intrinsicId = im.functions[i].intrinsicId;
            //Phase 9f: native flag must survive the merge — the producer
            //wrote no bytecode for a native declaration, so a dropped flag
            //would leave the consumer calling empty bytecode (silent stale
            //pResult instead of a native table lookup).
            placeholder.isNative = im.functions[i].isNative;
            //bytecode filled in stage B.2
            m_compiledModule.functions.push_back(std::move(placeholder));
        }
    }

    //Stage B.2: copy + remap bytecode into each placeholder.
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];
        for (uint32_t i = 0; i < im.functions.size(); ++i) {
            std::vector<uint8_t> bcCopy = im.functions[i].bytecode;
            RemapBytecode(bcCopy, pm);
            //Phase 9d: copy + remap tryBlocks (exceptionClassIdx only —
            //startPc/endPc/handlerPc are byte offsets within the same
            //bytecode buffer, so they don't change across module merge).
            std::vector<TryBlock> tbs = im.functions[i].tryBlocks;
            for (auto& tb : tbs) {
                if (tb.exceptionClassIdx != 0xFFFF) {
                    auto it = pm.classMap.find(tb.exceptionClassIdx);
                    if (it != pm.classMap.end())
                        tb.exceptionClassIdx = static_cast<uint16_t>(it->second);
                }
            }
            uint32_t targetIdx = pm.functionMap[i];
            m_compiledModule.functions[targetIdx].bytecode = std::move(bcCopy);
            m_compiledModule.functions[targetIdx].tryBlocks = std::move(tbs);
        }
    }

    //Stage B.3: complete class metadata remap (methodIndices + constructorIdx).
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];
        for (uint32_t i = 0; i < im.classes.size(); ++i) {
            if (pm.classWasPushed.find(i) == pm.classWasPushed.end()) continue;
            uint32_t targetIdx = pm.classMap[i];
            auto& cc = m_compiledModule.classes[targetIdx];
            for (auto& idx : cc.methodIndices)
                if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.functionMap.at(idx));
            if (cc.constructorIdx != 0xFFFF)
                cc.constructorIdx = static_cast<uint16_t>(pm.functionMap.at(cc.constructorIdx));
        }
    }

    //Stage B.4: fill m_funcIndexMap[stub] for user-codegen lookup (R3-C).
    for (auto& kv : m_importedFuncSourceIdx) {
        SnFunction* stub = kv.first;
        uint32_t srcModIdx = kv.second.first;
        uint32_t srcFuncIdx = kv.second.second;
        if (srcModIdx >= m_importRemaps.size()) continue;
        auto& pm = m_importRemaps[srcModIdx];
        auto it = pm.functionMap.find(srcFuncIdx);
        if (it == pm.functionMap.end()) continue;
        m_funcIndexMap[stub] = it->second;
    }
}


//Enum types are int32 at runtime. Struct types use RTK_Struct.
//Array types are detected via SnField::IsArrayType() (overridden by
//SnArrayTypeExpr to return true), not via the resolved element type.
uint8_t VmBackend::RuntimeTypeKind(SnField* pType) {
    if (!pType) return RTK_Int32;
    if (pType->IsArrayType()) return RTK_Array;
    auto k = pType->Kind();
    if (k == NK_EnumDecl) return RTK_Int32;
    if (k == NK_StructDecl) return RTK_Struct;
    if (k == NK_ClassDecl) return RTK_Class;
    return static_cast<uint8_t>(k);
}

//Option B Step 3: extract a constant-foldable default expression into a
//DefaultValueDesc suitable for serialization. Accepts:
//  - null literal (NF_NullLiteral SnLiteralExpr) → RTK_Null, no payload
//  - SnLiteralExpr with NK_Int32 kind                 → RTK_Int32
//  - SnLiteralExpr with NK_Float kind                 → RTK_Float
//  - SnLiteralExpr with NK_String kind                → RTK_String (pool idx)
//  - SnBinaryExpr(OP_Neg, SnLiteralExpr NK_Int32)     → RTK_Int32 (negative)
//Anything else (identifier ref, function call, cast, member access, etc.)
//returns RTK_Void — caller-side (Step 5 declaration check) rejects this
//for IsImported functions. In-module callers don't consult this vector
//at all, so a RTK_Void entry is harmless for them.
DefaultValueDesc VmBackend::ExtractDefaultValue(SnExpression* pExpr) {
    DefaultValueDesc dv;  // tag defaults to RTK_Void
    if (!pExpr) return dv;  // no default expression

    //Direct literal.
    if (pExpr->Kind() == NK_LiteralExpr) {
        auto* lit = static_cast<SnLiteralExpr*>(pExpr);
        //Null literal: stamped with NF_NullLiteral by KT_Null rule.
        if (lit->ContainFlags(NF_NullLiteral)) {
            dv.tag = RTK_Null;
            return dv;
        }
        //Type-driven literal dispatch. SnLiteralExpr's Variant Type()
        //points at the RnDataType — match pointer identity against the
        //global singletons (RnInt32/RnFloat/RnString).
        auto* litType = lit->Value().Type();
        if (litType == RnInt32::Instance()) {
            dv.tag = RTK_Int32;
            dv.intValue = static_cast<uint32_t>(
                lit->Value().Data().m_Int);
            return dv;
        }
        if (litType == RnFloat::Instance()) {
            dv.tag = RTK_Float;
            dv.floatValue = lit->Value().Data().m_Float;
            return dv;
        }
        if (litType == RnString::Instance()) {
            dv.tag = RTK_String;
            //Intern into producer's pool. Consumer remaps during load.
            auto* sPtr = lit->Value().Data().m_String;
            dv.stringIdx = AddStringConstant(sPtr ? *sPtr : std::string());
            return dv;
        }
        return dv;  //unknown literal type — not foldable
    }

    //Unary negation of numeric literal: `-5` / `-3.14` parse as OP_Neg
    //over a literal. Fold both int and float so negative floats work
    //cross-module too (not just negative ints).
    if (pExpr->Kind() == NK_BinaryExpr) {
        auto* bin = static_cast<SnBinaryExpr*>(pExpr);
        if (bin->Op() == SnBinaryExpr::OP_Neg
            && bin->Left() && bin->Left()->Kind() == NK_LiteralExpr) {
            auto* lit = static_cast<SnLiteralExpr*>(bin->Left());
            if (lit->Value().Type() == RnInt32::Instance()) {
                dv.tag = RTK_Int32;
                int32_t neg = -lit->Value().Data().m_Int;
                dv.intValue = static_cast<uint32_t>(neg);
                return dv;
            }
            if (lit->Value().Type() == RnFloat::Instance()) {
                dv.tag = RTK_Float;
                dv.floatValue = -lit->Value().Data().m_Float;
                return dv;
            }
        }
        return dv;  //other binary exprs not supported in MVP
    }

    return dv;  //non-literal, non-foldable
}

//Phase 8e-4: returns the RTK_* boxing tag for a generic type argument,
//plus an isPrimitive flag. The flag is needed because RTK_Int32 == 0, so
//"is class-T (no boxing)" and "is int-T (box as RTK_Int32)" both yield tag=0.
//Equivalent to the bool needsBoxing + uint8_t tTag pair from the Phase 8e-3
//C1 fix; refactored here so List and Dict can share the helper.
VmBackend::BoxingTagResult VmBackend::BoxingTagFor(SnField* pT) {
    if (!pT) return {0, false};
    NodeKind k = pT->Kind();
    if (k == NK_Int32 || k == NK_EnumDecl) return {RTK_Int32, true};
    if (k == NK_Float)  return {RTK_Float,  true};
    if (k == NK_String) return {RTK_String, true};
    return {0, false};  //class/struct/other T → no boxing
}

//Several opcodes read the pResult accumulator (OP_Box/OP_Unbox,
//OP_CastIntToFloat/OP_CastFloatToInt, OP_Int32_to_str/OP_Float_to_str).
//EmitExpression only leaves the value in pResult when the source's final
//opcode writes the accumulator (var_local, consts, calls); locals-writing
//sources (binary arithmetic, field/element loads) leave it stale — the
//cast_f2i quirk and the `"s" + (a+b)` dedup bug are both this hole.
//Reload pResult from the slot the value is guaranteed to live in before
//emitting any accumulator-reading opcode.
static void EmitPResultRefresh(BytecodeEmitter& emitter, uint16_t slot) {
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(slot);
}

//Pick a temp slot distinct from `exclude`, walking through the 4-slot pool.
//Composition rule: PickTempSlot(tempSlotN) = tempSlot(N+1). This chains
//for nested expressions — each recursive level uses the next slot.
//Slots 0..3 are tempSlot..tempSlot4. If exclude is not a temp slot, return
//tempSlot (the default scratch slot).
uint16_t VmBackend::PickTempSlot(uint16_t exclude) const {
    if (exclude == m_currFunc->tempSlot)  return m_currFunc->tempSlot2;
    if (exclude == m_currFunc->tempSlot2) return m_currFunc->tempSlot3;
    if (exclude == m_currFunc->tempSlot3) return m_currFunc->tempSlot4;
    return m_currFunc->tempSlot;
}

//Phase 9a: emit a compound-assign arithmetic op.
//Executes: locals[dst] = locals[dst] <op> locals[src]
//where op ∈ {Add, Sub, Mul, Div, Mod}. Type determines i32/f32 variant.
void VmBackend::EmitCompoundOp(int opInt,
        BytecodeEmitter& emitter, uint16_t dst, uint16_t src,
        SnField* lhsType) {
    auto op = static_cast<SnBinaryExpr::Operator>(opInt);
    bool isFloat = lhsType && lhsType->Kind() == NK_Float;
    bool isString = lhsType && lhsType->Kind() == NK_String;

    //String only supports += (concat). All other ops are invalid.
    if (isString) {
        if (op == SnBinaryExpr::OP_Add) {
            emitter.Emit(OpCode::OP_Concat_str);
            emitter.EmitUint16(dst);
            emitter.EmitUint16(src);
        }
        //Other ops on string silently ignored (should be caught by resolver).
        return;
    }

    OpCode opc;
    switch (op) {
    case SnBinaryExpr::OP_Add: opc = isFloat ? OpCode::OP_Add_f32 : OpCode::OP_Add_i32; break;
    case SnBinaryExpr::OP_Sub: opc = isFloat ? OpCode::OP_Sub_f32 : OpCode::OP_Sub_i32; break;
    case SnBinaryExpr::OP_Mul: opc = isFloat ? OpCode::OP_Mul_f32 : OpCode::OP_Mul_i32; break;
    case SnBinaryExpr::OP_Div: opc = isFloat ? OpCode::OP_Div_f32 : OpCode::OP_Div_i32; break;
    case SnBinaryExpr::OP_Mod: opc = OpCode::OP_Mod_i32; break;
    default: return;  //not an arithmetic op
    }
    emitter.Emit(opc);
    emitter.EmitUint16(dst);
    emitter.EmitUint16(src);
}

//Returns field offset in bytes, or -1 if not found.
static int FindFieldOffset(SnStructDecl& structDecl, const std::string& fieldName) {
    uint16_t off = 0;
    for (auto& sf : structDecl.Members()) {
        if (sf.Name() == fieldName)
            return off;
        off += VALUE_SIZE;
    }
    return -1;
}

//Returns class field offset in bytes (including +4 for classIdx slot), or -1.
//Object layout: [classIdx, root_ancestor_fields..., parent_fields..., own_fields...]
//Slot[0] = classIdx (4 bytes, used for runtime virtual dispatch via OP_CallMethod).
//Slot[1..N] = data fields (4 bytes each, offset starts at VALUE_SIZE=4).
//Compile-time offset here must match runtime AllocClassOnHeap in VmExecutor.
static int FindClassFieldOffset(SnClassDecl& classDecl, const std::string& fieldName) {
    //Phase 9d: built-in Exception classes expose message/backtrace fields
    //that aren't materialized as SnClassDecl members (the synthetic class
    //decl has empty Members()). Runtime layout (VmBackend::RegisterBuiltinClasses):
    //  slot[1] = message  → offset 4
    //  slot[2] = backtrace → offset 8
    //Walk SuperClass chain so user subclasses of Exception also resolve.
    auto isBuiltinExceptionName = [](const std::string& cn) {
        return cn == "Exception" || cn == "NullPointerException"
            || cn == "DivByZeroException" || cn == "IndexOutOfBoundsException"
            || cn == "AssertionException";
    };
    //Phase 9d: direct built-in Exception class — synthetic decl has no
    //Members(); the 2 runtime fields are fixed at slot[1]/slot[2].
    if (classDecl.IsBuiltinClass()
        && isBuiltinExceptionName(classDecl.Name())) {
        if (fieldName == "message")  return VALUE_SIZE;
        if (fieldName == "backtrace") return 2 * VALUE_SIZE;
        return -1;
    }
    //Collect ancestor chain from root to direct parent
    std::vector<SnClassDecl*> ancestors;
    auto* pSuper = classDecl.SuperClass();
    while (pSuper) {
        ancestors.push_back(pSuper);
        pSuper = pSuper->SuperClass();
    }
    //Search from root ancestor to direct parent (reversed order)
    uint16_t off = VALUE_SIZE; //skip classIdx slot
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        //Phase 9d: a built-in Exception ancestor contributes 2 runtime
        //fields (message, backtrace) that aren't in the synthetic
        //Members() list — RegisterClasses injects them into the compiled
        //field list, so account for them here.
        if ((*it)->IsBuiltinClass()
            && isBuiltinExceptionName((*it)->Name())) {
            if (fieldName == "message")  return off;
            if (fieldName == "backtrace") return off + VALUE_SIZE;
            off += 2 * VALUE_SIZE;
            continue;
        }
        for (auto& member : (*it)->Members()) {
            if (member.Kind() == NK_ClassField && member.Name() == fieldName)
                return off;
            if (member.Kind() == NK_ClassField)
                off += VALUE_SIZE;
        }
    }
    //Search in this class (after all parent fields)
    for (auto& member : classDecl.Members()) {
        if (member.Kind() == NK_ClassField && member.Name() == fieldName)
            return off;
        if (member.Kind() == NK_ClassField)
            off += VALUE_SIZE;
    }
    return -1;
}

//Bare identifier that resolved to a class member (implicit this.field,
//e.g. `v = 1;` inside a method): returns the SnClassDecl that owns the
//field node. For inherited fields the field node belongs to the ancestor
//decl, and FindClassFieldOffset walking from that owner yields the same
//flattened offset as from any subclass. Returns null when pField is not
//a class data member (locals, formals, enum members).
static SnClassDecl* OwningClassOfMemberField(SnField* pField)
{
    if (!pField || pField->Kind() != NK_ClassField)
        return nullptr;
    auto* pParent = pField->Parent();
    if (pParent && pParent->Kind() == NK_ClassDecl)
        return static_cast<SnClassDecl*>(pParent);
    return nullptr;
}

uint16_t VmBackend::AddStringConstant(const std::string& s) {
    auto& pool = m_compiledModule.stringConstants;
    for (uint16_t i = 0; i < static_cast<uint16_t>(pool.size()); ++i) {
        if (pool[i] == s)
            return i;
    }
    assert(pool.size() < UINT16_MAX && "string constant pool overflow");
    pool.push_back(s);
    return static_cast<uint16_t>(pool.size() - 1);
}

//Phase 9c follow-up: compute per-function call slot statistics for
//dynamic frame sizing. Returns {maxArgs, peakDepth} where:
//  maxArgs   = max callee formal count (+1 for method `this`) across all
//              InvokeExpr in the function body. Determines callParamBase size.
//  peakDepth = max simultaneous evalArea slot need across all call sites.
//              Determines evalArea size. Computed as the maximum over all
//              InvokeExpr of: claimSize + max(peakDepth of arg sub-exprs,
//              peakDepth of callee default expressions).
struct CallSlotStats { uint16_t maxArgs; uint16_t peakDepth; };

//True when a subscript's base expression resolves to a List<T>/
//Dict<K,V> generic instantiation — the subscript is sugar over the
//get()/set() intrinsics and must NOT take the OP_LoadElement array
//path. Shared by the read lowering, the subscript-assign lowering,
//the arr[i].field = v member-write receiver path, and the three
//walker sites (ExprPeakDepth/StmtPeakDepth/MaxArgsWalker) so the
//dispatch decision cannot drift between codegen and walkers.
static bool IsContainerSubscript(SnExpression& baseExpr) {
    //Array bases take the OP_LoadElement/OP_StoreElement path even when
    //the element type is a generic instantiation (`List<int>[] a`):
    //EvalDataType returns the ELEMENT type, so the container check must
    //come after the array-ness check (EvalDataType dispatch-order trap,
    //5th instance — mirrors the resolver peel guard in ExprResolver).
    {
        SnIdentifierExpr* pId = nullptr;
        if (baseExpr.Kind() == NK_IdentifierExpr)
            pId = static_cast<SnIdentifierExpr*>(&baseExpr);
        else if (baseExpr.Kind() == NK_MemberExpr) {
            auto* pInner = static_cast<SnMemberExpr&>(baseExpr).Inner();
            if (pInner && pInner->Kind() == NK_IdentifierExpr)
                pId = static_cast<SnIdentifierExpr*>(pInner);
        }
        if (pId && pId->Field() && pId->Field()->IsArrayType())
            return false;
    }
    auto* pBaseType = baseExpr.IsResolved()
        ? baseExpr.EvalDataType() : nullptr;
    if (!pBaseType || pBaseType->Kind() != NK_ClassDecl) return false;
    auto* pGenClass = static_cast<SnClassDecl*>(pBaseType);
    if (!pGenClass->IsGenericInstantiation()) return false;
    const auto& baseName = pGenClass->BaseName();
    const auto& typeArgs = pGenClass->GenericTypeArgs();
    if (baseName == "List") return !typeArgs.empty();
    if (baseName == "Dict") return typeArgs.size() > 1;
    return false;
}

//Forward declarations for the recursive walkers.
static uint16_t ExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited,
    bool isMethodContext = false);
static uint16_t StmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited);

//isMethodContext=true when the InvokeExpr is the Inner() of a MemberExpr
//(i.e. a method call shape `receiver.method(...)`). This is the ONLY
//reliable way to detect method calls — callee->Parent() is null for
//built-in methods (List.add, Dict.set, ToString, etc.), which would
//cause the walker to underreserve evalArea slots and EmitCallArgs
//(called with slotBase=1 from the MemberExpr handler) would overflow
//into user variable space.
static uint16_t ExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited,
    bool isMethodContext) {
    NodeKind kind = expr.Kind();
    if (kind == NK_InvokeExpr) {
        auto& invoke = static_cast<SnInvokeExpr&>(expr);
        auto* callee = invoke.Callee();
        size_t formalCount = callee ? callee->Params().size() : 0;
        if (!callee) { for (auto& p : invoke.Params()) ++formalCount; }
        //Method call (slotBase=1) when caller signals method context,
        //OR when callee is resolved to a method (Parent is class/struct/
        //interface). The method-context flag is authoritative for
        //built-in method calls where callee is null.
        bool isMethod = isMethodContext || (callee && callee->Parent()
            && (callee->Parent()->Kind() == NK_ClassDecl
                || callee->Parent()->Kind() == NK_StructDecl
                || callee->Parent()->Kind() == NK_InterfaceDecl));
        size_t slotBase = isMethod ? 1 : 0;
        size_t claimSize = formalCount + slotBase;

        //Peak depth of argument sub-expressions.
        uint16_t argDepth = 0;
        for (auto& param : invoke.Params()) {
            uint16_t d = ExprPeakDepth(param, visited);
            if (d > argDepth) argDepth = d;
        }

        //Peak depth of callee's default expressions (evaluated in caller frame).
        uint16_t defaultDepth = 0;
        if (callee && visited.find(callee) == visited.end()) {
            auto visitedPlus = visited;
            visitedPlus.insert(callee);
            for (auto& formal : callee->Params()) {
                if (formal.Value()) {
                    uint16_t d = ExprPeakDepth(*formal.Value(), visitedPlus);
                    if (d > defaultDepth) defaultDepth = d;
                }
            }
        }

        return static_cast<uint16_t>(claimSize) +
            (argDepth > defaultDepth ? argDepth : defaultDepth);
    }
    //Non-invoke expressions: recurse into children.
    //UnaryExpr: NLang uses NK_BinaryExpr for both binary and unary.
    //Unary ops (OP_Neg, OP_LogicalNot) have Right()==nullptr.
    if (kind == NK_BinaryExpr) {
        auto& bin = static_cast<SnBinaryExpr&>(expr);
        uint16_t l = ExprPeakDepth(*bin.Left(), visited);
        if (bin.Right()) {
            uint16_t r = ExprPeakDepth(*bin.Right(), visited);
            //Right operand parks in a per-level EvalAreaClaim(1) (round-4 —
            //the old PickTempSlot chain wrapped tempSlot4 → tempSlot at
            //depth 5); operand sub-expressions claim above it.
            uint16_t m = l > r ? l : r;
            return 1 + m;
        }
        return l;  //unary: operand → resultOffset, no claim
    }
    //CastExpr
    if (kind == NK_CastExpr) {
        auto& cast = static_cast<SnCastExpr&>(expr);
        return ExprPeakDepth(*cast.Source(), visited);
    }
    //AsExpr (expr as T)
    if (kind == NK_AsExpr) {
        auto& as = static_cast<SnAsExpr&>(expr);
        return ExprPeakDepth(*as.Operand(), visited);
    }
    //NamedArgExpr
    if (kind == NK_NamedArgExpr) {
        auto& named = static_cast<SnNamedArgExpr&>(expr);
        return ExprPeakDepth(*named.Inner(), visited);
    }
    //OutArgExpr (Phase 9e) — transparent like NamedArgExpr: its inner
    //identifier evaluates into the claimed binding slot.
    if (kind == NK_OutArgExpr) {
        auto& out = static_cast<SnOutArgExpr&>(expr);
        return ExprPeakDepth(*out.Inner(), visited);
    }
    //SubscriptExpr
    if (kind == NK_SubscriptExpr) {
        auto& sub = static_cast<SnSubscriptExpr&>(expr);
        uint16_t a = ExprPeakDepth(*sub.Array(), visited);
        uint16_t i = ExprPeakDepth(*sub.Index(), visited);
        //Both shapes claim 2 evalArea slots (receiver + index): container
        //lowers to a get() call, array reads park receiver/index in the
        //claim directly (Phase 10 audit round-3 — was a 4-deep PickTempSlot
        //chain that wrapped and clobbered at nesting depth 5).
        uint16_t claim = 2;
        uint16_t m = (a > i ? a : i);
        return claim + m;
    }
    //MemberExpr (field access: outer.inner)
    if (kind == NK_MemberExpr) {
        auto& member = static_cast<SnMemberExpr&>(expr);
        uint16_t d = ExprPeakDepth(*member.Outer(), visited, false);
        //If Inner is an InvokeExpr, this is a method call shape — pass
        //isMethodContext=true so the walker reserves slot 0 for `this`.
        uint16_t id = (member.Inner()
            && member.Inner()->Kind() == NK_InvokeExpr)
            ? ExprPeakDepth(*member.Inner(), visited, true)
            : ExprPeakDepth(*member.Inner(), visited, false);
        return d > id ? d : id;
    }
    //NewExpr
    //claimSize = 1 (this) + argCount, mirroring the codegen path which
    //claims an evalArea slice for {this, args...} then bulk-copies to
    //callParamBase before OP_CallMethodDirect. Conservative: claims even
    //when no ctor exists (alloc-only NewExpr doesn't need the slice, but
    //over-reserving by 1 slot is safe and rare).
    if (kind == NK_NewExpr) {
        auto& newExpr = static_cast<SnNewExpr&>(expr);
        size_t argCount = 0;
        for (auto& arg : newExpr.Args()) {
            if (arg.Kind() != NK_NameExpr) ++argCount;
        }
        uint16_t claimSize = static_cast<uint16_t>(1 + argCount);
        uint16_t d = 0;
        for (auto& arg : newExpr.Args()) {
            uint16_t ad = ExprPeakDepth(arg, visited, false);
            if (ad > d) d = ad;
        }
        return claimSize + d;
    }
    //NewArrayExpr
    if (kind == NK_NewArrayExpr) {
        auto& na = static_cast<SnNewArrayExpr&>(expr);
        return ExprPeakDepth(*na.Size(), visited);
    }
    //InitListExpr
    //Phase 9c follow-up: mirror the codegen's claim pattern.
    //  - Dict form: per-entry EvalAreaClaim(3) [this, key, value]
    //  - List form: per-entry EvalAreaClaim(2) [this, value] (Phase 10
    //    audit C2 — was callParamBase staging, clobbered by nested calls)
    //  - Array/Struct/Class forms: no claim (use temp slots)
    if (kind == NK_InitListExpr) {
        auto& init = static_cast<SnInitListExpr&>(expr);
        SnField* pTarget = init.EvalDataType();
        uint16_t claimSize = 0;
        if (pTarget && pTarget->Kind() == NK_ClassDecl) {
            auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
            const std::string& baseName = pClassDecl->BaseName();
            if (baseName == "Dict") claimSize = 3;
            else if (baseName == "List") claimSize = 2;
        }
        uint16_t maxChild = 0;
        for (auto& entry : init.Entries()) {
            if (entry.pValue) {
                uint16_t cd = ExprPeakDepth(*entry.pValue, visited);
                if (cd > maxChild) maxChild = cd;
            }
        }
        return claimSize + maxChild;
    }
    //Leaf expressions (LiteralExpr, IdentifierExpr, ThisExpr, etc.)
    return 0;
}

static uint16_t StmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    NodeKind kind = stmt.Kind();
    if (kind == NK_Paragraph) {
        auto& para = static_cast<SnParagraph&>(stmt);
        uint16_t d = 0;
        for (auto& child : para.Statements()) {
            uint16_t cd = StmtPeakDepth(child, visited);
            if (cd > d) d = cd;
        }
        return d;
    }
    if (kind == NK_ReturnStmt) {
        auto& ret = static_cast<SnReturnStmt&>(stmt);
        return ret.Result() ? ExprPeakDepth(*ret.Result(), visited) : 0;
    }
    if (kind == NK_InvokeStmt) {
        auto& invoke = static_cast<SnInvokeStmt&>(stmt);
        return ExprPeakDepth(*invoke.Expr(), visited);
    }
    if (kind == NK_LocalDeclStmt) {
        auto& decl = static_cast<SnLocalDeclStmt&>(stmt);
        //Initializer is handled by a subsequent AssignStmt.
        return 0;
    }
    if (kind == NK_AssignStmt) {
        auto& assign = static_cast<SnAssignStmt&>(stmt);
        //Walk the Left() lvalue too: a member-assign whose receiver is a
        //List/Dict subscript (`li[i].f = v`) emits a synthetic get() call
        //claiming 2 evalArea slots — under-reserving evalArea corrupts the
        //frame (walker-symmetry discipline, 5th instance).
        uint16_t l = assign.Left()
            ? ExprPeakDepth(*assign.Left(), visited) : 0;
        uint16_t r = assign.Right()
            ? ExprPeakDepth(*assign.Right(), visited) : 0;
        //Member targets park their staging in EvalAreaClaims (round-4):
        //array member-write (`arr[i].f = v`) claims 3 [array, index,
        //value]; plain member (`obj.f = v`), struct-to-struct and
        //container member-write (`li[i].f = v`) claim 2 [receiver, value].
        //The walker cannot cheaply separate the shapes, so claim a uniform
        //3 — over-reserving is the safe direction (container receivers
        //also count their get() claim inside the SubscriptExpr case).
        uint16_t claim = 0;
        if (assign.Left() && assign.Left()->Kind() == NK_MemberExpr)
            claim = 3;
        //Identifier targets (plain local `x = rhs`) stage the RHS in an
        //EvalAreaClaim(1) before copying to the destination (round-7 —
        //aliasing family). The struct branch (CopyStruct staging) and the
        //implicit this-field branch use temps only after their last
        //nested emission, needing no claim — over-reserve is safe.
        else if (assign.Left()
            && assign.Left()->Kind() == NK_IdentifierExpr)
            claim = 1;
        uint16_t m = l > r ? l : r;
        return claim + m;
    }
    if (kind == NK_IfStmt) {
        auto& ifStmt = static_cast<SnIfStmt&>(stmt);
        uint16_t d = ExprPeakDepth(*ifStmt.Cond(), visited);
        if (ifStmt.ThenStmt()) {
            uint16_t td = StmtPeakDepth(*ifStmt.ThenStmt(), visited);
            if (td > d) d = td;
        }
        if (ifStmt.ElseStmt()) {
            uint16_t ed = StmtPeakDepth(*ifStmt.ElseStmt(), visited);
            if (ed > d) d = ed;
        }
        return d;
    }
    if (kind == NK_WhileStmt) {
        auto& whileStmt = static_cast<SnWhileStmt&>(stmt);
        uint16_t d = ExprPeakDepth(*whileStmt.Cond(), visited);
        if (whileStmt.Body()) {
            uint16_t bd = StmtPeakDepth(*whileStmt.Body(), visited);
            if (bd > d) d = bd;
        }
        return d;
    }
    if (kind == NK_DoStmt) {
        auto& dw = static_cast<SnDoStmt&>(stmt);
        uint16_t d = ExprPeakDepth(*dw.Cond(), visited);
        if (dw.Body()) {
            uint16_t bd = StmtPeakDepth(*dw.Body(), visited);
            if (bd > d) d = bd;
        }
        return d;
    }
    if (kind == NK_ForStmt) {
        auto& forStmt = static_cast<SnForStmt&>(stmt);
        uint16_t d = 0;
        if (forStmt.Init()) { uint16_t id = StmtPeakDepth(*forStmt.Init(), visited); if (id > d) d = id; }
        if (forStmt.Cond()) { uint16_t cd = ExprPeakDepth(*forStmt.Cond(), visited); if (cd > d) d = cd; }
        if (forStmt.Fini()) { uint16_t fd = StmtPeakDepth(*forStmt.Fini(), visited); if (fd > d) d = fd; }
        if (forStmt.Body()) { uint16_t bd = StmtPeakDepth(*forStmt.Body(), visited); if (bd > d) d = bd; }
        return d;
    }
    if (kind == NK_SwitchStmt) {
        auto& sw = static_cast<SnSwitchStmt&>(stmt);
        uint16_t d = ExprPeakDepth(*sw.Cond(), visited);
        for (auto* c : sw.Cases()) {
            //SnCaseClause inherits SyntaxNode, not SnStatement — inline the walk.
            if (c->Cond()) {
                uint16_t cd = ExprPeakDepth(*c->Cond(), visited);
                if (cd > d) d = cd;
            }
            if (c->Body()) {
                for (auto& s : c->Body()->Statements()) {
                    uint16_t sd = StmtPeakDepth(s, visited);
                    if (sd > d) d = sd;
                }
            }
        }
        return d;
    }
    if (kind == NK_ForeachStmt) {
        auto& fe = static_cast<SnForeachStmt&>(stmt);
        uint16_t d = ExprPeakDepth(*fe.Iterable(), visited);
        if (fe.Body()) {
            uint16_t bd = StmtPeakDepth(*fe.Body(), visited);
            if (bd > d) d = bd;
        }
        return d;
    }
    if (kind == NK_BreakStmt || kind == NK_ContinueStmt) {
        return 0;
    }
    if (kind == NK_AssertStmt) {
        auto& as = static_cast<SnAssertStmt&>(stmt);
        return ExprPeakDepth(*as.Cond(), visited);
    }
    if (kind == NK_TryStmt) {
        auto& ts = static_cast<SnTryStmt&>(stmt);
        uint16_t d = 0;
        if (ts.TryBody()) { uint16_t bd = StmtPeakDepth(*ts.TryBody(), visited); if (bd > d) d = bd; }
        for (auto* c : ts.Catches()) {
            if (c->Body()) { uint16_t bd = StmtPeakDepth(*c->Body(), visited); if (bd > d) d = bd; }
        }
        if (ts.FinallyBody()) { uint16_t fd = StmtPeakDepth(*ts.FinallyBody(), visited); if (fd > d) d = fd; }
        return d;
    }
    if (kind == NK_SuperCallStmt) {
        auto& sc = static_cast<SnSuperCallStmt&>(stmt);
        //Mirror the codegen's EvalAreaClaim(1 + args) — same claimSize
        //pattern as NK_NewExpr in ExprPeakDepth. peakDepth without the
        //claimSize under-sizes evalArea and super-arg staging writes past
        //the frame (heap-buffer-overflow, deterministic in codegen).
        uint16_t claimSize = static_cast<uint16_t>(1 + sc.Args().size());
        uint16_t d = 0;
        for (auto* arg : sc.Args()) {
            uint16_t ad = ExprPeakDepth(*arg, visited);
            if (ad > d) d = ad;
        }
        return claimSize + d;
    }
    if (kind == NK_ThrowStmt) {
        auto& th = static_cast<SnThrowStmt&>(stmt);
        return th.Expr() ? ExprPeakDepth(*th.Expr(), visited) : 0;
    }
    if (kind == NK_CompoundAssignStmt) {
        auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
        //Member targets (`obj.f += v` and implicit `this.f += v`) park
        //receiver/old-value/RHS in an EvalAreaClaim(3); the walker cannot
        //distinguish the implicit-this shape (Left is a bare identifier
        //resolving to a this-field at codegen time), so claim 3
        //unconditionally — local targets stage in tempSlot2 and merely
        //over-reserve (the safe direction). Walk the Left() receiver too:
        //`mk().x += 1` hides a call in the receiver.
        uint16_t claim = 3;
        uint16_t l = ca.Left() ? ExprPeakDepth(*ca.Left(), visited) : 0;
        uint16_t r = ca.Right() ? ExprPeakDepth(*ca.Right(), visited) : 0;
        return claim + (l > r ? l : r);
    }
    if (kind == NK_SubscriptAssignStmt) {
        auto& sa = static_cast<SnSubscriptAssignStmt&>(stmt);
        //Both paths run inside an EvalAreaClaim(3) [receiver + index +
        //value] — container lowers to a set() call, array to a direct
        //StoreElement; either way the walker must mirror the codegen's
        //claim. walker-symmetry discipline, 4th instance (after
        //callparambase-clobber #2 and super() #3).
        uint16_t claim = 3;
        uint16_t d = ExprPeakDepth(*sa.Index(), visited);
        uint16_t v = ExprPeakDepth(*sa.Value(), visited);
        uint16_t a = ExprPeakDepth(*sa.Array(), visited);
        uint16_t m = d > v ? d : v;
        if (a > m) m = a;
        return claim + m;
    }
    return 0;
}

static CallSlotStats ComputeCallSlotStats(SnFunction& sn) {
    CallSlotStats stats{1, 1};  //min 1 slot each
    std::unordered_set<SnFunction*> visited;
    visited.insert(&sn);

    //Walk body for peakDepth.
    if (sn.Body()) {
        for (auto& stmt : sn.Body()->Statements()) {
            uint16_t d = StmtPeakDepth(stmt, visited);
            if (d > stats.peakDepth) stats.peakDepth = d;
        }
    }

    //Walk body for maxArgs (max callee formal count + slotBase).
    //Reuse a simple recursive helper.
    struct MaxArgsWalker {
        uint16_t maxArgs = 1;
        void walkExpr(SnExpression& expr, bool isMethodContext = false) {
            if (expr.Kind() == NK_InvokeExpr) {
                auto& invoke = static_cast<SnInvokeExpr&>(expr);
                auto* callee = invoke.Callee();
                size_t formalCount = callee ? callee->Params().size() : 0;
                if (!callee) { for (auto& p : invoke.Params()) ++formalCount; }
                //Method call detection: trust isMethodContext flag (set
                //when Inner of MemberExpr) OR callee->Parent() is class.
                //Built-in method calls have callee=null, so the flag is
                //the only reliable signal.
                bool isMethod = isMethodContext || (callee && callee->Parent()
                    && (callee->Parent()->Kind() == NK_ClassDecl
                        || callee->Parent()->Kind() == NK_StructDecl
                        || callee->Parent()->Kind() == NK_InterfaceDecl));
                size_t slotBase = isMethod ? 1 : 0;
                uint16_t claimSize = static_cast<uint16_t>(
                    formalCount + slotBase);
                if (claimSize > maxArgs) maxArgs = claimSize;
                //Also walk the invoke's own params for nested calls.
                for (auto& param : invoke.Params())
                    walkExpr(param);
                return;
            }
            //Recurse into children for nested calls.
            if (expr.Kind() == NK_BinaryExpr) {
                auto& bin = static_cast<SnBinaryExpr&>(expr);
                walkExpr(*bin.Left());
                if (bin.Right()) walkExpr(*bin.Right());
            } else if (expr.Kind() == NK_CastExpr) {
                walkExpr(*static_cast<SnCastExpr&>(expr).Source());
            } else if (expr.Kind() == NK_AsExpr) {
                walkExpr(*static_cast<SnAsExpr&>(expr).Operand());
            } else if (expr.Kind() == NK_NamedArgExpr) {
                walkExpr(*static_cast<SnNamedArgExpr&>(expr).Inner());
            } else if (expr.Kind() == NK_OutArgExpr) {
                //Phase 9e: out arg — walk the inner identifier.
                walkExpr(*static_cast<SnOutArgExpr&>(expr).Inner());
            } else if (expr.Kind() == NK_SubscriptExpr) {
                auto& sub = static_cast<SnSubscriptExpr&>(expr);
                //List/Dict subscript lowers to a synthetic get() call that
                //bulk-copies 2 slots (this + index) into callParamBase —
                //mirror the codegen so maxArgs never under-reserves the
                //region (a lone `li[0]` with no other calls would size
                //callParamBase at 1 and the get() would overflow it).
                if (IsContainerSubscript(*sub.Array()) && maxArgs < 2)
                    maxArgs = 2;
                walkExpr(*sub.Array());
                walkExpr(*sub.Index());
            } else if (expr.Kind() == NK_MemberExpr) {
                auto& member = static_cast<SnMemberExpr&>(expr);
                walkExpr(*member.Outer(), false);
                //If Inner is InvokeExpr, pass method-context flag so
                //slot 0 is reserved for `this`.
                if (member.Inner()
                    && member.Inner()->Kind() == NK_InvokeExpr)
                    walkExpr(*member.Inner(), true);
                else
                    walkExpr(*member.Inner(), false);
            } else if (expr.Kind() == NK_NewExpr) {
                //claimSize for ctor call = 1 (this) + argCount.
                auto& newExpr = static_cast<SnNewExpr&>(expr);
                size_t argCount = 0;
                for (auto& arg : newExpr.Args())
                    if (arg.Kind() != NK_NameExpr) ++argCount;
                uint16_t claimSize = static_cast<uint16_t>(1 + argCount);
                if (claimSize > maxArgs) maxArgs = claimSize;
                for (auto& arg : newExpr.Args())
                    walkExpr(arg);
            } else if (expr.Kind() == NK_NewArrayExpr) {
                walkExpr(*static_cast<SnNewArrayExpr&>(expr).Size());
            } else if (expr.Kind() == NK_InitListExpr) {
                //Phase 9c follow-up: collection inits emit implicit calls:
                //  - Dict: OP_CallMethod "set" with 3 slots (this, key, value)
                //  - List: OP_CallMethod "add" with 2 slots (this, value)
                //Track for maxArgs so callParamBase is sized correctly when
                //the legacy floor (8) is eventually removed.
                auto& init = static_cast<SnInitListExpr&>(expr);
                SnField* pTarget = init.EvalDataType();
                if (pTarget && pTarget->Kind() == NK_ClassDecl) {
                    auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
                    const std::string& bn = pClassDecl->BaseName();
                    if (bn == "Dict" && 3 > maxArgs) maxArgs = 3;
                    else if (bn == "List" && 2 > maxArgs) maxArgs = 2;
                }
                for (auto& entry : init.Entries())
                    if (entry.pValue) walkExpr(*entry.pValue);
            }
        }
        void walkStmt(SnStatement& stmt) {
            NodeKind kind = stmt.Kind();
            if (kind == NK_Paragraph) {
                for (auto& child : static_cast<SnParagraph&>(stmt).Statements())
                    walkStmt(child);
            } else if (kind == NK_ReturnStmt) {
                auto* r = static_cast<SnReturnStmt&>(stmt).Result();
                if (r) walkExpr(*r);
            } else if (kind == NK_InvokeStmt) {
                walkExpr(*static_cast<SnInvokeStmt&>(stmt).Expr());
            } else if (kind == NK_AssignStmt) {
                //Walk the Left() lvalue too — `li[i].f = v` receivers emit
                //a synthetic get() call needing 2 callParamBase slots.
                auto& as = static_cast<SnAssignStmt&>(stmt);
                auto* l = as.Left();
                if (l) walkExpr(*l);
                auto* v = as.Right();
                if (v) walkExpr(*v);
            } else if (kind == NK_IfStmt) {
                auto& ifStmt = static_cast<SnIfStmt&>(stmt);
                walkExpr(*ifStmt.Cond());
                if (ifStmt.ThenStmt()) walkStmt(*ifStmt.ThenStmt());
                if (ifStmt.ElseStmt()) walkStmt(*ifStmt.ElseStmt());
            } else if (kind == NK_WhileStmt) {
                auto& w = static_cast<SnWhileStmt&>(stmt);
                walkExpr(*w.Cond());
                if (w.Body()) walkStmt(*w.Body());
            } else if (kind == NK_DoStmt) {
                auto& dw = static_cast<SnDoStmt&>(stmt);
                walkExpr(*dw.Cond());
                if (dw.Body()) walkStmt(*dw.Body());
            } else if (kind == NK_ForStmt) {
                auto& f = static_cast<SnForStmt&>(stmt);
                if (f.Init()) walkStmt(*f.Init());
                if (f.Cond()) walkExpr(*f.Cond());
                if (f.Fini()) walkStmt(*f.Fini());
                if (f.Body()) walkStmt(*f.Body());
            } else if (kind == NK_SwitchStmt) {
                auto& sw = static_cast<SnSwitchStmt&>(stmt);
                walkExpr(*sw.Cond());
                for (auto* c : sw.Cases()) {
                    //SnCaseClause inherits SyntaxNode, not SnStatement.
                    if (c->Cond()) walkExpr(*c->Cond());
                    if (c->Body()) {
                        for (auto& s : c->Body()->Statements())
                            walkStmt(s);
                    }
                }
            } else if (kind == NK_ForeachStmt) {
                auto& fe = static_cast<SnForeachStmt&>(stmt);
                walkExpr(*fe.Iterable());
                if (fe.Body()) walkStmt(*fe.Body());
            } else if (kind == NK_AssertStmt) {
                walkExpr(*static_cast<SnAssertStmt&>(stmt).Cond());
            } else if (kind == NK_TryStmt) {
                auto& ts = static_cast<SnTryStmt&>(stmt);
                if (ts.TryBody()) walkStmt(*ts.TryBody());
                for (auto* c : ts.Catches()) {
                    if (c->Body()) walkStmt(*c->Body());
                }
                if (ts.FinallyBody()) walkStmt(*ts.FinallyBody());
            } else if (kind == NK_SuperCallStmt) {
                //super(args) claims 1 (this) + argc slots at call time.
                auto& sc = static_cast<SnSuperCallStmt&>(stmt);
                uint16_t claimSize = static_cast<uint16_t>(1 + sc.Args().size());
                if (claimSize > maxArgs) maxArgs = claimSize;
                for (auto* arg : sc.Args())
                    walkExpr(*arg);
            } else if (kind == NK_ThrowStmt) {
                auto* e = static_cast<SnThrowStmt&>(stmt).Expr();
                if (e) walkExpr(*e);
            } else if (kind == NK_CompoundAssignStmt) {
                auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
                //Walk Left() receiver too — `mk().x += 1` hides a call
                //needing callParamBase slots (C1-family asymmetry).
                if (ca.Left()) walkExpr(*ca.Left());
                if (ca.Right()) walkExpr(*ca.Right());
            } else if (kind == NK_SubscriptAssignStmt) {
                auto& sa = static_cast<SnSubscriptAssignStmt&>(stmt);
                //List/Dict subscript store lowers to a set() call that
                //bulk-copies 3 slots into callParamBase — reserve them.
                //Array stores read their claim slots directly (StoreElement
                //takes explicit operand offsets) and touch no call params.
                if (IsContainerSubscript(*sa.Array()) && maxArgs < 3)
                    maxArgs = 3;
                //Phase 10 audit C1: walk the array base too — a call in the
                //base (`makeArr(...)[0] = v`) needs its arg slots reserved or
                //the bulk-copy overflows callParamBase into evalArea.
                //StmtPeakDepth already walked it; this closes the asymmetry.
                walkExpr(*sa.Array());
                walkExpr(*sa.Index());
                walkExpr(*sa.Value());
            }
        }
    };

    MaxArgsWalker walker;
    if (sn.Body()) {
        for (auto& stmt : sn.Body()->Statements())
            walker.walkStmt(stmt);
    }
    stats.maxArgs = walker.maxArgs;
    return stats;
}

void VmBackend::GenerateFunction(SnFunction& func, size_t funcIdx) {
    CompiledFunction& compiledFunc = m_compiledModule.functions[funcIdx];

    //Phase 9f: native function declaration (`native int f(...);`). No
    //bytecode — the VM dispatches by name through the host-registered
    //native table (VmExecutor::RegisterNative). The record carries only
    //the signature: the native reads args directly from the caller's
    //callParamBase cells and writes the return into pResult.
    if (func.ContainFlags(NF_Native)) {
        compiledFunc.isNative = true;
        bool isMethod = func.Parent() && func.Parent()->Kind() == NK_ClassDecl;
        compiledFunc.paramCount = static_cast<uint16_t>(
            func.Params().size() + (isMethod ? 1 : 0));
        compiledFunc.localsSize = compiledFunc.paramCount * VALUE_SIZE;
        if (func.HasReturn() && func.ReturnType()) {
            auto* retType = func.ReturnType()->Field();
            compiledFunc.returnTypeKind = retType
                ? static_cast<uint16_t>(RuntimeTypeKind(retType)) : 0;
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

    // If this is a class method, allocate slot 0 for the 'this' pointer.
    bool isMethod = func.Parent() && func.Parent()->Kind() == NK_ClassDecl;
    if (isMethod) {
        AllocLocal("__this", VALUE_SIZE, RTK_Class, true);
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
        auto* retType = func.ReturnType()->Field();
        compiledFunc.returnTypeKind = retType
            ? static_cast<uint16_t>(RuntimeTypeKind(retType)) : 0;
        ctx.returnSlot = ctx.nextOffset;
        ctx.nextOffset += VALUE_SIZE;
    } else {
        //Void functions carry RTK_Void so cross-module stubs rebuild
        //without a return type (CreateFunctionStub: RTK_Void →
        //HasReturn() == false). The previous default of 0 (RTK_Int32)
        //made an imported void stub claim an int return value.
        compiledFunc.returnTypeKind = RTK_Void;
    }

    // Temporary slots pool (4 slots — supports up to 3-level nested binary
    // expressions without clobbering; see PickTempSlot).
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

    m_currFunc = nullptr;
}

void VmBackend::EmitBinding(const FormalBinding* pBindings, size_t bindingIdx,
                              uint16_t slotIdx, size_t slotBase,
                              BytecodeEmitter& emitter, uint16_t thisSlot,
                              uint16_t claimBase)
{
    const auto& b = pBindings[bindingIdx];
    //claimBase is the evalArea claim slice base. Always provided by
    //EmitCallArgs (the only caller). Bindings emit into the claim slice;
    //a bulk-copy loop in EmitCallArgs then moves them to callParamBase.
    uint16_t base = claimBase;
    uint16_t paramOffset = base + slotIdx * VALUE_SIZE;

    if (b.kind == FormalBinding::B_Default) {
        assert(b.pFormal && b.pFormal->Value());
        OverrideScope scope(*this);
        for (size_t j = 0; j < bindingIdx; ++j) {
            scope.Add(pBindings[j].pFormal->Name(),
                      base + (static_cast<uint16_t>(j + slotBase)) * VALUE_SIZE);
        }
        if (thisSlot != UINT16_MAX) {
            scope.BindThis(thisSlot);
        }
        EmitExpression(*b.pFormal->Value(), emitter, paramOffset);
    } else {
        assert(b.pCallerExpr);
        EmitExpression(*b.pCallerExpr, emitter, paramOffset);
    }

    //Struct deep-copy: if the formal is a struct type, copy the heap
    //subtree so the callee gets its own.
    auto* pFormalType = b.pFormal->EvalDataType();
    if (pFormalType && RuntimeTypeKind(pFormalType) == RTK_Struct) {
        int structIdx = m_compiledModule.FindStruct(pFormalType->Name());
        emitter.Emit(OpCode::OP_CopyStruct);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.EmitUint16(paramOffset);
        emitter.EmitUint16(structIdx >= 0
            ? static_cast<uint16_t>(structIdx) : 0);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(paramOffset);
    }
}

void VmBackend::EmitOutSpills(const std::vector<OutSpill>& spills,
                              BytecodeEmitter& emitter)
{
    for (const auto& s : spills) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(m_currFunc->callParamBase
                           + s.slotIdx * VALUE_SIZE);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(s.localOffset);
    }
}

uint32_t VmBackend::BuildOutMask(const std::vector<OutSpill>& spills)
{
    uint32_t mask = 0;
    for (const auto& s : spills) {
        if (s.slotIdx >= 32)
            throw std::runtime_error(
                "NLang backend: out parameter slot >= 32 is unsupported");
        mask |= 1u << s.slotIdx;
    }
    return mask;
}

void VmBackend::EmitCallArgs(const SnInvokeExpr& invoke, SnFunction* pCallee,
                              BytecodeEmitter& emitter, size_t slotBase,
                              const std::map<uint16_t, ArgBoxPlan>* pArgPlans,
                              uint16_t thisSlot,
                              std::vector<OutSpill>* pOutSpills) {
    //Determine total claim size (slotBase + arg count).
    const auto& bindings = invoke.Bindings();
    size_t argCount;
    if (!bindings.empty()) {
        argCount = bindings.size();
    } else {
        argCount = 0;
        for (auto& p : invoke.Params()) ++argCount;
    }
    uint16_t n = static_cast<uint16_t>(argCount + slotBase);

    //Claim a slice of the evalArea for this call's bindings.
    //Nested calls claim deeper slices, so inner bindings never overwrite
    //outer bindings. The RAII guard releases the claim on return.
    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();

    //Optional per-arg boxing application (built-in generic class methods).
    auto applyBox = [&](uint16_t slotIdx) {
        if (!pArgPlans) return;
        auto it = pArgPlans->find(slotIdx);
        if (it == pArgPlans->end() || !it->second.needsBox) return;
        uint16_t paramOffset = claimBase + slotIdx * VALUE_SIZE;
        EmitPResultRefresh(emitter, paramOffset);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(it->second.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(paramOffset);
    };

    //For method calls (slotBase=1), copy the receiver to claim[0] BEFORE
    //emitting bindings. Default-param expressions referencing `this` need
    //it available via OverrideScope/ThisOverrideStack.
    if (slotBase == 1 && thisSlot != UINT16_MAX) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(thisSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(claimBase);
    }

    //Emit each binding into the claimed evalArea slice.
    if (!pCallee) {
        //Unresolved invoke — fall back to legacy positional emit.
        uint16_t paramIdx = static_cast<uint16_t>(slotBase);
        for (auto& param : invoke.Params()) {
            uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
            EmitExpression(param, emitter, paramOffset);
            applyBox(paramIdx);
            ++paramIdx;
        }
    } else if (bindings.empty()) {
        //Legacy path: caller didn't go through Phase 9c resolver.
        uint16_t paramIdx = static_cast<uint16_t>(slotBase);
        for (auto& param : invoke.Params()) {
            uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
            EmitExpression(param, emitter, paramOffset);
            applyBox(paramIdx);
            ++paramIdx;
        }
    } else {
        if (bindings.size() + slotBase > kMaxFuncParams) {
            assert(false && "function parameters exceed kMaxFuncParams sanity ceiling");
            return;
        }

        for (size_t i = 0; i < bindings.size(); ++i) {
            uint16_t slotIdx = static_cast<uint16_t>(i + slotBase);
            EmitBinding(bindings.data(), i, slotIdx, slotBase, emitter,
                        thisSlot, claimBase);
            applyBox(slotIdx);
            //Phase 9e: out binding — record the caller local for the
            //post-call spill. The resolver guarantees pCallerExpr is a
            //plain identifier bound to a caller-frame slot (local var or
            //formal param), so the spill target is a plain frame offset.
            if (bindings[i].bIsOut) {
                auto& idExpr = static_cast<SnIdentifierExpr&>(
                    *bindings[i].pCallerExpr);
                auto target = ResolveBareIdentifier(idExpr.Field());
                if (target.kind != BareIdTarget::Local)
                    throw std::runtime_error(
                        "NLang backend: out argument is not a local variable");
                if (pOutSpills)
                    pOutSpills->push_back({slotIdx, target.localOffset});
            }
        }
    }

    //Bulk-copy evalArea claim → callParamBase just before the call.
    //OP_VarLocal reads from claimBase+i*4, OP_Assign writes to
    //callParamBase+i*4. This preserves any tagged Value representation
    //(boxed heap idx, string pool idx, etc.) since both opcodes copy
    //4 raw bytes.
    for (uint16_t i = 0; i < n; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
    }
    //EvalAreaClaim destructor releases the claim automatically.
}

void VmBackend::EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                                uint16_t resultOffset) {
    NodeKind kind = expr.Kind();

    if (kind == NK_LiteralExpr) {
        auto& lit = static_cast<SnLiteralExpr&>(expr);
        auto* evalType = lit.EvalDataType();

        if (!evalType) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }

        NodeKind typeKind = evalType->Kind();
        if (typeKind == NK_Int32) {
            int32_t v = lit.Value().Get<int32_t>();
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(v);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else if (typeKind == NK_Float) {
            float v = lit.Value().Get<float>();
            emitter.Emit(OpCode::OP_ConstFloat);
            emitter.EmitFloat(v);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else if (typeKind == NK_String) {
            auto* pStr = lit.Value().Data().m_String;
            std::string sVal = pStr ? *pStr : "";
            uint16_t poolIdx = AddStringConstant(sVal);
            emitter.Emit(OpCode::OP_ConstString);
            emitter.EmitUint16(poolIdx);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        }
        return;
    }

    if (kind == NK_IdentifierExpr) {
        auto& idExpr = static_cast<SnIdentifierExpr&>(expr);
        //Phase 9c: binding override — when evaluating a default-param
        //expression, an identifier referring to an earlier formal must
        //read from the caller-side callParamBase slot rather than the
        //callee's local frame. Check the override stack before falling
        //through to normal local/global resolution.
        auto override = LookupOverride(idExpr.Name());
        if (override.first) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(override.second);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        auto* field = idExpr.Field();
        if (field && field->Kind() == NK_EnumMember) {
            //Enum member constant — emit the resolved integer value.
            auto* pEnumMember = static_cast<SnEnumMember*>(field);
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(pEnumMember->Value());
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        if (field) {
            auto target = ResolveBareIdentifier(field);
            if (target.kind == BareIdTarget::Local) {
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(target.localOffset);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            } else if (target.kind == BareIdTarget::ThisField) {
                //Implicit this.<field> (bare member read inside a method).
                //Same opcode shape as the MemberExpr class-field read.
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(ImplicitThisSlot());
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
            } else {
                throw std::runtime_error(
                    "NLang backend: identifier has no codegen binding: "
                    + field->Name());
            }
        } else {
            //Unresolved identifier — write zero as fallback.
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        }
        return;
    }

    if (kind == NK_InvokeExpr) {
        auto& invoke = static_cast<SnInvokeExpr&>(expr);
        auto* callee = invoke.Callee();

        //Phase 9c: binding-aware argument emission. Handles positional,
        //named, and default-param bindings via EmitCallArgs.
        //Phase 9e: out arguments are collected here and written back
        //right after the call via the *Out opcode + spills.
        std::vector<OutSpill> outSpills;
        EmitCallArgs(invoke, callee, emitter, /*slotBase=*/0,
                     /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX,
                     &outSpills);

        // Find function index
        int funcIndex = -1;
        if (callee) {
            auto it = m_funcIndexMap.find(callee);
            if (it != m_funcIndexMap.end())
                funcIndex = static_cast<int>(it->second);
        }
        //callee == null means unresolved invoke — skip (compiler should have reported error)
        if (funcIndex >= 0) {
            if (!outSpills.empty()) {
                emitter.Emit(OpCode::OP_CallFuncOut);
                emitter.EmitUint16(static_cast<uint16_t>(funcIndex));
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.EmitInt32(static_cast<int32_t>(
                    BuildOutMask(outSpills)));
                //Result first: the spills below clobber pResult via
                //OP_VarLocal, so the call's return value must be stored
                //before any writeback.
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                EmitOutSpills(outSpills, emitter);
            } else {
                emitter.Emit(OpCode::OP_CallFunc);
                emitter.EmitUint16(static_cast<uint16_t>(funcIndex));
                emitter.EmitUint16(m_currFunc->callParamBase);
                // Result is in pResult, store to resultOffset
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            }
        }
        emitter.Emit(OpCode::OP_ParaEnd);
        return;
    }

    //Phase 9e: out arguments are consumed by the binding path in
    //EmitCallArgs (the wrapper never emits itself). Reaching this point
    //means the wrapper flowed into a non-binding emit site (ctor call,
    //super(...), or a legacy path) — all rejected by the resolver, so
    //this is an internal error, not a fallback.
    if (kind == NK_OutArgExpr) {
        throw std::runtime_error(
            "NLang backend: out argument in unsupported call form");
    }

    if (kind == NK_CastExpr) {
        auto& cast = static_cast<SnCastExpr&>(expr);
        EmitExpression(*cast.Source(), emitter, resultOffset);

        //Phase 8e-1: TCK_Box — primitive → Object implicit boxing.
        //The cast kind was computed by CastInfo when source was primitive
        //and target was Object. Emit OP_Box with the source's type tag
        //(RTK_Int32/RTK_Float/RTK_String) so the VM knows what to wrap.
        if (cast.CastKind() == TCK_Box) {
            auto* sourceType = cast.Source()->EvalDataType();
            uint8_t typeTag = RTK_Int32;
            if (sourceType) {
                NodeKind srcKind = sourceType->Kind();
                if (srcKind == NK_Float) typeTag = RTK_Float;
                else if (srcKind == NK_String) typeTag = RTK_String;
                else typeTag = RTK_Int32;
            }
            EmitPResultRefresh(emitter, resultOffset);
            emitter.Emit(OpCode::OP_Box);
            emitter.EmitByte(typeTag);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }

        auto* targetType = cast.Target();
        auto* sourceType = cast.Source()->EvalDataType();
        if (sourceType && targetType) {
            NodeKind srcKind = sourceType->Kind();
            NodeKind dstKind = targetType->Kind();
            //Phase 8e-9b: enum → string emits OP_Enum_to_str (preserves enum
            //identity for name lookup). Must check BEFORE collapsing enum to
            //int below, otherwise OP_Int32_to_str would fire and produce a
            //numeric string instead of the value name.
            //
            //Two detection paths:
            // (a) srcKind == NK_EnumDecl — explicit enum-typed variable cast
            //     (e.g. `(Color) c` where c is some int).
            // (b) Source expression's Field() is SnEnumMember — enum literal
            //     access like Color.Red wrapped by binary strengthening.
            //     EvalDataType is NK_Int32 here (SnEnumMember::EvalDataType
            //     returns NK_Int32), so srcKind is NK_Int32 — we must walk
            //     the Field() chain to discover the enum decl.
            if (dstKind == NK_String) {
                SnEnumDecl* pEnumDecl = nullptr;
                if (srcKind == NK_EnumDecl) {
                    pEnumDecl = static_cast<SnEnumDecl*>(sourceType);
                } else {
                    //Try Field() chain on source expression.
                    auto srcExprKind = cast.Source()->Kind();
                    if (srcExprKind == NK_MemberExpr
                        || srcExprKind == NK_IdentifierExpr)
                    {
                        auto& srcFieldExpr = static_cast<SnFieldExpr&>(
                            *cast.Source());
                        auto* srcField = srcFieldExpr.Field();
                        if (srcField
                            && srcField->Kind() == NK_EnumMember)
                        {
                            pEnumDecl = static_cast<SnEnumDecl*>(
                                srcField->Parent());
                        }
                    }
                }
                if (pEnumDecl) {
                    auto it = m_enumIndexMap.find(pEnumDecl);
                    if (it != m_enumIndexMap.end()) {
                        emitter.Emit(OpCode::OP_Enum_to_str);
                        emitter.EmitUint16(static_cast<uint16_t>(it->second));
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        return;
                    }
                }
            }
            //Phase 8e-9b: class → string emits a virtual toString() call.
            //Setup: copy resultOffset → callParamBase[0], OP_CallMethod by
            //name "toString", result lands in pResult, copy → resultOffset.
            if (srcKind == NK_ClassDecl && dstKind == NK_String) {
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase);
                uint16_t nameIdx = AddStringConstant("toString");
                emitter.Emit(OpCode::OP_CallMethod);
                emitter.EmitUint16(nameIdx);
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                return;
            }
            //Phase 9b-pre: array → string emits OP_Array_to_str (Array is a
            //VM primitive, not a class, so OP_CallMethod doesn't apply).
            //Array types don't set EvalDataType to an array-kind node; the
            //type info lives on the variable's SnField (IsArrayType flag).
            //Detection: walk the source expression's Field() chain.
            if (dstKind == NK_String) {
                auto srcExprKind = cast.Source()->Kind();
                if (srcExprKind == NK_MemberExpr
                    || srcExprKind == NK_IdentifierExpr)
                {
                    auto& srcFieldExpr = static_cast<SnFieldExpr&>(
                        *cast.Source());
                    auto* srcField = srcFieldExpr.Field();
                    if (srcField && srcField->IsArrayType()) {
                        emitter.Emit(OpCode::OP_Array_to_str);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        return;
                    }
                }
            }
            if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
            if (dstKind == NK_EnumDecl) dstKind = NK_Int32;
            if (srcKind == NK_Int32 && dstKind == NK_Float) {
                EmitPResultRefresh(emitter, resultOffset);
                emitter.Emit(OpCode::OP_CastIntToFloat);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            } else if (srcKind == NK_Float && dstKind == NK_Int32) {
                EmitPResultRefresh(emitter, resultOffset);
                emitter.Emit(OpCode::OP_CastFloatToInt);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            } else if (srcKind == NK_Int32 && dstKind == NK_String) {
                //Phase 8e-9a: int → string coercion for `int + string` etc.
                EmitPResultRefresh(emitter, resultOffset);
                emitter.Emit(OpCode::OP_Int32_to_str);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            } else if (srcKind == NK_Float && dstKind == NK_String) {
                //Phase 8e-9a: float → string coercion.
                EmitPResultRefresh(emitter, resultOffset);
                emitter.Emit(OpCode::OP_Float_to_str);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            }
        }
        return;
    }

    //Phase 8e-1.5: `expr as T` runtime-checked cast.
    //Valid kinds: TCK_Same (no-op), TCK_Box (primitive→Object), TCK_Unbox
    //(Object→primitive), TCK_Downcast (ancestor→subclass).
    if (kind == NK_AsExpr) {
        auto& asExpr = static_cast<SnAsExpr&>(expr);
        //Evaluate operand to resultOffset. After this, pResult holds the
        //value (heap idx for ref types) per EmitExpression convention.
        EmitExpression(*asExpr.Operand(), emitter, resultOffset);

        auto kind = asExpr.CastKind();
        if (kind == TCK_Same) {
            //Already the right type — no opcode needed.
            return;
        }
        if (kind == TCK_Box) {
            //Symmetric to NK_CastExpr's TCK_Box path: emit OP_Box typeTag.
            auto* sourceType = asExpr.Operand()->EvalDataType();
            uint8_t typeTag = RTK_Int32;
            if (sourceType) {
                NodeKind srcKind = sourceType->Kind();
                if (srcKind == NK_Float) typeTag = RTK_Float;
                else if (srcKind == NK_String) typeTag = RTK_String;
                else typeTag = RTK_Int32;
            }
            EmitPResultRefresh(emitter, resultOffset);
            emitter.Emit(OpCode::OP_Box);
            emitter.EmitByte(typeTag);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        if (kind == TCK_Unbox) {
            //Target type is primitive — derive RTK_* from target.
            auto* targetType = asExpr.ResolvedTarget();
            uint8_t typeTag = RTK_Int32;
            if (targetType) {
                NodeKind tgtKind = targetType->Kind();
                if (tgtKind == NK_Float) typeTag = RTK_Float;
                else if (tgtKind == NK_String) typeTag = RTK_String;
                else typeTag = RTK_Int32;
            }
            EmitPResultRefresh(emitter, resultOffset);
            emitter.Emit(OpCode::OP_Unbox);
            emitter.EmitByte(typeTag);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        if (kind == TCK_Downcast) {
            //Target is a subclass — emit OP_CheckCast classIdx.
            auto* targetType = asExpr.ResolvedTarget();
            uint16_t classIdx = 0;
            if (targetType) {
                int idx = m_compiledModule.FindClass(targetType->Name());
                classIdx = (idx >= 0)
                    ? static_cast<uint16_t>(idx) : 0;
            }
            emitter.Emit(OpCode::OP_CheckCast);
            emitter.EmitUint16(classIdx);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        //Other kinds (TCK_Auto, TCK_Dynamic, TCK_None) should have been
        //rejected by ExprResolver.Access(SnAsExpr&). Defensive fallback.
        return;
    }

    // Member expression - struct field access or delegate to inner
    if (kind == NK_MemberExpr) {
        auto& member = static_cast<SnMemberExpr&>(expr);
        auto* field = member.Field();
        if (field && field->Kind() == NK_EnumMember) {
            //Enum member constant (e.g. Color.Red).
            auto* pEnumMember = static_cast<SnEnumMember*>(field);
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(pEnumMember->Value());
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        //Phase 8e-9b: non-class receiver toString() dispatch.
        //For enum/int/float receivers, the resolver accepted the call
        //(EvalDataType=String, NF_Resolved) without setting m_pField.
        //Codegen dispatches based on outer->EvalDataType():
        // - enum (NK_EnumDecl or NK_EnumMember Field()) → OP_Enum_to_str
        // - int (NK_Int32) → OP_Int32_to_str
        // - float (NK_Float) → OP_Float_to_str
        // - string (NK_String) → identity (no opcode)
        //This mirrors the SnCastExpr handler's dispatch but for the
        //MemberExpr+InvokeExpr AST shape that `x.toString()` produces.
        {
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_InvokeExpr)
            {
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                if (invoke.CalleeName() == "toString")
                {
                    auto* outerType = member.Outer()->EvalDataType();
                    NodeKind outerKind = outerType ? outerType->Kind() : static_cast<NodeKind>(0);
                    //Phase 9b-pre: array receiver — array is a VM primitive,
                    //not a class. MUST be checked BEFORE the int path because
                    //EvalDataType for `int[] arr` returns the element type
                    //(NK_Int32); array-ness is on the SnField via IsArrayType().
                    {
                        auto outerExprKind = member.Outer()->Kind();
                        if (outerExprKind == NK_MemberExpr
                            || outerExprKind == NK_IdentifierExpr)
                        {
                            auto& outerFieldExpr = static_cast<SnFieldExpr&>(
                                *member.Outer());
                            auto* outerField = outerFieldExpr.Field();
                            if (outerField && outerField->IsArrayType())
                            {
                                EmitExpression(*member.Outer(), emitter, resultOffset);
                                emitter.Emit(OpCode::OP_Array_to_str);
                                emitter.Emit(OpCode::OP_Assign);
                                emitter.EmitUint16(resultOffset);
                                return;
                            }
                        }
                    }
                    //String.toString() — identity, no opcode.
                    if (outerKind == NK_String)
                    {
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        return;
                    }
                    //Enum receiver — need to find the SnEnumDecl for enumDefIdx.
                    //Two sub-cases:
                    // (a) outerKind == NK_EnumDecl (typed enum variable)
                    // (b) outerKind == NK_Int32 but outer's Field() is NK_EnumMember
                    //     (enum literal like Color.Green — EvalDataType is NK_Int32)
                    if (outerKind == NK_EnumDecl)
                    {
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        auto* enumDecl = static_cast<SnEnumDecl*>(outerType);
                        auto it = m_enumIndexMap.find(enumDecl);
                        if (it != m_enumIndexMap.end())
                        {
                            emitter.Emit(OpCode::OP_Enum_to_str);
                            emitter.EmitUint16(static_cast<uint16_t>(it->second));
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(resultOffset);
                        }
                        return;
                    }
                    if (outerKind == NK_Int32)
                    {
                        //Check if outer is an enum literal (Field() == NK_EnumMember)
                        auto outerExprKind = member.Outer()->Kind();
                        if (outerExprKind == NK_MemberExpr
                            || outerExprKind == NK_IdentifierExpr)
                        {
                            auto& outerFieldExpr = static_cast<SnFieldExpr&>(
                                *member.Outer());
                            auto* outerField = outerFieldExpr.Field();
                            if (outerField
                                && outerField->Kind() == NK_EnumMember)
                            {
                                EmitExpression(*member.Outer(), emitter, resultOffset);
                                auto* enumDecl = static_cast<SnEnumDecl*>(
                                    outerField->Parent());
                                auto it = m_enumIndexMap.find(enumDecl);
                                if (it != m_enumIndexMap.end())
                                {
                                    emitter.Emit(OpCode::OP_Enum_to_str);
                                    emitter.EmitUint16(static_cast<uint16_t>(it->second));
                                    emitter.Emit(OpCode::OP_Assign);
                                    emitter.EmitUint16(resultOffset);
                                }
                                return;
                            }
                        }
                        //Plain int receiver
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        EmitPResultRefresh(emitter, resultOffset);
                        emitter.Emit(OpCode::OP_Int32_to_str);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        return;
                    }
                    if (outerKind == NK_Float)
                    {
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        EmitPResultRefresh(emitter, resultOffset);
                        emitter.Emit(OpCode::OP_Float_to_str);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        return;
                    }
                    //Fall through to struct/class/interface handling below
                    //for class receivers (which have their own toString path
                    //via OP_CallMethod).
                }
            }
        }
        //Array.length builtin property (e.g. arr.length).
        //The outer expression refers to an array-typed field/local; check
        //its IsArrayType() flag (forwarded from the type's SnNameExpr).
        //MUST be checked BEFORE the struct/class dispatch below: for
        //struct-element arrays (`Point[] b`), EvalDataType returns the
        //ELEMENT type (NK_StructDecl), so the struct branch would match
        //first, fail FindFieldOffset("length"), and silently emit only
        //the receiver. Same dispatch-order hazard as the array toString
        //path above.
        {
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                SnField* outerField = nullptr;
                if (member.Outer()->Kind() == NK_IdentifierExpr)
                    outerField = static_cast<SnIdentifierExpr*>(
                        member.Outer())->Field();
                if (outerField && outerField->IsArrayType()
                    && fieldName == "length")
                {
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    emitter.Emit(OpCode::OP_NullCheck);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ArrayLength);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    return;
                }
            }
        }
        //Struct field access (e.g. pt.x, pt.inner.x)
        auto* outerType = member.Outer()->EvalDataType();
        if (outerType && outerType->Kind() == NK_StructDecl) {
            auto* structDecl = static_cast<SnStructDecl*>(outerType);
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Find the field's offset within the struct
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int off = FindFieldOffset(*structDecl, fieldName);
                if (off < 0) return; //should not happen after type resolution
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
            }
            return;
        }
        //Class field access (e.g. obj.x, obj.y)
        if (outerType && outerType->Kind() == NK_ClassDecl) {
            auto* classDecl = static_cast<SnClassDecl*>(outerType);
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Null check
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int off = FindClassFieldOffset(*classDecl, fieldName);
                if (off < 0) return;
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
            } else if (inner && inner->Kind() == NK_InvokeExpr) {
                //Class method call
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);

                //Phase 8e-4: per-method boxing plan for built-in generic
                //classes (List<T>, Dict<K,V>). For primitive type arguments,
                //the corresponding param slots must be boxed (OP_Box) before
                //OP_CallMethod, and Get()/index returns must be unboxed
                //(OP_Unbox) after. argPlans maps paramIdx → {tag,needsBox};
                //returnsBoxed/returnTag handle the return-side unbox.
                //For class/struct type args, BoxingTagFor returns 0 → no
                //boxing; values pass as heap idxs directly.
                std::map<uint16_t, ArgBoxPlan> argPlans;
                bool returnsBoxed = false;
                uint8_t returnTag = 0;
                if (classDecl->IsGenericInstantiation()) {
                    const auto& typeArgs = classDecl->GenericTypeArgs();
                    const auto& methodName = invoke.CalleeName();
                    const auto& baseName = classDecl->BaseName();
                    if (baseName == "List") {
                        auto t = BoxingTagFor(
                            typeArgs.empty() ? nullptr : typeArgs[0]);
                        if (t.isPrimitive) {
                            //List<T> method signatures:
                            //  Add(T)         — T at paramIdx 1
                            //  Set(int, T)    — T at paramIdx 2
                            //  IndexOf(T)     — T at paramIdx 1
                            //  Contains(T)    — T at paramIdx 1
                            //  Get(int) → T   — return unboxed
                            if (methodName == "add"
                                || methodName == "indexOf"
                                || methodName == "contains") {
                                argPlans[1] = {t.tag, true};
                            } else if (methodName == "set") {
                                argPlans[2] = {t.tag, true};
                            } else if (methodName == "get") {
                                returnsBoxed = true; returnTag = t.tag;
                            }
                        }
                    } else if (baseName == "Dict") {
                        auto k = BoxingTagFor(
                            typeArgs.empty() ? nullptr : typeArgs[0]);
                        auto v = (typeArgs.size() > 1)
                            ? BoxingTagFor(typeArgs[1]) : BoxingTagResult{0, false};
                        if (methodName == "set") {
                            if (k.isPrimitive) argPlans[1] = {k.tag, true};
                            if (v.isPrimitive) argPlans[2] = {v.tag, true};
                        } else if (methodName == "get") {
                            if (k.isPrimitive) argPlans[1] = {k.tag, true};
                            if (v.isPrimitive) { returnsBoxed = true; returnTag = v.tag; }
                        } else if (methodName == "containsKey"
                            || methodName == "remove") {
                            if (k.isPrimitive) argPlans[1] = {k.tag, true};
                        }
                    }
                }

                auto* callee = invoke.Callee();
                //Phase 9e: out-argument spills collected by EmitCallArgs.
                std::vector<OutSpill> outSpills;
                //Phase 9c: evaluate args via the shared binding-aware
                //helper. Slot 0 is reserved for `this` (slotBase=1).
                //Boxing plans (for built-in generic class methods like
                //List<int>.add) are applied per-arg inside EmitCallArgs.
                //thisSlot=resultOffset: default-param expressions that
                //reference `this.field` resolve `this` to resultOffset
                //(the slot holding the receiver object). This avoids the
                //callParamBase nested-call clobber bug — copying `this`
                //to callParamBase[0] before emitting args would be
                //overwritten by any nested call inside the args.
                EmitCallArgs(invoke, callee, emitter, /*slotBase=*/1,
                             &argPlans, /*thisSlot=*/resultOffset,
                             &outSpills);
                //EmitCallArgs now copies `this` to claim[0] and bulk-copies
                //to callParamBase[0] — no separate this-copy needed here.
                bool isVirtual = callee && callee->ContainFlags(NF_Virtual);
                if (isVirtual) {
                    //Virtual method dispatch — name-based lookup at runtime
                    //(like EN's I_Base_CallVirtualFunc + FindFunctionChecked)
                    //Phase 9e: out args are resolver-rejected on virtual
                    //methods; spills here would be silently lost.
                    if (!outSpills.empty())
                        throw std::runtime_error(
                            "NLang backend: out argument on virtual call");
                    uint16_t nameIdx = AddStringConstant(callee->Name());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                } else if (callee) {
                    //Non-virtual (final) method — direct call by function index
                    //(like EN's I_Base_CallFinalFunc + NFunction*)
                    auto it = m_funcIndexMap.find(callee);
                    if (it != m_funcIndexMap.end()) {
                        if (!outSpills.empty()) {
                            emitter.Emit(OpCode::OP_CallMethodDirectOut);
                            emitter.EmitUint16(static_cast<uint16_t>(it->second));
                            emitter.EmitUint16(m_currFunc->callParamBase);
                            emitter.EmitInt32(static_cast<int32_t>(
                                BuildOutMask(outSpills)));
                        } else {
                            emitter.Emit(OpCode::OP_CallMethodDirect);
                            emitter.EmitUint16(static_cast<uint16_t>(it->second));
                            emitter.EmitUint16(m_currFunc->callParamBase);
                        }
                    }
                } else {
                    //Phase 8e-1: no AST callee. Covers two cases:
                    //  (a) Builtin class methods (ByteStream/FileStream/List/Dict) —
                    //      the synthesized SnClassDecl has no method members.
                    //  (b) User-class calls to inherited Object protocol methods
                    //      (Equals/GetHashCode) — no AST override exists, so
                    //      callee is null. The VM walks superClassIdx to find
                    //      Object's intrinsic stub and short-circuits to
                    //      ExecuteIntrinsic.
                    //Both paths dispatch by name via OP_CallMethod. Phase 9e:
                    //out args cannot bind here (no formals) — internal error.
                    if (!outSpills.empty())
                        throw std::runtime_error(
                            "NLang backend: out argument on builtin call");
                    uint16_t nameIdx = AddStringConstant(invoke.CalleeName());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                }
                //Phase 8e-3/8e-4: unbox Get() return for primitive T/V.
                if (returnsBoxed) {
                    emitter.Emit(OpCode::OP_Unbox);
                    emitter.EmitByte(returnTag);
                }
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                //Phase 9e: out spills LAST — OP_VarLocal clobbers pResult,
                //so the return value (and any unbox) must complete first.
                if (!outSpills.empty())
                    EmitOutSpills(outSpills, emitter);
                emitter.Emit(OpCode::OP_ParaEnd);
            }
            return;
        }
        //Interface method call (e.g. p.Print()).
        //At runtime, p holds a heap reference whose actual class is unknown
        //at compile time. Always dispatch virtually by method name — the
        //VM walks the runtime class's methodIndices via superClassIdx.
        if (outerType && outerType->Kind() == NK_InterfaceDecl) {
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Null check
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_InvokeExpr) {
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                //Phase 9c: binding-aware arg emit; slot 0 reserved for `this`.
                //thisSlot=resultOffset so default-param `this.field` reads
                //the receiver from the outer-expression result slot.
                std::vector<OutSpill> outSpills;
                EmitCallArgs(invoke, invoke.Callee(), emitter, /*slotBase=*/1,
                             /*pArgPlans=*/nullptr, /*thisSlot=*/resultOffset,
                             &outSpills);
                //EmitCallArgs now copies `this` to claim[0] and bulk-copies
                //to callParamBase[0] — no separate this-copy needed here.
                //Phase 9e: interface dispatch is name-based (virtual) —
                //out args are resolver-rejected; spills here are internal.
                if (!outSpills.empty())
                    throw std::runtime_error(
                        "NLang backend: out argument on interface call");
                //Always virtual dispatch by name.
                auto* callee = invoke.Callee();
                if (callee) {
                    uint16_t nameIdx = AddStringConstant(callee->Name());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                }
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_ParaEnd);
            }
            return;
        }
        auto* inner = member.Inner();
        //Array element field access: arr[i].field (struct element)
        if (member.Outer()->Kind() == NK_SubscriptExpr) {
            auto& sub = static_cast<SnSubscriptExpr&>(*member.Outer());
            auto* subElemType = sub.EvalDataType();
            if (subElemType && subElemType->Kind() == NK_StructDecl) {
                auto* structDecl = static_cast<SnStructDecl*>(subElemType);
                //Evaluate array ref + index, load element heap index
                EmitExpression(sub, emitter, resultOffset);
                if (inner && inner->Kind() == NK_IdentifierExpr) {
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    int off = FindFieldOffset(*structDecl, fieldName);
                    if (off < 0) return;
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                }
                return;
            }
            //Class element field access: arr[i].field (class element)
            if (subElemType && subElemType->Kind() == NK_ClassDecl) {
                auto* classDecl = static_cast<SnClassDecl*>(subElemType);
                EmitExpression(sub, emitter, resultOffset);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(resultOffset);
                if (inner && inner->Kind() == NK_IdentifierExpr) {
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    int off = FindClassFieldOffset(*classDecl, fieldName);
                    if (off < 0) return;
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                }
                return;
            }
        }
        //String builtin methods: s.length(), s.GetHashCode(), s.Equals(other)
        //Phase 8e-1: GetHashCode/Equals dispatch to intrinsics for value semantics.
        //Strings are primitives (pool idx), not classes — these intrinsics are the
        //only way to invoke the protocol on them. Parallel to s.length() shortcut.
        if (inner && inner->Kind() == NK_InvokeExpr) {
            auto& invoke = static_cast<SnInvokeExpr&>(*inner);
            if (outerType && outerType->Kind() == NK_String)
            {
                const auto& methName = invoke.CalleeName();
                if (methName == "length")
                {
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    emitter.Emit(OpCode::OP_StrLen);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    return;
                }
                if (methName == "getHashCode")
                {
                    //Phase 9c follow-up: route through evalArea claim so the
                    //receiver expression (if it contains nested calls) does
                    //not get clobbered by inner bulk-copies to callParamBase.
                    EvalAreaClaim claim(*this, 1);
                    uint16_t claimBase = claim.base();
                    EmitExpression(*member.Outer(), emitter, claimBase);
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(claimBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_CallIntrinsic);
                    emitter.EmitUint16(INTR_String_GetHashCode);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }
                if (methName == "equals")
                {
                    //Phase 9c follow-up: route through evalArea claim so
                    //nested calls in args don't clobber receiver/args in
                    //callParamBase.
                    uint16_t argCount = 0;
                    for (auto& p : invoke.Params()) ++argCount;
                    uint16_t n = static_cast<uint16_t>(1 + argCount);
                    EvalAreaClaim claim(*this, n);
                    uint16_t claimBase = claim.base();
                    EmitExpression(*member.Outer(), emitter, claimBase);
                    uint16_t paramIdx = 1;
                    for (auto& param : invoke.Params()) {
                        EmitExpression(param, emitter,
                            claimBase + paramIdx * VALUE_SIZE);
                        ++paramIdx;
                    }
                    for (uint16_t i = 0; i < n; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallIntrinsic);
                    emitter.EmitUint16(INTR_String_Equals);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }
            }
            EmitExpression(*static_cast<SnExpression*>(inner), emitter, resultOffset);
        }
        return;
    }

    // Name expression - delegate
    if (kind == NK_NameExpr) {
        auto& nameExpr = static_cast<SnNameExpr&>(expr);
        if (nameExpr.Expr()) {
            EmitExpression(*nameExpr.Expr(), emitter, resultOffset);
        }
        return;
    }

    // New expression - object instantiation
    if (kind == NK_NewExpr) {
        auto& newExpr = static_cast<SnNewExpr&>(expr);
        auto* pClassDecl = newExpr.ClassDecl();
        if (!pClassDecl) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        //Phase 8e-3: generic instantiations (List<int>) share one CompiledClass
        //named "List" at runtime (erasure). BaseName() returns the unqualified
        //"List" for generic instances, or the full name for ordinary classes.
        const std::string& className = pClassDecl->BaseName();
        int classIdx = m_compiledModule.FindClass(className);
        if (classIdx < 0) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
        //Phase 9c follow-up: route ctor args through evalArea claim so
        //nested calls inside ctor args don't clobber each other (parallel
        //to EmitCallArgs). claim layout: [0]=this, [1..N]=args.
        uint16_t allocSlot = resultOffset;
        if (ctorIdx != 0xFFFF) {
            //Count actual args (skip NameExpr which are named-arg markers).
            size_t argCount = 0;
            for (auto& param : newExpr.Args()) {
                if (param.Kind() != NK_NameExpr) ++argCount;
            }
            uint16_t n = static_cast<uint16_t>(1 + argCount);  //this + args

            EvalAreaClaim claim(*this, n);
            uint16_t claimBase = claim.base();

            //Emit args to claim[1..N]. Nested calls in args claim deeper
            //slices — they bulk-copy to callParamBase but that's fine;
            //our this/args live in evalArea, not callParamBase.
            uint16_t paramIdx = 1;
            for (auto& param : newExpr.Args()) {
                if (param.Kind() == NK_NameExpr) continue;
                uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
                EmitExpression(param, emitter, paramOffset);
                ++paramIdx;
            }
            //Allocate to resultOffset (no conflict with args — they're in evalArea).
            allocSlot = resultOffset;

            emitter.Emit(OpCode::OP_New);
            emitter.EmitUint16(allocSlot);
            emitter.EmitUint16(static_cast<uint16_t>(classIdx));

            //Copy this (new object heap index) to claim[0].
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(allocSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(claimBase);

            //Bulk-copy evalArea claim → callParamBase just before the call.
            for (uint16_t i = 0; i < n; ++i) {
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
            }
            emitter.Emit(OpCode::OP_CallMethodDirect);
            emitter.EmitUint16(ctorIdx);
            emitter.EmitUint16(m_currFunc->callParamBase);
            emitter.Emit(OpCode::OP_ParaEnd);
            //claim releases on scope exit.
            return;
        }

        emitter.Emit(OpCode::OP_New);
        emitter.EmitUint16(allocSlot);
        emitter.EmitUint16(static_cast<uint16_t>(classIdx));
        return;
    }

    // This expression - reads the implicit first parameter
    if (kind == NK_ThisExpr) {
        //Inside a method-call default-param expression, `this` resolves
        //to the caller-side callParamBase slot holding the receiver.
        //Otherwise (inside a method body), `this` is local 0.
        auto thisOverride = LookupThisOverride();
        uint16_t thisSlot = thisOverride.first ? thisOverride.second : 0;
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(thisSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return;
    }

    // New array expression: new T[size]
    if (kind == NK_NewArrayExpr) {
        auto& newArr = static_cast<SnNewArrayExpr&>(expr);
        //Register the array type (idempotent)
        uint16_t arrayTypeIdx = RegisterArrayType(
            newArr.ElementType()->Field());
        //Size scratch must avoid resultOffset. When the dst is a temp
        //(e.g. the RHS of a member assignment, emitted into tempSlot2
        //while the receiver lives in tempSlot), hardcoding tempSlot
        //here would clobber the receiver.
        uint16_t sizeSlot = PickTempSlot(resultOffset);
        EmitExpression(*newArr.Size(), emitter, sizeSlot);
        emitter.Emit(OpCode::OP_AllocArray);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(arrayTypeIdx);
        emitter.EmitUint16(sizeSlot);
        return;
    }

    //Phase 8e-6: collection initializer `[...]` / `new T{...}`.
    //Dispatches on resolved EvalDataType:
    //  - Array (target->IsArrayType()): OP_AllocArray + per-element OP_StoreElement
    //  - List<T> (SnClassDecl, BaseName "List"): OP_New + per-entry OP_CallMethod "add" with boxing
    //  - Dict/Struct/Class: handled in Phase D (falls through to assert for now).
    if (kind == NK_InitListExpr) {
        auto& initList = static_cast<SnInitListExpr&>(expr);
        SnField* pTarget = initList.EvalDataType();
        if (!pTarget) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }

        //---- Array form: target is T[] ----
        //For `int[] arr = [..]`, the resolver set TargetIsArray=true and
        //stored the element type field in EvalDataType (since NLang has no
        //standalone array-type object — array-ness is a flag on variables).
        if (initList.TargetIsArray()) {
            SnField* pElemField = pTarget;
            uint16_t arrayTypeIdx = RegisterArrayType(pElemField);
            int32_t n = static_cast<int32_t>(initList.Entries().size());

            //Alloc array with size = n.
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(n);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->tempSlot);
            emitter.Emit(OpCode::OP_AllocArray);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(arrayTypeIdx);
            emitter.EmitUint16(m_currFunc->tempSlot);

            //Pick value slot distinct from resultOffset (handles nesting).
            uint16_t valueSlot = PickTempSlot(resultOffset);
            //Store each entry: arr[i] = entries[i].pValue.
            for (int32_t i = 0; i < n; ++i) {
                auto& entry = initList.Entries()[i];
                if (!entry.pValue) continue;
                EmitExpression(*entry.pValue, emitter, valueSlot);
                //Index constant to callParamBase (avoids tempSlot/valueSlot).
                emitter.Emit(OpCode::OP_ConstInt32);
                emitter.EmitInt32(i);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.Emit(OpCode::OP_StoreElement);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.EmitUint16(valueSlot);
            }
            return;
        }

        //---- List<T> form ----
        //pTarget is the resolved SnClassDecl for List<T> (generic
        //instantiation). Erasure: runtime class name is "List".
        if (pTarget->Kind() == NK_ClassDecl) {
            auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
            const std::string& baseName = pClassDecl->BaseName();
            if (baseName == "List") {
                int classIdx = m_compiledModule.FindClass("List");
                if (classIdx < 0) {
                    emitter.Emit(OpCode::OP_ConstZero);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    return;
                }
                //Allocate List instance.
                emitter.Emit(OpCode::OP_New);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(classIdx));
                //Call no-arg ctor if present.
                uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
                if (ctorIdx != 0xFFFF) {
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_CallMethodDirect);
                    emitter.EmitUint16(ctorIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                //Boxing plan for primitive T.
                const auto& typeArgs = pClassDecl->GenericTypeArgs();
                auto t = BoxingTagFor(
                    typeArgs.empty() ? nullptr : typeArgs[0]);
                uint16_t addNameIdx = AddStringConstant("add");
                //Phase 10 audit C2: stage entry values in evalArea via
                //EvalAreaClaim, immune to nested-call bulk-copies into
                //callParamBase. Pre-fix, the value was emitted straight to
                //callParamBase+1; a binary entry like `1 + helper(0,0,5)`
                //parked the left operand there and the call's arg bulk-copy
                //overwrote it (stored 5 instead of 6). Mirrors the Dict form
                //below (Phase 9c follow-up). ExprPeakDepth's InitListExpr
                //case must track the matching claimSize=2.
                for (auto& entry : initList.Entries()) {
                    if (!entry.pValue) continue;
                    EvalAreaClaim claim(*this, 2);  // this, value
                    uint16_t claimBase = claim.base();
                    uint16_t valOff = claimBase + 1 * VALUE_SIZE;
                    EmitExpression(*entry.pValue, emitter, valOff);
                    if (t.isPrimitive) {
                        EmitPResultRefresh(emitter, valOff);
                        emitter.Emit(OpCode::OP_Box);
                        emitter.EmitByte(t.tag);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(valOff);
                    }
                    //this = resultOffset → claim[0]
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    //Bulk-copy claim → callParamBase
                    for (uint16_t i = 0; i < 2; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(addNameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                return;
            }
            if (baseName == "Dict") {
                //---- Dict<K,V> form ----
                int classIdx = m_compiledModule.FindClass("Dict");
                if (classIdx < 0) {
                    emitter.Emit(OpCode::OP_ConstZero);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    return;
                }
                emitter.Emit(OpCode::OP_New);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(classIdx));
                uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
                if (ctorIdx != 0xFFFF) {
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_CallMethodDirect);
                    emitter.EmitUint16(ctorIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                const auto& typeArgs = pClassDecl->GenericTypeArgs();
                auto kBox = BoxingTagFor(
                    typeArgs.empty() ? nullptr : typeArgs[0]);
                auto vBox = (typeArgs.size() > 1)
                    ? BoxingTagFor(typeArgs[1]) : BoxingTagResult{0, false};
                uint16_t setNameIdx = AddStringConstant("set");
                //Phase 9c follow-up: route through EvalAreaClaim so the key
                //and value live in evalArea, immune to nested-call bulk-copies
                //to callParamBase. Pre-fix, a value like `helper(5,3)` would
                //bulk-copy to callParamBase[0..2], clobbering the key at [1].
                for (auto& entry : initList.Entries()) {
                    if (!entry.pValue) continue;
                    //Key: dict requires String key form. Identifier keys are
                    //accepted for struct init only — for dict they would be
                    //a resolver error. Value-only entries are also invalid.
                    if (entry.keyKind != InitEntry::KeyKind::String
                        && entry.keyKind != InitEntry::KeyKind::Identifier) {
                        continue;
                    }
                    EvalAreaClaim claim(*this, 3);  // this, key, value
                    uint16_t claimBase = claim.base();
                    uint16_t keyOff = claimBase + 1 * VALUE_SIZE;
                    uint16_t valOff = claimBase + 2 * VALUE_SIZE;
                    uint16_t keyPoolIdx = AddStringConstant(entry.keyStr);
                    emitter.Emit(OpCode::OP_ConstString);
                    emitter.EmitUint16(keyPoolIdx);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(keyOff);
                    if (kBox.isPrimitive) {
                        emitter.Emit(OpCode::OP_Box);
                        emitter.EmitByte(kBox.tag);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(keyOff);
                    }
                    //Value
                    EmitExpression(*entry.pValue, emitter, valOff);
                    if (vBox.isPrimitive) {
                        EmitPResultRefresh(emitter, valOff);
                        emitter.Emit(OpCode::OP_Box);
                        emitter.EmitByte(vBox.tag);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(valOff);
                    }
                    //this = resultOffset → claim[0]
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    //Bulk-copy claim → callParamBase
                    for (uint16_t i = 0; i < 3; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(setNameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                return;
            }
            //---- User class init form: new ClassName{field1:v1, ...} ----
            //Allocate via OP_New, then per-field OP_StoreField. No-arg ctor
            //is invoked if present.
            {
                const std::string& className = pClassDecl->BaseName().empty()
                    ? pClassDecl->Name() : pClassDecl->BaseName();
                int classIdx = m_compiledModule.FindClass(className);
                if (classIdx < 0) {
                    emitter.Emit(OpCode::OP_ConstZero);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    return;
                }
                emitter.Emit(OpCode::OP_New);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(classIdx));
                uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
                if (ctorIdx != 0xFFFF) {
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_CallMethodDirect);
                    emitter.EmitUint16(ctorIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                //Per-field store.
                uint16_t valueSlot = PickTempSlot(resultOffset);
                for (auto& entry : initList.Entries()) {
                    if (entry.keyKind != InitEntry::KeyKind::Identifier)
                        continue;
                    if (!entry.pValue) continue;
                    int off = FindClassFieldOffset(*pClassDecl, entry.keyStr);
                    if (off < 0) continue;
                    EmitExpression(*entry.pValue, emitter, valueSlot);
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                    emitter.EmitUint16(valueSlot);
                }
                return;
            }
        }

        //---- Struct init form: new StructName{field1:v1, ...} ----
        //Allocate a fresh struct via OP_AllocStruct, then per-field OP_StoreField
        //using the struct-specific FindFieldOffset (linear scan, no inheritance).
        if (pTarget->Kind() == NK_StructDecl) {
            auto* pStructDecl = static_cast<SnStructDecl*>(pTarget);
            const std::string& structName = pStructDecl->Name();
            int structIdx = m_compiledModule.FindStruct(structName);
            if (structIdx < 0) {
                emitter.Emit(OpCode::OP_ConstZero);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                return;
            }
            auto& cs = m_compiledModule.structs[structIdx];
            emitter.Emit(OpCode::OP_AllocStruct);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(static_cast<uint16_t>(structIdx));
            emitter.EmitUint16(cs.fieldCount);
            //Per-field store. FindFieldOffset returns byte offset within the
            //struct's data area (no classIdx slot like classes have).
            //Identifier keys address by name; value-only entries (the
            //`{ v1, v2 }` form) fill fields in DECLARATION ORDER — skipping
            //them silently zeroed every field (Phase 8e-6 declared
            //declaration-order support; the arg-position probe
            //`sum(new Point{ 3 })` exposed it).
            uint16_t valueSlot = PickTempSlot(resultOffset);
            size_t ordinal = 0;
            for (auto& entry : initList.Entries()) {
                if (!entry.pValue) continue;
                int off = -1;
                if (entry.keyKind == InitEntry::KeyKind::Identifier) {
                    off = FindFieldOffset(*pStructDecl, entry.keyStr);
                } else if (ordinal < pStructDecl->Members().size()) {
                    off = static_cast<int>(ordinal * VALUE_SIZE);
                }
                ++ordinal;
                if (off < 0) continue;
                EmitExpression(*entry.pValue, emitter, valueSlot);
                emitter.Emit(OpCode::OP_StoreField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
                emitter.EmitUint16(valueSlot);
            }
            return;
        }

        //Unknown target kind — no codegen path yet.
        assert(false && "NK_InitListExpr: unsupported target kind");
        return;
    }

    // Subscript expression: arr[index]
    if (kind == NK_SubscriptExpr) {
        auto& sub = static_cast<SnSubscriptExpr&>(expr);
        //List<T>/Dict<K,V> subscript sugar: li[i] == li.get(i),
        //d[k] == d.get(k). Dispatch on the base's resolved type being a
        //generic instantiation (arrays take the OP_LoadElement path below).
        //Emits through EvalAreaClaim so nested calls (e.g. foo(li[j]))
        //cannot clobber an outer call's callParamBase slice. Unboxes the
        //get() return for primitive T/V, mirroring the foreach element
        //load and the member-call boxing plans.
        if (IsContainerSubscript(*sub.Array())) {
            auto* pGenClass = static_cast<SnClassDecl*>(
                sub.Array()->EvalDataType());
            const auto& baseName = pGenClass->BaseName();
            const auto& typeArgs = pGenClass->GenericTypeArgs();
            bool isList = (baseName == "List" && !typeArgs.empty());
            bool isDict = (baseName == "Dict" && typeArgs.size() > 1);
            {
                        //Dict keys box when primitive; List's index is int.
                        auto keyBox = isDict
                            ? BoxingTagFor(typeArgs[0])
                            : BoxingTagResult{0, false};
                        SnField* pElem = isList ? typeArgs[0]
                            : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
                        auto valBox = BoxingTagFor(pElem);
                        EvalAreaClaim claim(*this, 2);
                        uint16_t claimBase = claim.base();
                        //Receiver → claim[0] directly (round-4: NOT via
                        //resultOffset — a self-referential read `i = li[i]`
                        //would overwrite the index's source slot with the
                        //List handle before the index is emitted). Same
                        //shape as the array path below; resultOffset is
                        //written only by the final get() store.
                        EmitExpression(*sub.Array(), emitter, claimBase);
                        emitter.Emit(OpCode::OP_NullCheck);
                        emitter.EmitUint16(claimBase);
                        //arg0 = index (box primitive Dict keys).
                        uint16_t keyOffset = claimBase + VALUE_SIZE;
                        EmitExpression(*sub.Index(), emitter, keyOffset);
                        if (keyBox.isPrimitive) {
                            EmitPResultRefresh(emitter, keyOffset);
                            emitter.Emit(OpCode::OP_Box);
                            emitter.EmitByte(keyBox.tag);
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(keyOffset);
                        }
                        //Bulk-copy claim → callParamBase (raw 4-byte moves
                        //preserve tagged representations).
                        for (uint16_t i = 0; i < 2; ++i) {
                            emitter.Emit(OpCode::OP_VarLocal);
                            emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(
                                m_currFunc->callParamBase + i * VALUE_SIZE);
                        }
                        uint16_t nameIdx = AddStringConstant("get");
                        emitter.Emit(OpCode::OP_CallMethod);
                        emitter.EmitUint16(nameIdx);
                        emitter.EmitUint16(m_currFunc->callParamBase);
                        if (valBox.isPrimitive) {
                            emitter.Emit(OpCode::OP_Unbox);
                            emitter.EmitByte(valBox.tag);
                        }
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        emitter.Emit(OpCode::OP_ParaEnd);
                        return;
                    }
        }
        //Phase 10 audit round-3: park receiver AND index in an exclusive
        //EvalAreaClaim(2), mirroring the container get() shape. The old
        //chain (receiver → resultOffset, index → PickTempSlot(resultOffset))
        //broke at nesting depth 5 — PickTempSlot wraps tempSlot4 back to
        //tempSlot, so `a[b[c[d[e[0]]]]]` clobbered an outer parked value —
        //and parked the receiver in resultOffset, clobberable whenever a
        //non-temp exclude slot fell through to the same temp. Claim slots
        //stack per nesting level, so read depth is now unbounded. Same
        //instruction count as the old shape; only the slot numbers change.
        EvalAreaClaim claim(*this, 2);
        uint16_t claimBase = claim.base();
        EmitExpression(*sub.Array(), emitter, claimBase);
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(claimBase);
        uint16_t indexSlot = claimBase + VALUE_SIZE;
        EmitExpression(*sub.Index(), emitter, indexSlot);
        emitter.Emit(OpCode::OP_LoadElement);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(claimBase);
        emitter.EmitUint16(indexSlot);
        //Phase 9d-3: no defensive CopyStruct for struct element types here.
        //Value copies belong at assignment/store boundaries (AssignStmt and
        //SubscriptAssign both emit their own OP_CopyStruct). Copying on read
        //broke write-through receivers — `arr[i].f = v` stored into a
        //discarded copy — and caused a redundant double copy for
        //`Point p = arr[i]`.
        return;
    }

    // Binary/unary operator expression
    if (kind == NK_BinaryExpr) {
        auto& bin = static_cast<SnBinaryExpr&>(expr);
        auto op = bin.Op();

        if (op == SnBinaryExpr::OP_Neg || op == SnBinaryExpr::OP_LogicalNot) {
            //Unary: evaluate operand to resultOffset, then negate in-place
            EmitExpression(*bin.Left(), emitter, resultOffset);
            if (op == SnBinaryExpr::OP_Neg) {
                auto* evalType = bin.Left()->EvalDataType();
                if (evalType && evalType->Kind() == NK_Float)
                    emitter.Emit(OpCode::OP_Neg_f32);
                else
                    emitter.Emit(OpCode::OP_Neg_i32);
                emitter.EmitUint16(resultOffset);
            } else {
                emitter.Emit(OpCode::OP_LogicalNot);
                emitter.EmitUint16(resultOffset);
            }
            return;
        }

        //Binary: evaluate left to resultOffset, right to a different slot,
        //then apply op. rightSlot must differ from resultOffset to avoid the
        //right operand overwriting the left before the binary op executes.
        //
        //Phase 8e-8: iterate sn.Children() instead of Left()/Right() because
        //resolver may wrap each operand in SnCastExpr for symmetric promotion,
        //after which m_pLeft/m_pRight are stale (still point to the original
        //expression inside the cast). Children()[0]/[1] always reflect the
        //post-wrap tree. Dispatch on Children()[0]'s EvalDataType — for
        //arithmetic with promotion this is the cast target (= T_result); for
        //comparison (no wrap) this is the operand type, which selects the
        //i32/f32/str variant (e.g. OP_Eq_str for string==string even though
        //bin.EvalDataType() is Int32 for all comparisons).
        //Phase 10 audit round-4: park the right operand in a per-level
        //EvalAreaClaim instead of the PickTempSlot chain — the 4-slot chain
        //wraps tempSlot4 → tempSlot at nesting depth 5, silently clobbering
        //the outer parked left operand (`1+(2+(3+(4+(5+6))))` evaluated to
        //25). Claim slots stack per nesting level, so right-nesting depth
        //is unbounded, and claims never collide with resultOffset (claims
        //start at the evalArea cursor; resultOffset always sits below it).
        EvalAreaClaim rightClaim(*this, 1);
        uint16_t rightSlot = rightClaim.base();
        auto& binChildren = bin.Children();
        auto binIt = binChildren.begin();
        auto& leftChild = static_cast<SnExpression&>(*binIt);
        EmitExpression(leftChild, emitter, resultOffset);
        ++binIt;
        EmitExpression(static_cast<SnExpression&>(*binIt), emitter, rightSlot);

        auto* evalType = leftChild.EvalDataType();
        bool isFloat = evalType && evalType->Kind() == NK_Float;
        bool isString = evalType && evalType->Kind() == NK_String;

        switch (op) {
        case SnBinaryExpr::OP_Add:
            if (isString)
                emitter.Emit(OpCode::OP_Concat_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_Add_f32 : OpCode::OP_Add_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Sub:
            emitter.Emit(isFloat ? OpCode::OP_Sub_f32 : OpCode::OP_Sub_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Mul:
            emitter.Emit(isFloat ? OpCode::OP_Mul_f32 : OpCode::OP_Mul_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Div:
            emitter.Emit(isFloat ? OpCode::OP_Div_f32 : OpCode::OP_Div_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Mod:
            emitter.Emit(OpCode::OP_Mod_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Less:
            emitter.Emit(isFloat ? OpCode::OP_Less_f32 : OpCode::OP_Less_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LessEqual:
            emitter.Emit(isFloat ? OpCode::OP_LessEqual_f32 : OpCode::OP_LessEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Greater:
            emitter.Emit(isFloat ? OpCode::OP_Greater_f32 : OpCode::OP_Greater_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_GreaterEqual:
            emitter.Emit(isFloat ? OpCode::OP_GreaterEqual_f32 : OpCode::OP_GreaterEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Equal:
            if (isString)
                emitter.Emit(OpCode::OP_Eq_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_Equal_f32 : OpCode::OP_Equal_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_NotEqual:
            if (isString)
                emitter.Emit(OpCode::OP_Ne_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_NotEqual_f32 : OpCode::OP_NotEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LogicalAnd:
            emitter.Emit(OpCode::OP_LogicalAnd);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LogicalOr:
            emitter.Emit(OpCode::OP_LogicalOr);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        default:
            throw std::runtime_error(
                "NLang backend: unsupported binary operator");
        }
        return;
    }

    // Fallback: write zero
    emitter.Emit(OpCode::OP_ConstZero);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

void VmBackend::EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter) {
    NodeKind kind = stmt.Kind();

    //Emit a line marker at every statement so the VM can produce source
    //location info in backtraces and runtime errors. Skipped for paragraphs
    //(they are containers, not statements with their own source location).
    if (kind != NK_Paragraph) {
        if (auto* pLoc = stmt.Location()) {
            if (auto* pScript = dynamic_cast<const ScriptLocation*>(pLoc)) {
                uint16_t line = static_cast<uint16_t>(
                    pScript->m_nStartLine & 0xFFFF);
                emitter.Emit(OpCode::OP_DebugInfo);
                emitter.EmitUint16(line);
            }
        }
    }

    if (kind == NK_ReturnStmt) {
        auto& ret = static_cast<SnReturnStmt&>(stmt);
        if (ret.Result()) {
            //Phase 9d-2: evaluate the return expression BEFORE running
            //finally copies (Java semantics: expr first, finally second).
            EmitExpression(*ret.Result(), emitter, m_currFunc->returnSlot);
        }
        //Phase 9d-2: a return leaving try/finally regions runs each
        //finally body inline (innermost first). returnSlot is a reserved
        //slot so the copies cannot clobber the result.
        for (size_t i = m_finallyStack.size(); i > 0; --i)
            EmitStatement(*m_finallyStack[i - 1], emitter);
        if (ret.Result()) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(m_currFunc->returnSlot);
        }
        emitter.Emit(OpCode::OP_Return);
        return;
    }

    if (kind == NK_InvokeStmt) {
        auto& invoke = static_cast<SnInvokeStmt&>(stmt);
        EmitExpression(*invoke.Expr(), emitter, m_currFunc->tempSlot);
        return;
    }

    if (kind == NK_Paragraph) {
        auto& para = static_cast<SnParagraph&>(stmt);
        for (auto& child : para.Statements())
            EmitStatement(child, emitter);
        return;
    }

    //Local variable declaration.
    //After the decomposition pattern, initializers are handled by
    //AssignStmts inserted after this declaration. We only allocate
    //the local variable slot here.
    //Reference: EN's LocalDeclStmt::Compile (SeStatements.cpp:226).
    if (kind == NK_LocalDeclStmt) {
        auto& decl = static_cast<SnLocalDeclStmt&>(stmt);
        //Detect array type via IsArrayType() on the type expression.
        bool isArrayType = decl.Type()->IsArrayType();
        uint8_t typeKind;
        SnField* evalType = nullptr;
        if (isArrayType) {
            typeKind = RTK_Array;
            //Walk through SnArrayTypeExpr to find the element type.
            auto* pCur = decl.Type();
            while (pCur->Kind() == NK_ArrayTypeExpr)
                pCur = static_cast<SnArrayTypeExpr*>(pCur)->ElementType();
            if (auto* pNameExpr = dynamic_cast<SnNameExpr*>(pCur))
                evalType = pNameExpr->Field();
        } else {
            evalType = decl.Type()->Field();
            typeKind = RuntimeTypeKind(evalType);
        }
        for (auto& local : decl.Decls()) {
            uint16_t offset = AllocLocal(local.name, VALUE_SIZE, typeKind, false);
            //For struct types, emit OP_AllocStruct to allocate on heap.
            if (typeKind == RTK_Struct && evalType) {
                int structIdx = m_compiledModule.FindStruct(evalType->Name());
                if (structIdx >= 0) {
                    auto& cs = m_compiledModule.structs[structIdx];
                    emitter.Emit(OpCode::OP_AllocStruct);
                    emitter.EmitUint16(offset);
                    emitter.EmitUint16(static_cast<uint16_t>(structIdx));
                    emitter.EmitUint16(cs.fieldCount);
                }
            }
            //Class and array types start as null (0) — no allocation needed.
        }
        return;
    }

    //Assignment statement.
    //Reference: EN's AssignStmt::Compile (SeStatements.cpp:313).
    if (kind == NK_AssignStmt) {
        auto& assign = static_cast<SnAssignStmt&>(stmt);
        if (assign.Left()->Kind() == NK_IdentifierExpr) {
            auto& idExpr = static_cast<SnIdentifierExpr&>(*assign.Left());
            auto* field = idExpr.Field();
            if (field) {
                auto target = ResolveBareIdentifier(field);
                if (target.kind == BareIdTarget::Local) {
                    uint16_t offset = target.localOffset;
                    auto* varType = field->EvalDataType();
                    //Array locals (`Point[] arr = ...`): EvalDataType returns
                    //the ELEMENT type, so the struct check below would
                    //deep-copy the whole array block as if it were one
                    //struct (corrupting heap kind metadata and GC tracing).
                    //Array assignment is a reference (heap idx) copy.
                    if (varType && RuntimeTypeKind(varType) == RTK_Struct
                        && !field->IsArrayType()) {
                        //Struct assignment: evaluate right to temp, then deep-copy.
                        EmitExpression(*assign.Right(), emitter, m_currFunc->tempSlot2);
                        int structIdx = m_compiledModule.FindStruct(varType->Name());
                        emitter.Emit(OpCode::OP_CopyStruct);
                        emitter.EmitUint16(offset);
                        emitter.EmitUint16(m_currFunc->tempSlot2);
                        emitter.EmitUint16(structIdx >= 0
                            ? static_cast<uint16_t>(structIdx) : 0);
                    } else {
                        //Phase 10 audit round-7: stage the RHS in an
                        //EvalAreaClaim(1) and copy to the destination —
                        //emitting straight into `offset` let a binary's
                        //LEFT operand write the destination before the
                        //RIGHT evaluated (`k = li[0] + li[k]` read the
                        //clobbered slot: OOB or a silently wrong index).
                        //JLS 15.26.1: the destination is written only
                        //after the whole RHS has been evaluated.
                        EvalAreaClaim claim(*this, 1);
                        uint16_t valueSlot = claim.base();
                        EmitExpression(*assign.Right(), emitter, valueSlot);
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(valueSlot);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(offset);
                    }
                } else if (target.kind == BareIdTarget::ThisField) {
                    //Implicit this.<field> = value (bare member write inside
                    //a method). Same opcode shape as the MemberExpr write.
                    EmitExpression(*assign.Right(), emitter,
                        m_currFunc->tempSlot2);
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(ImplicitThisSlot());
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.Emit(OpCode::OP_NullCheck);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
                    emitter.EmitUint16(m_currFunc->tempSlot2);
                }
            }
        } else if (assign.Left()->Kind() == NK_MemberExpr) {
            //Struct or class field assignment
            auto& memberExpr = static_cast<SnMemberExpr&>(*assign.Left());
            //Specialized handling for arr[i].field = value:
            //the subscript outer's index must NOT alias tempSlot2 (which holds
            //the right side value). Emit the subscript inline using
            //callParamBase for the index slot.
            if (memberExpr.Outer()->Kind() == NK_SubscriptExpr) {
                auto& sub = static_cast<SnSubscriptExpr&>(
                    *memberExpr.Outer());
                auto* elemType = sub.EvalDataType();
                if (elemType
                    && (elemType->Kind() == NK_ClassDecl
                        || elemType->Kind() == NK_StructDecl))
                {
                    auto* inner = memberExpr.Inner();
                    if (inner->Kind() != NK_IdentifierExpr) return;
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    uint16_t fieldOff = 0;
                    if (elemType->Kind() == NK_ClassDecl) {
                        int off = FindClassFieldOffset(
                            *static_cast<SnClassDecl*>(elemType), fieldName);
                        if (off < 0) return;
                        fieldOff = static_cast<uint16_t>(off);
                    } else {
                        int off = FindFieldOffset(
                            *static_cast<SnStructDecl*>(elemType), fieldName);
                        if (off < 0) return;
                        fieldOff = static_cast<uint16_t>(off);
                    }
                    //List/Dict receiver: `li[i].field = v` — the subscript
                    //is sugar over get(), so delegate to the generic
                    //NK_SubscriptExpr lowering (EvalAreaClaim + get() call).
                    //Phase 10 audit round-4: park [receiver, value] in an
                    //EvalAreaClaim(2) — the old tempSlot/tempSlot2 staging
                    //was clobbered by a call in the value (argument binaries
                    //fall through PickTempSlot to tempSlot; struct-argument
                    //deep copy scratches tempSlot), crashing or storing
                    //through the wrong object. Receiver first (JLS 15.26.1).
                    if (IsContainerSubscript(*sub.Array())) {
                        EvalAreaClaim claim(*this, 2);
                        uint16_t objSlot = claim.base();
                        uint16_t valueSlot = objSlot + VALUE_SIZE;
                        EmitExpression(sub, emitter, objSlot);
                        if (elemType->Kind() == NK_ClassDecl) {
                            emitter.Emit(OpCode::OP_NullCheck);
                            emitter.EmitUint16(objSlot);
                        }
                        EmitExpression(*assign.Right(), emitter, valueSlot);
                        emitter.Emit(OpCode::OP_StoreField);
                        emitter.EmitUint16(objSlot);
                        emitter.EmitUint16(fieldOff);
                        emitter.EmitUint16(valueSlot);
                        return;
                    }
                    //Phase 10 audit round-3: stage value/index/array in an
                    //exclusive EvalAreaClaim(3) — the old tempSlot/tempSlot2/
                    //callParamBase staging let a nested index expression
                    //(subscript-get, binary arithmetic — both scratch
                    //tempSlot via the PickTempSlot fall-through) clobber the
                    //parked array, crashing or silently storing through the
                    //wrong heap object. Same discipline as the container
                    //path above and SubscriptAssignStmt.
                    EvalAreaClaim claim(*this, 3);
                    uint16_t claimBase = claim.base();
                    uint16_t indexSlot = claimBase + VALUE_SIZE;
                    uint16_t valueSlot = claimBase + 2 * VALUE_SIZE;
                    //1. Evaluate right side → claim[2]
                    EmitExpression(*assign.Right(), emitter, valueSlot);
                    //2. Evaluate index → claim[1]
                    EmitExpression(*sub.Index(), emitter, indexSlot);
                    //3. Evaluate array ref → claim[0], null-checked
                    EmitExpression(*sub.Array(), emitter, claimBase);
                    emitter.Emit(OpCode::OP_NullCheck);
                    emitter.EmitUint16(claimBase);
                    //4. load element into tempSlot (free scratch — all
                    //operands are parked in exclusive claim slots now)
                    emitter.Emit(OpCode::OP_LoadElement);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(claimBase);
                    emitter.EmitUint16(indexSlot);
                    //5. null_check the element (class) or skip (struct is value)
                    if (elemType->Kind() == NK_ClassDecl) {
                        emitter.Emit(OpCode::OP_NullCheck);
                        emitter.EmitUint16(m_currFunc->tempSlot);
                    }
                    //6. store_field obj=tempSlot off=fieldOff src=claim[2]
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(fieldOff);
                    emitter.EmitUint16(valueSlot);
                    return;
                }
            }
            auto* outerType = memberExpr.Outer()->EvalDataType();
            if (outerType && outerType->Kind() == NK_ClassDecl) {
                //Class field assignment: reference semantics, no deep copy
                auto* classDecl = static_cast<SnClassDecl*>(outerType);
                auto* inner = memberExpr.Inner();
                if (inner->Kind() != NK_IdentifierExpr) return;
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int fieldOffInt = FindClassFieldOffset(*classDecl, fieldName);
                if (fieldOffInt < 0) return;
                uint16_t fieldOff = static_cast<uint16_t>(fieldOffInt);
                //Phase 10 audit round-4: park [receiver, value] in an
                //EvalAreaClaim(2) — the old tempSlot/tempSlot2 staging was
                //clobbered by a call in the RHS (argument binaries fall
                //through PickTempSlot to tempSlot; struct-argument deep
                //copy scratches tempSlot), crashing or silently storing
                //through the wrong object. Receiver first (JLS 15.26.1).
                EvalAreaClaim claim(*this, 2);
                uint16_t objSlot = claim.base();
                uint16_t valueSlot = objSlot + VALUE_SIZE;
                EmitExpression(*memberExpr.Outer(), emitter, objSlot);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(objSlot);
                EmitExpression(*assign.Right(), emitter, valueSlot);
                emitter.Emit(OpCode::OP_StoreField);
                emitter.EmitUint16(objSlot);
                emitter.EmitUint16(fieldOff);
                emitter.EmitUint16(valueSlot);
            } else if (outerType && outerType->Kind() == NK_StructDecl) {
                auto* structDecl = static_cast<SnStructDecl*>(outerType);
                //Find the field's offset and type
                auto* inner = memberExpr.Inner();
                if (inner->Kind() != NK_IdentifierExpr) return;
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int fieldOffInt = FindFieldOffset(*structDecl, fieldName);
                if (fieldOffInt < 0) return;
                uint16_t fieldOff = static_cast<uint16_t>(fieldOffInt);
                SnField* fieldType = nullptr;
                bool fieldIsArray = false;
                for (auto& sf : structDecl->Members()) {
                    if (sf.Name() == fieldName) {
                        fieldType = sf.EvalDataType();
                        fieldIsArray = sf.IsArrayType();
                        break;
                    }
                }
                //Array-typed struct fields store a heap idx (reference
                //semantics) — same IsArrayType guard as local assignment.
                if (fieldType && RuntimeTypeKind(fieldType) == RTK_Struct
                    && !fieldIsArray) {
                    //Struct-to-struct field assignment: deep-copy first.
                    //Phase 10 audit round-4: stage rhs/copy/outer in an
                    //EvalAreaClaim(3) — same parking discipline as the
                    //other member targets (temps are pure scratch; the
                    //fresh-copy handle must survive the outer emission).
                    EvalAreaClaim claim(*this, 3);
                    uint16_t copySlot = claim.base();
                    uint16_t objSlot = copySlot + VALUE_SIZE;
                    uint16_t rhsSlot = objSlot + VALUE_SIZE;
                    EmitExpression(*assign.Right(), emitter, rhsSlot);
                    int fieldStructIdx = m_compiledModule.FindStruct(
                        fieldType->Name());
                    emitter.Emit(OpCode::OP_CopyStruct);
                    emitter.EmitUint16(copySlot);
                    emitter.EmitUint16(rhsSlot);
                    emitter.EmitUint16(fieldStructIdx >= 0
                        ? static_cast<uint16_t>(fieldStructIdx) : 0);
                    //Evaluate outer (parent struct's heap index)
                    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
                    //Store the new heap index into the parent's field
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(objSlot);
                    emitter.EmitUint16(fieldOff);
                    emitter.EmitUint16(copySlot);
                } else {
                    //Primitive/enum/string/array field assignment.
                    //Phase 10 audit round-4: EvalAreaClaim(2) [receiver,
                    //value], receiver first — same discipline as the
                    //class branch above.
                    EvalAreaClaim claim(*this, 2);
                    uint16_t objSlot = claim.base();
                    uint16_t valueSlot = objSlot + VALUE_SIZE;
                    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
                    EmitExpression(*assign.Right(), emitter, valueSlot);
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(objSlot);
                    emitter.EmitUint16(fieldOff);
                    emitter.EmitUint16(valueSlot);
                }
            }
        }
        return;
    }

    //Compound assignment: x += y, this.f -= 1, arr[i] *= 2
    //Phase 9a: left-value is evaluated only once (read-modify-write).
    //Assert statement: assert(cond); - exit(1) on failure.
    //Codegen pattern: evaluate condition, OP_JumpIfNot to fail block,
    //fail block emits OP_AssertFail which throws (caught by main → exit 1).
    if (kind == NK_AssertStmt) {
        auto& as = static_cast<SnAssertStmt&>(stmt);
        EmitExpression(*as.Cond(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToFail = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(m_currFunc->tempSlot);
        //Success path: jump over fail block
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        //Fail block
        size_t failStart = emitter.CurrentOffset();
        emitter.Emit(OpCode::OP_AssertFail);
        //Optional message: use the assertion's source location text.
        //For Phase 9a we emit an empty message idx (0) — VmExecutor
        //prints "assertion failed" alone when msg is empty.
        emitter.EmitUint16(0);
        //End
        size_t endPos = emitter.CurrentOffset();
        emitter.PatchUint16(jumpToFail, static_cast<uint16_t>(failStart));
        emitter.PatchUint16(jumpToEnd, static_cast<uint16_t>(endPos));
        return;
    }

    if (kind == NK_CompoundAssignStmt) {
        auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
        auto op = ca.Op();

        if (ca.Left()->Kind() == NK_IdentifierExpr) {
            auto& idExpr = static_cast<SnIdentifierExpr&>(*ca.Left());
            auto* field = idExpr.Field();
            if (!field) return;
            BareIdTarget target = ResolveBareIdentifier(field);
            if (target.kind == BareIdTarget::Local) {
                //Local variable: compute in-place on the local slot
                EmitExpression(*ca.Right(), emitter, m_currFunc->tempSlot2);
                EmitCompoundOp(op, emitter, target.localOffset,
                    m_currFunc->tempSlot2, field->EvalDataType());
            } else if (target.kind == BareIdTarget::ThisField) {
                //Implicit this.<field> compound assign: same read-modify-write
                //shape as the explicit MemberExpr branch below, with the
                //receiver sourced from ImplicitThisSlot().
                //Phase 10 audit round-3: all three live values (receiver,
                //old field value, RHS) park in exclusive EvalAreaClaim
                //slots. Parking the receiver in tempSlot — unavoidable
                //across the RHS emission in a read-modify-write — let any
                //nested RHS expression clobber it, because PickTempSlot
                //falls back to tempSlot whenever its exclude slot is not
                //one of the four temps (claim slots included).
                {
                    EvalAreaClaim claim(*this, 3);
                    uint16_t claimBase = claim.base();
                    uint16_t oldValSlot = claimBase + VALUE_SIZE;
                    uint16_t rhsSlot = claimBase + 2 * VALUE_SIZE;
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(ImplicitThisSlot());
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    emitter.Emit(OpCode::OP_NullCheck);
                    emitter.EmitUint16(claimBase);
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(oldValSlot);
                    emitter.EmitUint16(claimBase);
                    emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
                    EmitExpression(*ca.Right(), emitter, rhsSlot);
                    EmitCompoundOp(op, emitter, oldValSlot,
                        rhsSlot, field->EvalDataType());
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(claimBase);
                    emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
                    emitter.EmitUint16(oldValSlot);
                }
            }
        } else if (ca.Left()->Kind() == NK_MemberExpr) {
            //Class/struct field: evaluate outer once, read field, compute, write back
            auto& memberExpr = static_cast<SnMemberExpr&>(*ca.Left());
            auto* outerType = memberExpr.Outer()->EvalDataType();
            if (!outerType) return;
            auto* inner = memberExpr.Inner();
            if (inner->Kind() != NK_IdentifierExpr) return;
            auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
            SnField* lhsFieldType = memberExpr.EvalDataType();

            uint16_t fieldOff = 0;
            if (outerType->Kind() == NK_ClassDecl) {
                int off = FindClassFieldOffset(
                    *static_cast<SnClassDecl*>(outerType), fieldName);
                if (off < 0) return;
                fieldOff = static_cast<uint16_t>(off);
            } else if (outerType->Kind() == NK_StructDecl) {
                int off = FindFieldOffset(
                    *static_cast<SnStructDecl*>(outerType), fieldName);
                if (off < 0) return;
                fieldOff = static_cast<uint16_t>(off);
            } else {
                return;
            }

            //Phase 10 audit round-3: read-modify-write parks three live
            //values across the RHS emission (receiver, old value, RHS) —
            //all three go into exclusive EvalAreaClaim slots for the same
            //reason as the implicit this-field branch above: PickTempSlot
            //falls back to tempSlot for non-temp exclude slots, so a
            //receiver parked in any temp is clobberable by a nested RHS.
            {
                EvalAreaClaim claim(*this, 3);
                uint16_t claimBase = claim.base();
                uint16_t oldValSlot = claimBase + VALUE_SIZE;
                uint16_t rhsSlot = claimBase + 2 * VALUE_SIZE;
                EmitExpression(*memberExpr.Outer(), emitter, claimBase);
                if (outerType->Kind() == NK_ClassDecl) {
                    emitter.Emit(OpCode::OP_NullCheck);
                    emitter.EmitUint16(claimBase);
                }
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(oldValSlot);
                emitter.EmitUint16(claimBase);
                emitter.EmitUint16(fieldOff);
                EmitExpression(*ca.Right(), emitter, rhsSlot);
                EmitCompoundOp(op, emitter, oldValSlot,
                    rhsSlot, lhsFieldType);
                emitter.Emit(OpCode::OP_StoreField);
                emitter.EmitUint16(claimBase);
                emitter.EmitUint16(fieldOff);
                emitter.EmitUint16(oldValSlot);
            }
        }
        //Note: subscript compound assign (arr[i] += 1) is intentionally not
        //supported in Phase 9a — left-value single-eval requires 4 scratch
        //slots and complicates the grammar. Users can write
        //`arr[i] = arr[i] + 1` instead.
        return;
    }

    //Subscript assignment: arr[index] = value
    //Reference: EN's AssignStmt::Compile pattern for indexed stores.
    if (kind == NK_SubscriptAssignStmt) {
        auto& sub = static_cast<SnSubscriptAssignStmt&>(stmt);
        //List<T>/Dict<K,V> subscript store: li[i] = v == li.set(i, v),
        //d[k] = v == d.set(k, v) — sugar over the set() intrinsic call.
        //Runs entirely inside an EvalAreaClaim(3) [this, index, value] so
        //nested calls in any operand cannot clobber the params, and no
        //shared temp slots are touched (claim slots are exclusive).
        if (IsContainerSubscript(*sub.Array())) {
            auto* pGenClass = static_cast<SnClassDecl*>(
                sub.Array()->EvalDataType());
            const auto& baseName = pGenClass->BaseName();
            const auto& typeArgs = pGenClass->GenericTypeArgs();
            bool isList = (baseName == "List" && !typeArgs.empty());
            bool isDict = (baseName == "Dict" && typeArgs.size() > 1);
            {
                        //Dict keys box when primitive; List's index is int.
                        auto keyBox = isDict
                            ? BoxingTagFor(typeArgs[0])
                            : BoxingTagResult{0, false};
                        //Value boxes when T/V is primitive (argPlans[2]
                        //in the member-call path; slot 2 here).
                        SnField* pElem = isList ? typeArgs[0]
                            : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
                        auto valBox = BoxingTagFor(pElem);
                        EvalAreaClaim claim(*this, 3);
                        uint16_t claimBase = claim.base();
                        //arg0 = index → claim[1]
                        uint16_t keyOffset = claimBase + VALUE_SIZE;
                        EmitExpression(*sub.Index(), emitter, keyOffset);
                        if (keyBox.isPrimitive) {
                            EmitPResultRefresh(emitter, keyOffset);
                            emitter.Emit(OpCode::OP_Box);
                            emitter.EmitByte(keyBox.tag);
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(keyOffset);
                        }
                        //arg1 = value → claim[2]
                        uint16_t valOffset = claimBase + 2 * VALUE_SIZE;
                        EmitExpression(*sub.Value(), emitter, valOffset);
                        if (valBox.isPrimitive) {
                            EmitPResultRefresh(emitter, valOffset);
                            emitter.Emit(OpCode::OP_Box);
                            emitter.EmitByte(valBox.tag);
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(valOffset);
                        }
                        //this = receiver → claim[0], null-checked.
                        EmitExpression(*sub.Array(), emitter, claimBase);
                        emitter.Emit(OpCode::OP_NullCheck);
                        emitter.EmitUint16(claimBase);
                        //Bulk-copy claim → callParamBase, then set().
                        for (uint16_t i = 0; i < 3; ++i) {
                            emitter.Emit(OpCode::OP_VarLocal);
                            emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(
                                m_currFunc->callParamBase + i * VALUE_SIZE);
                        }
                        uint16_t nameIdx = AddStringConstant("set");
                        emitter.Emit(OpCode::OP_CallMethod);
                        emitter.EmitUint16(nameIdx);
                        emitter.EmitUint16(m_currFunc->callParamBase);
                        emitter.Emit(OpCode::OP_ParaEnd);
                        return;
                    }
        }
        //Detect element type from the array's resolved field.
        //The element type is needed for struct deep-copy on write.
        SnField* elemType = nullptr;
        if (sub.Array()->Kind() == NK_IdentifierExpr) {
            auto* arrField = static_cast<SnIdentifierExpr&>(
                *sub.Array()).Field();
            if (arrField && arrField->IsArrayType()
                && arrField->EvalDataType())
                elemType = arrField->EvalDataType();
        }
        //Phase 10 audit round-2: runs entirely inside an EvalAreaClaim(3)
        //[array, index, value] — the old tempSlot/tempSlot2/callParamBase
        //staging let any nested expression in the index (subscript-get,
        //binary arithmetic — both reuse the shared temps) clobber the
        //parked base/value. Claim slots are exclusive, same discipline as
        //the container path above.
        EvalAreaClaim claim(*this, 3);
        uint16_t claimBase = claim.base();
        uint16_t indexSlot = claimBase + VALUE_SIZE;
        uint16_t valueSlot = claimBase + 2 * VALUE_SIZE;
        EmitExpression(*sub.Value(), emitter, valueSlot);
        EmitExpression(*sub.Index(), emitter, indexSlot);
        //Array reference last, null-checked (mirrors container path).
        EmitExpression(*sub.Array(), emitter, claimBase);
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(claimBase);
        //For struct element types, deep-copy value before storing.
        if (elemType && RuntimeTypeKind(elemType) == RTK_Struct) {
            int structIdx = m_compiledModule.FindStruct(elemType->Name());
            emitter.Emit(OpCode::OP_CopyStruct);
            emitter.EmitUint16(valueSlot);
            emitter.EmitUint16(valueSlot);
            emitter.EmitUint16(structIdx >= 0
                ? static_cast<uint16_t>(structIdx) : 0);
        }
        emitter.Emit(OpCode::OP_StoreElement);
        emitter.EmitUint16(claimBase);
        emitter.EmitUint16(indexSlot);
        emitter.EmitUint16(valueSlot);
        return;
    }

    //If/else statement.
    //Reference: EN's IfStmt::Compile (SeStatements.cpp:367).
    if (kind == NK_IfStmt) {
        auto& ifStmt = static_cast<SnIfStmt&>(stmt);
        //Evaluate condition to tempSlot
        EmitExpression(*ifStmt.Cond(), emitter, m_currFunc->tempSlot);
        //JumpIfNot to else/endif
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToElse = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        emitter.EmitUint16(m_currFunc->tempSlot);  //local offset to check
        //Then branch
        EmitStatement(*ifStmt.ThenStmt(), emitter);
        //Jump to endif (skip else branch)
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        //Else branch
        size_t elseStart = emitter.CurrentOffset();
        if (ifStmt.ElseStmt())
            EmitStatement(*ifStmt.ElseStmt(), emitter);
        //Fixup jumps
        size_t endPos = emitter.CurrentOffset();
        emitter.PatchUint16(jumpToElse, static_cast<uint16_t>(elseStart));
        emitter.PatchUint16(jumpToEnd, static_cast<uint16_t>(endPos));
        return;
    }

    //While loop statement.
    //Reference: EN's WhileStmt::DoCompile (SeStatements.cpp:468).
    if (kind == NK_WhileStmt) {
        auto& whileStmt = static_cast<SnWhileStmt&>(stmt);
        size_t loopStart = emitter.CurrentOffset();

        PushLoopContext();

        //Evaluate condition to tempSlot
        EmitExpression(*whileStmt.Cond(), emitter, m_currFunc->tempSlot);
        //JumpIfNot to end of loop
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        emitter.EmitUint16(m_currFunc->tempSlot);  //local offset to check
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //Loop body
        EmitStatement(*whileStmt.Body(), emitter);

        //Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //Fixup jumps
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        //Reference: EN's WhileStmt continue jumps back to locStart (condition check)
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopStart));

        m_loopStack.pop_back();
        return;
    }

    //Do-while loop statement.
    //Reference: EN's DoStmt::DoCompile (SeStatements.cpp:507).
    if (kind == NK_DoStmt) {
        auto& doStmt = static_cast<SnDoStmt&>(stmt);

        PushLoopContext();

        //1. Loop body (executed at least once)
        size_t loopStart = emitter.CurrentOffset();
        EmitStatement(*doStmt.Body(), emitter);

        //2. Continue target: condition check
        size_t continueTarget = emitter.CurrentOffset();

        //3. Condition check
        EmitExpression(*doStmt.Cond(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(m_currFunc->tempSlot);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //4. Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //5. Loop end, fixup jumps
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        //Reference: EN's DoStmt continue jumps to locCondition
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
    }

    //For loop statement.
    //Reference: EN's ForStmt::DoCompile (SeStatements.cpp:546).
    if (kind == NK_ForStmt) {
        auto& forStmt = static_cast<SnForStmt&>(stmt);

        //1. Compile init part (before loop context)
        if (forStmt.Init())
            EmitStatement(*forStmt.Init(), emitter);
        //Compile decomposed init AssignStmts
        for (auto* pExtra : forStmt.InitExtras())
            EmitStatement(*pExtra, emitter);

        //2. Loop start
        size_t loopStart = emitter.CurrentOffset();

        //3. Enter loop context (reference: EN's LoopStmt::Compile)
        PushLoopContext();

        //4. Condition check
        EmitExpression(*forStmt.Cond(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(m_currFunc->tempSlot);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //5. Loop body
        EmitStatement(*forStmt.Body(), emitter);

        //6. Continue target: fini part
        size_t continueTarget = emitter.CurrentOffset();

        //7. Compile fini part
        if (forStmt.Fini())
            EmitStatement(*forStmt.Fini(), emitter);

        //8. Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //9. Loop end
        size_t loopEnd = emitter.CurrentOffset();

        //10. Fixup jumps (reference: EN's FixDirectJumps)
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
    }

    //Foreach loop statement (Phase 8e-5).
    //Index-based expansion reusing Length()/Get() (List), arr.length + arr[i] (Array).
    //No new opcode. Dict path is Phase D.
    if (kind == NK_ForeachStmt) {
        auto& fe = static_cast<SnForeachStmt&>(stmt);

        //--- Detect iterable kind -----------------------------------------
        SnExpression* pIter = fe.Iterable();
        bool isArray = false;
        bool isList  = false;
        bool isDict  = false;
        SnField* pElemType = nullptr;

        if (pIter->Kind() == NK_IdentifierExpr) {
            auto* pField = static_cast<SnIdentifierExpr*>(pIter)->Field();
            if (pField && pField->IsArrayType()) {
                isArray = true;
                pElemType = pField->EvalDataType();
            }
        }
        if (!isArray) {
            auto* pIterType = pIter->EvalDataType();
            if (pIterType && pIterType->Kind() == NK_ClassDecl) {
                auto* pClass = static_cast<SnClassDecl*>(pIterType);
                if (pClass->IsGenericInstantiation()) {
                    const auto& baseName = pClass->BaseName();
                    const auto& typeArgs = pClass->GenericTypeArgs();
                    if (baseName == "List") {
                        isList = true;
                        pElemType = typeArgs.empty() ? nullptr : typeArgs[0];
                    } else if (baseName == "Dict") {
                        isDict = true;
                        pElemType = typeArgs.empty() ? nullptr : typeArgs[0];
                    }
                }
            }
        }
        //Phase C: Array + List. Phase D: Dict (inline Keys() call materializes
        //a List<K> into iterSlot, then the rest mirrors the List path).
        assert((isArray || isList || isDict)
            && "foreach iterable must be Array, List<T>, or Dict<K,V>");

        uint8_t elemKind = RuntimeTypeKind(pElemType);

        //--- 1. Allocate hidden locals BEFORE LoopContext push -------------
        //AllocLocal dedupes by name, so uniquify hidden locals via per-function
        //counter (nested foreach would otherwise collide on __foreach_iter etc.).
        uint16_t counter = m_currFunc->foreachCounter++;
        uint16_t userVarSlot = AllocLocal(fe.VarName(),
            VALUE_SIZE, elemKind, false);
        //Array iter is RTK_Array; List and Dict-via-Keys are RTK_Class.
        uint8_t iterKind = isArray
            ? static_cast<uint8_t>(RTK_Array)
            : static_cast<uint8_t>(RTK_Class);
        uint16_t iterSlot = AllocLocal(
            "__foreach_iter_" + std::to_string(counter),
            VALUE_SIZE, iterKind, false);
        uint16_t iSlot = AllocLocal(
            "__foreach_i_" + std::to_string(counter),
            VALUE_SIZE, RTK_Int32, false);
        uint16_t nSlot = AllocLocal(
            "__foreach_n_" + std::to_string(counter),
            VALUE_SIZE, RTK_Int32, false);

        //--- 2. Evaluate iterable into iterSlot ---------------------------
        EmitExpression(*pIter, emitter, iterSlot);

        //--- 2b. Dict: inline dict.Keys() → materialize List<K> into iterSlot
        //For Dict foreach, iterSlot now holds a dict heap idx; we replace it
        //with a fresh List<K> heap idx from the Keys() intrinsic. From here on,
        //the lowering is identical to List<T> iteration (element type = K).
        if (isDict) {
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase);
            uint16_t keysIdx = AddStringConstant("keys");
            emitter.Emit(OpCode::OP_CallMethod);
            emitter.EmitUint16(keysIdx);
            emitter.EmitUint16(m_currFunc->callParamBase);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_ParaEnd);
        }

        //--- 3. Compute length into nSlot ---------------------------------
        if (isArray) {
            //OP_ArrayLength <dst=arr> <src=arr> — operates on the heap idx in
            //the slot. Mirror the pattern at line 1259 (NullCheck first).
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_ArrayLength);
            emitter.EmitUint16(nSlot);
            emitter.EmitUint16(iterSlot);
        } else {
            //List<T>.Length() (or Dict-after-Keys: List<K>.Length()).
            //Call shape mirrors line 1155-1200.
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase);
            uint16_t nameIdx = AddStringConstant("length");
            emitter.Emit(OpCode::OP_CallMethod);
            emitter.EmitUint16(nameIdx);
            emitter.EmitUint16(m_currFunc->callParamBase);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(nSlot);
            emitter.Emit(OpCode::OP_ParaEnd);
        }

        //--- 4. i = 0 -----------------------------------------------------
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(iSlot);

        //--- 5. Loop start ------------------------------------------------
        size_t loopStart = emitter.CurrentOffset();

        //--- 6. Enter loop context (reuse LoopContext for break/continue) -
        PushLoopContext();

        //--- 7. Condition: i < n → jumpToEnd if not -----------------------
        //tempSlot = i; tempSlot2 = n; OP_Less_i32 writes 1/0 into tempSlot.
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(iSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(nSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        emitter.Emit(OpCode::OP_Less_i32);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder, patched at step 12
        emitter.EmitUint16(m_currFunc->tempSlot);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //--- 8. Body-prelude: load element i into userVarSlot -------------
        if (isArray) {
            //OP_LoadElement <dst> <arr> <index>
            emitter.Emit(OpCode::OP_LoadElement);
            emitter.EmitUint16(userVarSlot);
            emitter.EmitUint16(iterSlot);
            emitter.EmitUint16(iSlot);
            //Struct element types need deep-copy on read (value semantics),
            //parallel to subscript codegen at line 1478-1485.
            if (elemKind == RTK_Struct && pElemType) {
                int structIdx = m_compiledModule.FindStruct(
                    pElemType->Name());
                emitter.Emit(OpCode::OP_CopyStruct);
                emitter.EmitUint16(userVarSlot);
                emitter.EmitUint16(userVarSlot);
                emitter.EmitUint16(structIdx >= 0
                    ? static_cast<uint16_t>(structIdx) : 0);
            }
        } else {
            //List<T>.Get(i). Per-method boxing plan: unbox primitive T.
            //callParamBase[1] = i; callParamBase[0] = this; OP_CallMethod.
            uint16_t paramOffset = m_currFunc->callParamBase + 1 * VALUE_SIZE;
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(iSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(paramOffset);
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase);
            uint16_t nameIdx = AddStringConstant("get");
            emitter.Emit(OpCode::OP_CallMethod);
            emitter.EmitUint16(nameIdx);
            emitter.EmitUint16(m_currFunc->callParamBase);
            auto t = BoxingTagFor(pElemType);
            if (t.isPrimitive) {
                emitter.Emit(OpCode::OP_Unbox);
                emitter.EmitByte(t.tag);
            }
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(userVarSlot);
            emitter.Emit(OpCode::OP_ParaEnd);
        }

        //--- 9. Body ------------------------------------------------------
        EmitStatement(*fe.Body(), emitter);

        //--- 10. Continue target: i = i + 1 -------------------------------
        size_t continueTarget = emitter.CurrentOffset();
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(1);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        //OP_Add_i32 <dst> <src>: locals[dst] += locals[src].
        emitter.Emit(OpCode::OP_Add_i32);
        emitter.EmitUint16(iSlot);
        emitter.EmitUint16(m_currFunc->tempSlot2);

        //--- 11. Jump back to loop start ----------------------------------
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //--- 12. Patch break/continue; pop LoopContext --------------------
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
    }

    //Break statement.
    //Reference: EN's BreakStmt::Compile (SeStatements.cpp:860).
    //Break exits the innermost enclosing switch or loop.
    if (kind == NK_BreakStmt) {
        if (m_loopStack.empty()) {
            //This should be caught by an earlier validation pass.
            //Reference: EN's BreakStmt::Compile checks NestBreaks.
            assert(!"break statement not in loop or switch");
            return;
        }
        //Phase 9d follow-up: if break exits a loop/switch that sits
        //outside one or more catch bodies (relative to where the break
        //is lexically), emit OP_PopHandler for each such catch body.
        //This balances handlerExcStack — otherwise a subsequent `throw;`
        //in this function would rethrow a stale exception.
        int pops = m_catchBodyDepth - m_loopStack.back().catchBodyDepthAtEntry;
        for (int i = 0; i < pops; ++i)
            emitter.Emit(OpCode::OP_PopHandler);
        //Phase 9d-2: run inline copies of the finally bodies this break
        //passes through (regions entered after the target loop), innermost
        //first, then jump to the loop's break target.
        for (size_t i = m_finallyStack.size();
             i > static_cast<size_t>(m_loopStack.back().finallyDepthAtEntry);
             --i) {
            EmitStatement(*m_finallyStack[i - 1], emitter);
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        m_loopStack.back().breakJumps.push_back(jumpPos);
        return;
    }

    //Continue statement.
    //Reference: EN's ContinueStmt::Compile (SeStatements.cpp:886).
    //Continue targets the innermost enclosing *loop*, not switch.
    if (kind == NK_ContinueStmt) {
        //Walk the stack to find the nearest actual loop (not switch).
        //Reference: EN's ContinueStmt skips switch contexts.
        auto it = m_loopStack.rbegin();
        while (it != m_loopStack.rend() && it->isSwitch)
            ++it;
        if (it == m_loopStack.rend()) {
            //This should be caught by an earlier validation pass.
            //Reference: EN's ContinueStmt::Compile checks NestContinues.
            assert(!"continue statement not in loop");
            return;
        }
        //Phase 9d follow-up: same handler-balancing as break — see above.
        int pops = m_catchBodyDepth - it->catchBodyDepthAtEntry;
        for (int i = 0; i < pops; ++i)
            emitter.Emit(OpCode::OP_PopHandler);
        //Phase 9d-2: same finally trampolines as break, targeting this
        //loop's continue target.
        for (size_t i = m_finallyStack.size();
             i > static_cast<size_t>(it->finallyDepthAtEntry);
             --i) {
            EmitStatement(*m_finallyStack[i - 1], emitter);
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        it->continueJumps.push_back(jumpPos);
        return;
    }

    //Switch statement.
    //Reference: EN's SwitchStmt::DoCompile (SeStatements.cpp:725).
    if (kind == NK_SwitchStmt) {
        auto& switchStmt = static_cast<SnSwitchStmt&>(stmt);

        //1. Allocate a dedicated slot for the switch value.
        //This slot must not be overwritten by case condition compilation.
        uint16_t switchSlot = m_currFunc->nextOffset;
        m_currFunc->nextOffset += VALUE_SIZE;

        //2. Compile the switch expression to switchSlot
        EmitExpression(*switchStmt.Cond(), emitter, switchSlot);

        //3. Emit OP_Switch with the switch value's local offset.
        //Note: OP_Switch is a marker opcode (no runtime effect beyond reading
        //the operand). It aids disassembly and could be given runtime semantics
        //in a future optimization (e.g. jump-table dispatch).
        emitter.Emit(OpCode::OP_Switch);
        emitter.EmitUint16(switchSlot);

        //4. Enter switch context (break jumps out of switch)
        PushLoopContext(true);

        //5. Compile each case clause
        //Reference: EN's SwitchStmt::DoCompile — for each case, emit
        //I_Base_Case + jump-to-next-handler placeholder + condition + body.
        std::vector<size_t> nextJumps;
        std::vector<size_t> caseStartOffsets;

        for (auto* pCase : switchStmt.Cases()) {
            //Record this case's start offset
            size_t caseStart = emitter.CurrentOffset();
            caseStartOffsets.push_back(caseStart);

            //Emit OP_Case with jump-to-next-handler placeholder.
            //Note: OP_Case is a marker opcode. Its uint16 operand is patched by
            //FixChainedJumps but never used at runtime (branching is done by
            //OP_JumpIfNot). A future optimization could merge OP_Case with the
            //condition check into a single opcode.
            emitter.Emit(OpCode::OP_Case);
            size_t jumpToNext = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder, patched by FixChainedJumps
            nextJumps.push_back(jumpToNext);

            //Compile condition: switch_value == case_constant
            //Load switch value from dedicated slot to tempSlot2
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(switchSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            //Compile case constant to tempSlot
            EmitExpression(*pCase->Cond(), emitter, m_currFunc->tempSlot);
            //Compare: tempSlot2 == tempSlot → result in tempSlot2
            emitter.Emit(OpCode::OP_Equal_i32);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            emitter.EmitUint16(m_currFunc->tempSlot);
            //If not equal, jump to next case handler
            emitter.Emit(OpCode::OP_JumpIfNot);
            size_t condJumpPos = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder, same target as nextJump
            emitter.EmitUint16(m_currFunc->tempSlot2);
            nextJumps.push_back(condJumpPos);

            //Compile case body
            EmitStatement(*pCase->Body(), emitter);
        }

        //5. Mark locCaseEnd (after all cases, before default)
        size_t locCaseEnd = emitter.CurrentOffset();

        //6. Compile default clause
        if (switchStmt.Default())
            EmitStatement(*switchStmt.Default(), emitter);

        //7. Mark locEnd (after default)
        size_t locEnd = emitter.CurrentOffset();

        //8. FixChainedJumps: patch nextJumps so each case's jumps point
        //to the next case's start. The last case's jumps point to
        //default (if present) or switch end.
        //Reference: EN's Compiler::FixChainedJumps (Compiler.h:86).
        {
            size_t caseCount = caseStartOffsets.size();
            for (size_t i = 0; i < caseCount; ++i) {
                uint16_t target;
                if (i + 1 < caseCount)
                    target = static_cast<uint16_t>(caseStartOffsets[i + 1]);
                else
                    target = static_cast<uint16_t>(
                        switchStmt.Default() ? locCaseEnd : locEnd);

                //Each case has 2 entries in nextJumps: jumpToNext and condJumpPos
                size_t baseIdx = i * 2;
                emitter.PatchUint16(nextJumps[baseIdx], target);
                emitter.PatchUint16(nextJumps[baseIdx + 1], target);
            }
        }

        //9. Fix break jumps (jump to switch end)
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(locEnd));

        m_loopStack.pop_back();
        return;
    }

    //Phase 9d: try { body } catch (Type var) { handler } ...
    //Phase 9d-2: optional finally clause (full Java semantics).
    //
    //Codegen pattern without finally:
    //   tryStart:
    //     <body bytecode>
    //     OP_Jump postTry      ; body completed normally — skip all catches
    //   tryEnd:                 ; (also each handler's startPc for tryBlocks table)
    //   handler1:
    //     <handler1 bytecode>
    //     OP_PopHandler
    //     OP_Jump postTry
    //   handler2:
    //     ...
    //   postTry:
    //
    //Codegen pattern with finally:
    //   tryStart:
    //     <body bytecode>
    //     OP_Jump finallyNormal
    //   tryEnd:
    //   handler1:
    //     <handler1 bytecode>
    //     OP_PopHandler
    //     OP_Jump finallyNormal  ; catch completion also runs finally
    //   handler2:
    //     ...
    //   rangeEnd:                ; finally entry's covered range extends here
    //                             ; so exceptions in catch bodies reach it
    //   finallyHandler:          ; tryBlocks entry (catch-all 0xFFFF), pushed
    //     <finally body copy>    ; LAST so typed catches win first
    //     OP_Rethrow             ; re-raise the in-flight exception
    //   finallyNormal:
    //     <finally body copy>    ; normal/catch-completion path
    //     OP_Jump postTry
    //   postTry:
    //
    //All finally body copies live OUTSIDE the covered range [tryStart,
    //rangeEnd), so an exception thrown inside a finally body propagates
    //outward directly (no double execution by the same handler).
    //break/continue/return leaving the region execute their own inline
    //copies (see NK_BreakStmt/NK_ContinueStmt/NK_ReturnStmt).
    //
    //tryBlocks entries are pushed in declaration order; the runtime scans
    //linearly and the first range+type match wins.
    if (kind == NK_TryStmt) {
        auto& ts = static_cast<SnTryStmt&>(stmt);
        SnStatement* pFinally = ts.FinallyBody();

        uint16_t tryStart = static_cast<uint16_t>(emitter.CurrentOffset());

        //Phase 9d-2: register the finally region BEFORE emitting the try
        //body and catch bodies — break/continue/return sites inside them
        //consult m_finallyStack to emit inline copies.
        if (pFinally)
            m_finallyStack.push_back(pFinally);

        if (ts.TryBody())
            EmitStatement(*ts.TryBody(), emitter);
        //Body completed normally — skip catch handlers (to finallyNormal
        //when a finally clause exists, else postTry).
        emitter.Emit(OpCode::OP_Jump);
        size_t tryEndJumpPatch = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder, patched below
        uint16_t tryEnd = static_cast<uint16_t>(emitter.CurrentOffset());

        std::vector<size_t> catchEndJumpPatches;
        for (auto* pCatch : ts.Catches()) {
            uint16_t handlerPc = static_cast<uint16_t>(emitter.CurrentOffset());

            //Resolve exceptionClassIdx from CatchType's resolved EvalDataType
            //(a SnClassDecl*). If unresolved (earlier resolver error), use
            //0xFFFF as a sentinel — at runtime 0xFFFF is a catch-all, but
            //unresolved types only occur after a reported compile error.
            uint16_t excClassIdx = 0xFFFF;
            if (pCatch->CatchType()->IsResolved()
                && pCatch->CatchType()->Field()) {
                auto* pType = pCatch->CatchType()->Field();
                if (pType && pType->Kind() == NK_ClassDecl) {
                    auto& ccName = pType->Name();
                    auto found = std::find_if(
                        m_compiledModule.classes.begin(),
                        m_compiledModule.classes.end(),
                        [&](const CompiledClass& c) { return c.name == ccName; });
                    if (found != m_compiledModule.classes.end()) {
                        excClassIdx = static_cast<uint16_t>(
                            std::distance(m_compiledModule.classes.begin(),
                                          found));
                    }
                }
            }

            //catchLocalOff: allocate a local slot for the catch var. This
            //is where the runtime writes the caught Exception heap idx on
            //handler entry, and where the body's IdentifierExpr resolves.
            uint16_t typeKind = RTK_Class;
            uint16_t catchOff = AllocLocal(pCatch->VarName(), VALUE_SIZE,
                                           typeKind, false);
            m_currFunc->func->tryBlocks.push_back(
                {tryStart, tryEnd, handlerPc, excClassIdx, catchOff});

            if (pCatch->Body()) {
                ++m_catchBodyDepth;
                EmitStatement(*pCatch->Body(), emitter);
                --m_catchBodyDepth;
            }
            emitter.Emit(OpCode::OP_PopHandler);
            emitter.Emit(OpCode::OP_Jump);
            catchEndJumpPatches.push_back(emitter.CurrentOffset());
            emitter.EmitUint16(0);  //placeholder
        }

        if (!pFinally) {
            //No finally: normal and catch-completion jumps go to postTry.
            uint16_t postTry = static_cast<uint16_t>(emitter.CurrentOffset());
            emitter.PatchUint16(tryEndJumpPatch, postTry);
            for (size_t p : catchEndJumpPatches)
                emitter.PatchUint16(p, postTry);
            return;
        }

        //Finally layout. The finally entry's covered range extends past
        //the catch handlers so exceptions raised inside a catch body also
        //run the finally body (then rethrow outward — a sibling catch must
        //NOT intercept it, which the linear scan guarantees because each
        //catch entry's endPc is still tryEnd).
        uint16_t rangeEnd = static_cast<uint16_t>(emitter.CurrentOffset());
        uint16_t finallyHandlerPc = rangeEnd;

        //finallyHandler: exception path — run body copy, then rethrow.
        //catchLocalOff must be a real slot: the runtime writes the caught
        //exception heap idx there on handler entry unconditionally. A
        //hidden shared scratch local (name-deduped per function) is dead
        //storage; slot 0 would clobber `this` in methods.
        uint16_t finallyExcOff = AllocLocal("$finally_exc", VALUE_SIZE,
                                            RTK_Class, false);
        m_currFunc->func->tryBlocks.push_back(
            {tryStart, rangeEnd, finallyHandlerPc, 0xFFFF, finallyExcOff});
        EmitStatement(*pFinally, emitter);
        emitter.Emit(OpCode::OP_Rethrow);

        //finallyNormal: normal and catch-completion path — body copy,
        //then jump past the region.
        uint16_t finallyNormal = static_cast<uint16_t>(emitter.CurrentOffset());
        EmitStatement(*pFinally, emitter);
        emitter.Emit(OpCode::OP_Jump);
        size_t finallyEndJumpPatch = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder, patched to postTry

        uint16_t postTry = static_cast<uint16_t>(emitter.CurrentOffset());
        emitter.PatchUint16(tryEndJumpPatch, finallyNormal);
        for (size_t p : catchEndJumpPatches)
            emitter.PatchUint16(p, finallyNormal);
        emitter.PatchUint16(finallyEndJumpPatch, postTry);
        m_finallyStack.pop_back();
        return;
    }

    if (kind == NK_ThrowStmt) {
        auto& th = static_cast<SnThrowStmt&>(stmt);
        if (th.IsRethrow()) {
            emitter.Emit(OpCode::OP_Rethrow);
        } else {
            EmitExpression(*th.Expr(), emitter, m_currFunc->tempSlot);
            emitter.Emit(OpCode::OP_Throw);
            emitter.EmitUint16(m_currFunc->tempSlot);
        }
        return;
    }

    //Phase 9d-2: super(args); — forward to the direct parent constructor.
    //Mirrors NK_NewExpr's ctor-call pattern: evalArea claim [0]=this,
    //[1..N]=args, bulk copy to callParamBase, OP_CallMethodDirect on the
    //parent's ctor (works for both user ctors and built-in Exception
    //family stubs via the intrinsic shortcut). No parent ctor (0xFFFF) is
    //a legal no-op — the resolver guarantees no args in that case.
    if (kind == NK_SuperCallStmt) {
        auto& sc = static_cast<SnSuperCallStmt&>(stmt);
        if (!m_pCurrClass || !m_pCurrClass->SuperClass())
            return;  //resolver already reported; emit nothing
        auto* pParent = m_pCurrClass->SuperClass();
        int parentClassIdx = m_compiledModule.FindClass(pParent->Name());
        if (parentClassIdx < 0)
            return;
        uint16_t ctorIdx =
            m_compiledModule.classes[parentClassIdx].constructorIdx;
        if (ctorIdx == 0xFFFF)
            return;  //parent has no ctor; args were rejected by resolver

        uint16_t n = static_cast<uint16_t>(1 + sc.Args().size());
        EvalAreaClaim claim(*this, n);
        uint16_t claimBase = claim.base();

        //args → claim[1..N] (positional only; resolver rejected named).
        uint16_t paramIdx = 1;
        for (auto* pArg : sc.Args()) {
            uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
            EmitExpression(*pArg, emitter, paramOffset);
            ++paramIdx;
        }
        //this (local 0 in a method) → claim[0].
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(claimBase);

        //Bulk-copy claim → callParamBase, then call the parent ctor.
        for (uint16_t i = 0; i < n; ++i) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(claimBase + i * VALUE_SIZE);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
        }
        emitter.Emit(OpCode::OP_CallMethodDirect);
        emitter.EmitUint16(ctorIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_ParaEnd);
        return;
    }
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
