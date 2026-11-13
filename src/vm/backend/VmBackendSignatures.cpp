/*---
    VmBackendSignatures.cpp — body-less记录族的签名面：v1.12类型描述符
    收集（CollectSignatureTypeDescs）与三张签名-only记录（native/Phase
    9f、abstract/0.13.1c、共享体FillSignatureOnlyRecord），以及三者与
    AllocParamsAndDefaults共享的签名助手（this接收者谓词、接收者GC种类、
    泛型记录元数据）。从VmBackend.cpp分文件（源尺寸守卫拆分，零行为
    变化；一关注点一TU，同族先例见RegisterInterfaces.cpp）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/runtime/NodeConsts.h>
#include <vector>

namespace nlang {

//True when the frame carries a slot-0 `this` receiver: class/struct/enum
//methods only. 0.9.10: a static method has no receiver — its frame starts
//at the first formal, matching the free-function layout it is called with.
//0.9.11 D2: struct constructors join (structs hold no general methods, so
//only ctors reach this predicate).
bool VmBackend::HasThisReceiver(const SnFunction& func) {
    return func.Parent()
        && (func.Parent()->Kind() == NK_ClassDecl
            || func.Parent()->Kind() == NK_StructDecl
            || func.Parent()->Kind() == NK_EnumDecl)
        && !func.ContainFlags(NF_Static);
}

//GC kind of the `this` slot. Phase 12: an enum method's `this` is the enum
//VALUE (int32), not a heap reference — RTK_Class here would make GC root
//scanning treat the integer as a heap index (plan 12b round-1 MAJOR 2/3).
//0.9.11 D2: a struct constructor's `this` is the struct heap handle —
//RTK_Struct keeps GC root scanning on the right pool.
uint8_t VmBackend::ThisReceiverKind(const SnFunction& func) {
    if (func.Parent()->Kind() == NK_EnumDecl)
        return RTK_Int32;
    if (func.Parent()->Kind() == NK_StructDecl)
        return RTK_Struct;
    return RTK_Class;
}

//0.13.1a: generic-record metadata — the frame descriptor cell is filled
//only for these records at push, and the cross-module stub rebuild
//matches the declared parameter names to bind placeholder leaves in
//signatures (mirroring CompiledClass::typeParamNames). Task 4: a
//generic FREE function names its own parameters on the declaration —
//one implementation record, signature parameterization retained
//(placeholder leaves), the call-site descriptor row supplies the frame
//cell. 0.13.1c Task 8: a static of a generic INTERFACE joins — the
//template row owns the one record (`Pair<T>.echo` compiles once;
//`Pair<int,string>` static calls ride the call-site descriptor row).
void VmBackend::FillGenericRecordMeta(const SnFunction& func,
                                      CompiledFunction& compiledFunc) {
    auto* pOwner = func.Parent();
    if (pOwner && pOwner->Kind() == NK_ClassDecl
        && static_cast<SnClassDecl*>(pOwner)->IsGenericDeclaration())
    {
        compiledFunc.isGeneric = true;
        compiledFunc.typeParamNames =
            static_cast<SnClassDecl*>(pOwner)->TypeParamNames();
    }
    else if (pOwner && pOwner->Kind() == NK_InterfaceDecl
        && static_cast<SnInterfaceDecl*>(pOwner)->IsGenericDeclaration())
    {
        compiledFunc.isGeneric = true;
        compiledFunc.typeParamNames =
            static_cast<SnInterfaceDecl*>(pOwner)->TypeParamNames();
    }
    else if (func.IsGenericDeclaration())
    {
        compiledFunc.isGeneric = true;
        compiledFunc.typeParamNames = func.TypeParamNames();
    }
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

//Shared signature fill for the two body-less record families — native
//(Phase 9f) and abstract (0.13.1c): flags, locals, param count, defaults.
void VmBackend::FillSignatureOnlyRecord(SnFunction& func,
                                        CompiledFunction& compiledFunc) {
    //0.9.10: static members have no receiver slot (see AllocParamsAndDefaults).
    compiledFunc.isStatic = func.ContainFlags(NF_Static);
    FillGenericRecordMeta(func, compiledFunc);
    const bool isInstanceMethod = HasThisReceiver(func);
    //v2.16: the instance-method flag rides the record (ABI v12 native
    //bridge).
    compiledFunc.isInstanceMethod = isInstanceMethod;
    //Frame-layout locals (this + one per formal) with the true runtime
    //kinds. The native path emits no bytecode, so this is metadata-only —
    //the GC never scans a native function's frame (CallNative stages its
    //args into its own buffer). The kind-裁决 chain's frame-layout fallback
    //reads the param kind from these slots when the v1.12 descriptor
    //degrades the type (func signatures, interface types, depth-cap
    //containers), so a reference cell must carry the reference kind, not
    //the int32 placeholder. Mirrors AllocParamsAndDefaults (the non-native
    //path).
    if (isInstanceMethod)
        AllocLocal("__this", kFrameSlotBytes, ThisReceiverKind(func), true);
    for (auto& param : func.Params())
        AllocLocal(param.Name(), kFrameSlotBytes,
                   RuntimeTypeKind(param.EvalDataType()), true);
    compiledFunc.paramCount = static_cast<uint16_t>(
        func.Params().size() + (isInstanceMethod ? 1 : 0));
    compiledFunc.localsSize = compiledFunc.paramCount * kFrameSlotBytes;
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
        auto dv = ExtractDefaultValue(param.Value(),
                                      RuntimeTypeKind(param.EvalDataType()));
        if (param.Value() && !dv.hasDefault()
            && !EncodeDefaultExpr(func, param.Value(), dv))
            dv.tag = RTK_Unfoldable;
        compiledFunc.defaultValues.push_back(dv);
    }
}

//Phase 9f: `native int f(...);` — host-table dispatch (RegisterNative).
void VmBackend::FillNativeFunctionRecord(SnFunction& func,
                                         CompiledFunction& compiledFunc) {
    compiledFunc.isNative = true;
    FillSignatureOnlyRecord(func, compiledFunc);
}

//0.13.1c: `virtual int f(...);` without a body — the record IS the obligation.
void VmBackend::FillAbstractFunctionRecord(SnFunction& func,
                                           CompiledFunction& compiledFunc) {
    compiledFunc.isAbstract = true;
    FillSignatureOnlyRecord(func, compiledFunc);
}

} //namespace nlang
