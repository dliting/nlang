#pragma once
#include "BytecodeOps.h"
#include <cstdint>
#include <cstring>  //std::memcpy (EmitFloat/EmitDouble bit copies)
#include <string>
#include <vector>

namespace nlang {

//Jump operands encode an absolute in-function pc as uint16 (the executor
//reads them with BytecodeReader::ReadUint16 + Seek), so one function's
//compiled code must stay under this ceiling or its branch targets wrap
//silently to low addresses. Enforced at function finalization
//(VmBackend::CheckFunctionBytecodeSize); the debug line table's u16 pc
//shares the same width (Disassembler.h).
constexpr size_t kMaxFunctionBytecodeBytes = 0x10000;

class BytecodeEmitter {
public:
    void Emit(OpCode op) {
        m_bytes.push_back(static_cast<uint8_t>(op));
    }

    //Phase 8e-1: emit a raw byte operand (e.g. type tag for OP_Box).
    void EmitByte(uint8_t v) {
        m_bytes.push_back(v);
    }

    void EmitUint16(uint16_t v) {
        m_bytes.push_back(static_cast<uint8_t>(v & 0xFF));
        m_bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }

    void EmitInt16(int16_t v) {
        EmitUint16(static_cast<uint16_t>(v));
    }

    void EmitInt32(int32_t v) {
        for (size_t i = 0; i < sizeof(int32_t); ++i) {
            m_bytes.push_back(static_cast<uint8_t>(
                (v >> (i * 8)) & 0xFF));
        }
    }

    void EmitFloat(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        for (size_t i = 0; i < sizeof(uint32_t); ++i) {
            m_bytes.push_back(static_cast<uint8_t>(
                (bits >> (i * 8)) & 0xFF));
        }
    }

    //0.7.5: 8-byte immediates for OP_ConstInt64/OP_ConstDouble (same
    //little-endian byte-at-a-time shape as EmitFloat).
    void EmitInt64(int64_t v) {
        for (size_t i = 0; i < sizeof(int64_t); ++i) {
            m_bytes.push_back(static_cast<uint8_t>(
                (v >> (i * 8)) & 0xFF));
        }
    }

    void EmitDouble(double v) {
        int64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        EmitInt64(bits);
    }

    uint16_t AddStringConstant(const std::string& s) {
        for (uint16_t i = 0; i < static_cast<uint16_t>(
            m_stringConstants.size()); ++i) {
            if (m_stringConstants[i] == s)
                return i;
        }
        m_stringConstants.push_back(s);
        return static_cast<uint16_t>(m_stringConstants.size() - 1);
    }

    void EmitStringConstant(uint16_t poolIndex) {
        Emit(OpCode::OP_ConstString);
        EmitUint16(poolIndex);
    }

    size_t CurrentOffset() const { return m_bytes.size(); }

    void PatchUint16(size_t offset, uint16_t value) {
        m_bytes[offset]     = static_cast<uint8_t>(value & 0xFF);
        m_bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    }

    std::vector<uint8_t> TakeBytes() {
        return std::move(m_bytes);
    }

    const std::vector<std::string>& StringConstants() const {
        return m_stringConstants;
    }

private:
    std::vector<uint8_t> m_bytes;
    std::vector<std::string> m_stringConstants;
};

} // namespace nlang
