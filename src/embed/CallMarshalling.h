/*---
    CallMarshalling.h — call() 与宿主函数直派（⑤）共享的单元编组。
    声明 kind 驱动：string 形收取 String（铸柄，值语义）/Null（柄 0）；
    标量行经 EncodeScalarCell 严格校验（kind/值域）；参考形参（含
    List/Dict 描述符 kind）走 RefFactory——堆句柄透传或 builder 物化
    （每次跨越新对象，元素按声明 TypeDesc 校验）。形参 kind 的裁决链：
    描述符可表达者以描述符为准，NonSerialized 哨兵（func 签名等）回退
    到帧布局 locals 表（GC 同源），int32 占位是最后防线。
---*/
#pragma once
#include "Marshalling.h"
#include "RefValue.h"
#include "VmExecutor.h"
#include "nlang/embed/NLang.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nlang::embed {

//Formal i's param local — the frame-layout slot the marshalled cell
//lands in (params occupy offsets 0, 8, 16, ... of the flat frame).
inline const LocalDescriptor* ParamLocalSlot(const CompiledFunction& f,
                                             size_t i) {
    const uint16_t cellOffset = static_cast<uint16_t>(i * kFrameSlotBytes);
    for (const LocalDescriptor& ld : f.locals)
        if (ld.isParam && ld.offset == cellOffset)
            return &ld;
    return nullptr;
}

//Formal i's declared kind. The v1.12 descriptor is authoritative, but its
//not-expressible sentinel (func signatures, interface types, depth-cap
//containers) defers to the frame layout — the same locals table the GC
//scans — which carries the true runtime kind of the cell the value lands
//in. Modules predating the descriptor table fall back to int32 (the
//historical placeholder kind).
inline uint16_t DeclaredParamKind(const CompiledFunction& f, size_t i) {
    if (f.paramTypeDescs.size() > i) {
        const uint16_t kind = f.paramTypeDescs[i].type.kind;
        if (kind != RTK_NonSerialized)
            return kind;
        const LocalDescriptor* slot = ParamLocalSlot(f, i);
        return slot ? slot->typeKind
                    : static_cast<uint16_t>(RTK_Int32);
    }
    return static_cast<uint16_t>(RTK_Int32);
}

//Formal i's full descriptor — the element-kind source when the formal is
//a container and the host value a builder; nullptr when the table has none.
inline const TypeDesc* DeclaredParamType(const CompiledFunction& f, size_t i) {
    return f.paramTypeDescs.size() > i ? &f.paramTypeDescs[i].type : nullptr;
}

//Reference-kinded declarations: the four heap-slot kinds plus the
//descriptor-only container kinds (RTK_List/RTK_Dict never appear in the
//legacy fieldTypeKinds/returnTypeKind bytes).
inline bool IsReferenceKind(uint16_t k) {
    return k == RTK_Class || k == RTK_Struct || k == RTK_Array
        || k == RTK_Func || k == RTK_List || k == RTK_Dict;
}

//One host Value → one 8-byte frame cell (declaration-driven).
//declaredType (when present) supplies container element kinds for builder
//materialization and refines the reference-kind check.
//Null into a string formal writes nothing — callers hand in a zeroed cell
//so the null handle 0 reads back as a null reference.
//Reference-formal arm of ValueIntoCell: builder materialization, null,
//or rooted-handle passthrough — with the kind-consistency check at the
//marshalling boundary (not left to a misleading downstream slot guard).
inline int32_t RefHandleForFormal(VmExecutor& executor, const Value& v,
                                  uint16_t declaredKind,
                                  const TypeDesc* declaredType) {
    if (detail::RefFactory::IsBuilder(v)) {
        //Builder 只物化进容器形参（RTK_List/RTK_Dict，元素校验在
        //Materialize 内）与宽 RTK_Class。Materialize 把 NonSerialized
        //描述符视作无约束，而 Func/Struct/Array 形参的 kind 恰恰来自
        //帧布局回退、只在此处可见——必须在编组边界拒收。
        if (declaredKind != RTK_List && declaredKind != RTK_Dict
                && declaredKind != RTK_Class)
            throw BadValue(
                "builder value cannot cross into a non-container formal");
        return detail::RefFactory::Materialize(executor, v, declaredType);
    }
    if (v.kind() == Value::Kind::Null)
        return 0;   //null 引用＝句柄 0
    if (!detail::RefFactory::IsRef(v))
        throw BadValue("reference formal needs a reference, builder, "
                       "or Null host value");
    //RTK_Class 是 Object 式宽形参，容器/用户类两可不检；容器描述符
    //（可表达时）比帧布局回退的 RTK_Class 更具体，作为期望 kind 优先。
    const uint8_t expected =
        (declaredType
             && (declaredType->kind == RTK_List
                 || declaredType->kind == RTK_Dict))
            ? declaredType->kind
            : static_cast<uint8_t>(declaredKind);
    const Value::Kind vk = v.kind();
    const bool matches =
        expected == RTK_Class
        || (expected == RTK_List && vk == Value::Kind::List)
        || (expected == RTK_Dict && vk == Value::Kind::Dict)
        || (expected == RTK_Struct && vk == Value::Kind::Struct)
        || (expected == RTK_Array && vk == Value::Kind::Array)
        || (expected == RTK_Func && vk == Value::Kind::Func);
    if (!matches)
        throw BadValue("reference formal and host reference kind mismatch");
    return detail::RefFactory::HeapIdxOf(v);
}

