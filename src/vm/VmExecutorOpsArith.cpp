/*---
    VmExecutorOpsArith.cpp — 整数/浮点算术、比较、逻辑与数值转换操作码
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::OpCastIntToFloat(uint8_t* pResult) {
    int32_t iv;
    std::memcpy(&iv, pResult, sizeof(iv));
    float fv = static_cast<float>(iv);
    std::memcpy(pResult, &fv, sizeof(fv));
}

void VmExecutor::OpCastFloatToInt(uint8_t* pResult) {
    float fv;
    std::memcpy(&fv, pResult, sizeof(fv));
    int32_t iv = static_cast<int32_t>(fv);
    std::memcpy(pResult, &iv, sizeof(iv));
}

void VmExecutor::OpInt32_to_str(uint8_t* pResult) {
    int32_t iv;
    std::memcpy(&iv, pResult, sizeof(iv));
    int32_t handle = MintNewString(std::to_string(iv));
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpFloat_to_str(uint8_t* pResult) {
    float fv;
    std::memcpy(&fv, pResult, sizeof(fv));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", fv);
    int32_t handle = MintNewString(buf);
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpAdd_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    a += b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpSub_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    a -= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpMul_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    a *= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpDiv_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    if (b == 0)
        RaiseNlangException(m_divZeroExcClassIdx,
                            "NLang VM: division by zero");
    a /= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpMod_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    if (b == 0)
        RaiseNlangException(m_divZeroExcClassIdx,
                            "NLang VM: modulo by zero");
    a %= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpNeg_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    int32_t a;
    std::memcpy(&a, locals + dst, sizeof(a));
    a = -a;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpAdd_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    a += b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpSub_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    a -= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpMul_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    a *= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpDiv_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + dst, sizeof(a));
    std::memcpy(&b, locals + src, sizeof(b));
    if (b == 0.0f)
        RaiseNlangException(m_divZeroExcClassIdx,
                            "NLang VM: division by zero");
    a /= b;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpNeg_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    float a;
    std::memcpy(&a, locals + dst, sizeof(a));
    a = -a;
    std::memcpy(locals + dst, &a, sizeof(a));
}

void VmExecutor::OpLess_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a < b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpLessEqual_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a <= b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpGreater_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a > b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpGreaterEqual_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a >= b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpEqual_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a == b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpNotEqual_i32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a != b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpLess_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a < b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpLessEqual_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a <= b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpGreater_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a > b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpGreaterEqual_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a >= b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpEqual_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a == b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpNotEqual_f32(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    float a, b;
    std::memcpy(&a, locals + lhs, sizeof(a));
    std::memcpy(&b, locals + rhs, sizeof(b));
    int32_t r = (a != b) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpLogicalNot(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    int32_t a;
    std::memcpy(&a, locals + dst, sizeof(a));
    a = !a;
    std::memcpy(locals + dst, &a, sizeof(a));
}

} // namespace nlang
