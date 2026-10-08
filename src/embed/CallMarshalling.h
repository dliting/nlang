/*---
    CallMarshalling.h — call() 与宿主函数直派（⑤）共享的单元编组。
    声明 kind 驱动：string 形收取 String（铸柄，值语义）/Null（柄 0）；
    标量行经 EncodeScalarCell 严格校验（kind/值域）；参考形参的桥接
    属 Task 9——现由标量行默认分支拒绝。
---*/
#pragma once
#include "Marshalling.h"
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

//One host Value → one 8-byte frame cell (declaration-driven).
inline void ValueIntoCell(VmExecutor& executor, const Value& v,
                          uint16_t declaredKind, uint8_t cell[8]) {
    if (declaredKind == RTK_String) {
        if (v.kind() == Value::Kind::String) {
            const int32_t handle = executor.MintHostString(v.asString());
            std::memcpy(cell, &handle, sizeof(handle));
        } else if (v.kind() != Value::Kind::Null) {
            throw BadValue("string formal needs a String or Null host value");
        }
        return;
    }
    if (v.kind() == Value::Kind::Null)
        throw BadValue("null host value needs a reference formal");
    EncodeScalarCell(v, declaredKind, cell);
}

//One frame cell → host Value (string copies out — value semantics; a
//null handle 0 becomes Kind::Null; scalars decode by row).
inline Value ValueFromCell(const VmExecutor& executor, uint16_t declaredKind,
                           const uint8_t cell[8]) {
    if (declaredKind == RTK_String) {
        int32_t handle = 0;
        std::memcpy(&handle, cell, sizeof(handle));
        if (handle == 0)
            return Value();   //null reference
        return Value(executor.StrValCopy(handle));
    }
    return DecodeScalarCell(cell, declaredKind);
}

//HostFn path: the callee's argument cells → host Values (the declaration
//is the marshalling source, mirroring Impl::EncodeArgs in reverse).
inline std::vector<Value> ArgsFromCells(const VmExecutor& executor,
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
//discards whatever the host returned).
inline void ResultIntoCell(VmExecutor& executor,
                           const CompiledFunction& callee, const Value& v,
                           uint8_t resultCell[8]) {
    if (callee.returnTypeKind == RTK_Void)
        return;
    ValueIntoCell(executor, v, callee.returnTypeKind, resultCell);
}

}  // namespace nlang::embed