inline void ValueIntoCell(VmExecutor& executor, const Value& v,
                          uint16_t declaredKind, uint8_t cell[8],
                          const TypeDesc* declaredType = nullptr) {
    if (declaredKind == RTK_String) {
        if (v.kind() == Value::Kind::String) {
            const int32_t handle = executor.MintHostString(v.asString());
            std::memcpy(cell, &handle, sizeof(handle));
        } else if (v.kind() != Value::Kind::Null) {
            throw BadValue("string formal needs a String or Null host value");
        }
        return;
    }
    if (IsReferenceKind(declaredKind)) {
        const int32_t handle =
            RefHandleForFormal(executor, v, declaredKind, declaredType);
        std::memcpy(cell, &handle, sizeof(handle));
        return;
    }
    if (v.kind() == Value::Kind::Null)
        throw BadValue("null host value needs a reference formal");
    EncodeScalarCell(v, declaredKind, cell);
}

//One frame cell → host Value (string copies out — value semantics; a
//null handle 0 becomes Kind::Null; scalars decode by row; reference
//kinds become rooted proxies — registration mutates the root table,
//hence a non-const executor).
inline Value ValueFromCell(VmExecutor& executor, uint16_t declaredKind,
                           const uint8_t cell[8]) {
    if (declaredKind == RTK_String) {
        int32_t handle = 0;
        std::memcpy(&handle, cell, sizeof(handle));
        if (handle == 0)
            return Value();   //null reference
        return Value(executor.StrValCopy(handle));
    }
    if (IsReferenceKind(declaredKind)) {
        int32_t handle = 0;
        std::memcpy(&handle, cell, sizeof(handle));
        if (handle == 0)
            return Value();   //null reference
        if (declaredKind == RTK_List)
            return detail::RefFactory::MakeRooted(executor, handle,
                                                  Value::Kind::List);
        if (declaredKind == RTK_Dict)
            return detail::RefFactory::MakeRooted(executor, handle,
                                                  Value::Kind::Dict);
        if (declaredKind == RTK_Array)
            return detail::RefFactory::MakeRooted(executor, handle,
                                                  Value::Kind::Array);
        if (declaredKind == RTK_Struct)
            return detail::RefFactory::MakeRooted(executor, handle,
                                                  Value::Kind::Struct);
        if (declaredKind == RTK_Func)
            return detail::RefFactory::MakeRooted(executor, handle,
                                                  Value::Kind::Func);
        //RTK_Class：擦除式 List/Dict 实例与用户类实例共享 class 槽——
        //运行时类名甄别宿主 Kind（HostClassName 的越界 runtime_error
        //经 Guarded 翻译；NLangThrow 先行重抛）
        return Guarded([&]() {
            const std::string name = executor.HostClassName(handle);
            if (name == kBuiltinListTypeName)
                return detail::RefFactory::MakeRooted(executor, handle,
                                                      Value::Kind::List);
            if (name == kBuiltinDictTypeName)
                return detail::RefFactory::MakeRooted(executor, handle,
                                                      Value::Kind::Dict);
            return detail::RefFactory::MakeRooted(executor, handle,
                                                  Value::Kind::Object);
        });
    }
    return DecodeScalarCell(cell, declaredKind);
}

//HostFn path: the callee's argument cells → host Values (the declaration
//is the marshalling source, mirroring Impl::EncodeArgs in reverse).
inline std::vector<Value> ArgsFromCells(VmExecutor& executor,
                                        const CompiledFunction& callee,
                                        const uint8_t* argCells,
                                        uint32_t argc) {
    std::vector<Value> args;
    args.reserve(argc);
    for (uint32_t i = 0; i < argc; ++i)
        args.push_back(ValueFromCell(executor, DeclaredParamKind(callee, i),
                                     argCells + i * kFrameSlotBytes));
    return args;
}

//HostFn path: the host return Value → result cell (a Void callee
//discards whatever the host returned; container descriptors hand the
//element-kind source to builder materialization).
inline void ResultIntoCell(VmExecutor& executor,
                           const CompiledFunction& callee, const Value& v,
                           uint8_t resultCell[8]) {
    if (callee.returnTypeKind == RTK_Void)
        return;
    const bool containerReturn = callee.returnTypeDesc.kind == RTK_List
                              || callee.returnTypeDesc.kind == RTK_Dict;
    ValueIntoCell(executor, v, callee.returnTypeKind, resultCell,
                  containerReturn ? &callee.returnTypeDesc : nullptr);
}

}  // namespace nlang::embed
