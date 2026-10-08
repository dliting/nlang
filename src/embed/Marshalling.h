/*---
    Marshalling.h — 宿主 Value ↔ 8 字节帧单元（声明驱动，spec §13）。
    标量行在此；参考类型（string mint/list-dict 物化/句柄透传）经
    VmExecutor 值桥，散布在 InterpreterImpl/HostFunctionTable/Proxies。
---*/
#pragma once
#include "nlang/embed/NLang.h"
#include "nlang/runtime/PrimitiveTypes.h"
#include "nlang/vm/CompiledModule.h"
#include <cstdint>

namespace nlang::embed {

//把宿主标量编码进 8 字节帧单元。declaredKind 是被调声明的 RTK。
//kind 不匹配或窄化越界抛 BadValue（不静默截断）。
//Int 宿主值可编码一切有符号窄行（值域校验）；Long 承载 64 位行与
//超 int32 的无符号行；Float/Double/Bool/Char 一一对应。
void EncodeScalarCell(const Value& v, uint16_t declaredKind, uint8_t outCell[8]);

//按声明 RTK 解码 8 字节帧单元为宿主值。窄行折叠：byte/ubyte/short/
//ushort/int/uint → Int（uint 超 int32 范围 → Long）；long/ulong → Long；
//float → Float；double → Double；bool → Bool；char → Char（Unicode 标量）。
Value DecodeScalarCell(const uint8_t cell[8], uint16_t declaredKind);

}  // namespace nlang::embed
