/*---
    VmExecutorOpsArith.cpp — 逻辑非操作码（其余算术/比较/转换家族已由
    0.7.5 泛化指令族迁往 VmExecutorOpsPrim.cpp 的注册表分派表）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::OpLogicalNot(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    int32_t a;
    std::memcpy(&a, locals + dst, sizeof(a));
    a = !a;
    std::memcpy(locals + dst, &a, sizeof(a));
}

} // namespace nlang
