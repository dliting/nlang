#pragma once
#include "BytecodeOps.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace nlang {

class BytecodeReader {
public:
    explicit BytecodeReader(const uint8_t* data, size_t size)
        : m_data(data), m_size(size), m_offset(0) {}

    OpCode ReadOp() {
        return static_cast<OpCode>(m_data[m_offset++]);
    }

    uint8_t  ReadByte()   { return m_data[m_offset++]; }

    uint16_t ReadUint16() {
        uint16_t lo = m_data[m_offset++];
        uint16_t hi = m_data[m_offset++];
        return lo | (hi << 8);
    }

    int16_t  ReadInt16()  { return static_cast<int16_t>(ReadUint16()); }

    int32_t  ReadInt32() {
        int32_t v = 0;
        for (size_t i = 0; i < sizeof(int32_t); ++i)
            v |= static_cast<int32_t>(m_data[m_offset++]) << (i * 8);
        return v;
    }

    float    ReadFloat() {
        uint32_t bits = 0;
        for (size_t i = 0; i < sizeof(uint32_t); ++i)
            bits |= static_cast<uint32_t>(m_data[m_offset++]) << (i * 8);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    size_t CurrentOffset() const { return m_offset; }
    void Seek(size_t offset) { m_offset = offset; }
    bool Eof() const { return m_offset >= m_size; }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_offset;
};

} // namespace nlang
