/*---
    CallMarshalling.h — call() 与宿主函数直派（⑤）共享的单元编组。
    声明 kind 驱动：string 形收取 String（铸柄，值语义）/Null（柄 0）；
    标量行经 EncodeScalarCell 严格校验（kind/值域）；参考形参（含
    List/Dict 描述符 kind）走 RefFactory——堆句柄透传或 builder 物化
    （每次跨越新对象，元素按声明 TypeDesc 校验）。
---*/
#pragma once
#include "Marshalling.h"
#include "RefValue.h"
#include "VmExecutor.h"
#include "nlang/embed/NLang.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace nlang::embed {

//Formal i's declared kind; modules predating the descriptor table fall
//back to int32 (the historical placeholder kind).
inline uint16_t DeclaredParamKind(const CompiledFunction& f, size_t i) {
    return f.paramTypeDescs.size() > i ? f.paramTypeDescs[i].type.kind
                                       : static_cast<uint16_t>(RTK_Int32);
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
//materialization and the container/reference mismatch check.
//Null into a string formal writes nothing — callers hand in a zeroed cell
//so the null handle 0 reads back as a null reference.
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
        int32_t handle = 0;
        if (detail::RefFactory::IsBuilder(v)) {
            handle = detail::RefFactory::Materialize(executor, v, declaredType);
        } else if (v.kind() == Value::Kind::Null) {
            handle = 0;   //null 引用＝句柄 0
        } else if (detail::RefFactory::IsRef(v)) {
            handle = detail::RefFactory::HeapIdxOf(v);
            //容器描述符与宿主参考 kind 错配（RTK_Class 是 Object 式宽形参，
            //容器/用户类两可，不在此列）
            if (declaredType
                    && ((declaredType->kind == RTK_List
                            && v.kind() != Value::Kind::List)
                        || (declaredType->kind == RTK_Dict
                            && v.kind() != Value::Kind::Dict)))
                throw BadValue(
                    "container formal and host reference kind mismatch");
        } else {
            throw BadValue("reference formal needs a reference, builder, "
                           "or Null host value");
        }
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
            if (name == "List")
                return detail::RefFactory::MakeRooted(executor, handle,
                                                      Value::Kind::List);
            if (name == "Dict")
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
