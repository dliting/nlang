/*---
    Marshalling.cpp — 标量行实现（值域常量＋按行宽读写）。
---*/
#include "Marshalling.h"
#include <cstring>
#include <string>

namespace nlang::embed {

//行宽值域（不写 magic number）
constexpr int64_t kInt8Min  = -128;
constexpr int64_t kInt8Max  = 127;
constexpr int64_t kUint8Max = 255;
constexpr int64_t kInt16Min = -32768;
constexpr int64_t kInt16Max = 32767;
constexpr int64_t kUint16Max = 65535;
constexpr int64_t kInt32Min = -2147483648LL;
constexpr int64_t kInt32Max = 2147483647LL;
constexpr int64_t kUint32Max = 4294967295LL;

//按 v 自身的 kind 取整型位（CheckKind 契约：错配在此抛 BadValue）
static int64_t ScalarBits(const Value& v) {
    switch (v.kind()) {
    case Value::Kind::Int:  return v.asInt();
    case Value::Kind::Long: return v.asLong();
    case Value::Kind::Bool: return v.asBool() ? 1 : 0;
    case Value::Kind::Char: return static_cast<int64_t>(v.asChar());
    default:
        throw BadValue("scalar row needs an integer-kind host value");
    }
}

//低半格 4 字节或整格 8 字节（帧单元 ABI：窄标量占低半格）
static void WriteCell(uint8_t outCell[8], int64_t bits, int bytes) {
    std::memcpy(outCell, &bits, static_cast<size_t>(bytes));
}

static int64_t ReadCell(const uint8_t cell[8], int bytes) {
    if (bytes == 8) {
        int64_t bits = 0;
        std::memcpy(&bits, cell, sizeof(bits));
        return bits;
    }
    //窄行以 int32 视图读回（符号由编码侧保证）
    int32_t narrow = 0;
    std::memcpy(&narrow, cell, sizeof(narrow));
    return narrow;
}

//值域校验＋写入；行名进诊断消息
static void EncodeSigned(Value v, int64_t lo, int64_t hi,
                         const char* rowName, uint8_t cell[8]) {
    const int64_t bits = ScalarBits(v);
    if (bits < lo || bits > hi)
        throw BadValue("value " + std::to_string(bits)
            + " does not fit declared " + rowName);
    WriteCell(cell, bits, 4);
}

static void EncodeUnsigned(Value v, int64_t hi,
                           const char* rowName, uint8_t cell[8]) {
    const int64_t bits = ScalarBits(v);
    if (bits < 0 || bits > hi)
        throw BadValue("value " + std::to_string(bits)
            + " does not fit declared " + rowName);
    WriteCell(cell, bits, 4);
}

//kind 族校验（声明驱动严格性：Float 行不收 Double，无隐式窄化）
static void RequireKind(Value::Kind actual, Value::Kind expected,
                        const char* rowName) {
    if (actual != expected)
        throw BadValue(std::string("host kind does not match declared ")
            + rowName);
}

void EncodeScalarCell(const Value& v, uint16_t declaredKind, uint8_t outCell[8]) {
    switch (declaredKind) {
    case RTK_Byte:   EncodeSigned(v, kInt8Min, kInt8Max, "byte", outCell); break;
    case RTK_UByte:  EncodeUnsigned(v, kUint8Max, "ubyte", outCell); break;
    case RTK_Short:  EncodeSigned(v, kInt16Min, kInt16Max, "short", outCell); break;
    case RTK_UShort: EncodeUnsigned(v, kUint16Max, "ushort", outCell); break;
    case RTK_Int32:  EncodeSigned(v, kInt32Min, kInt32Max, "int", outCell); break;
    case RTK_UInt32: EncodeUnsigned(v, kUint32Max, "uint", outCell); break;
    case RTK_Long: {
        //64 位行收 Int 或 Long（宽化安全）
        if (v.kind() != Value::Kind::Int && v.kind() != Value::Kind::Long)
            throw BadValue("host kind does not match declared long");
        WriteCell(outCell, ScalarBits(v), 8);
        break;
    }
    case RTK_ULong: {
        if (v.kind() != Value::Kind::Int && v.kind() != Value::Kind::Long)
            throw BadValue("host kind does not match declared ulong");
        const int64_t bits = ScalarBits(v);
        if (bits < 0)
            throw BadValue("value " + std::to_string(bits)
                + " does not fit declared ulong");
        WriteCell(outCell, bits, 8);
        break;
    }
    case RTK_Float: {
        RequireKind(v.kind(), Value::Kind::Float, "float");
        const float f = v.asFloat();
        std::memcpy(outCell, &f, sizeof(f));
        break;
    }
    case RTK_Double: {
        RequireKind(v.kind(), Value::Kind::Double, "double");
        WriteCell(outCell, 0, 8);   //清零避免读未初始化
        const double d = v.asDouble();
        std::memcpy(outCell, &d, sizeof(d));
        break;
    }
    case RTK_Bool: {
        RequireKind(v.kind(), Value::Kind::Bool, "bool");
        WriteCell(outCell, v.asBool() ? 1 : 0, 4);
        break;
    }
    case RTK_Char: {
        RequireKind(v.kind(), Value::Kind::Char, "char");
        WriteCell(outCell, static_cast<int64_t>(v.asChar()), 4);
        break;
    }
    default:
        throw BadValue("unsupported declared scalar kind "
            + std::to_string(declaredKind));
    }
}

Value DecodeScalarCell(const uint8_t cell[8], uint16_t declaredKind) {
    switch (declaredKind) {
    case RTK_Byte: case RTK_Short: case RTK_Int32:
        return Value(static_cast<int32_t>(ReadCell(cell, 4)));
    case RTK_UByte: case RTK_UShort:
        return Value(static_cast<int32_t>(ReadCell(cell, 4)));
    case RTK_UInt32: {
        //uint 超 int32 范围 → Long（宿主无 uint32 kind）
        const uint32_t u = static_cast<uint32_t>(ReadCell(cell, 4));
        if (u > 2147483647u)
            return Value(static_cast<int64_t>(u));
        return Value(static_cast<int32_t>(u));
    }
    case RTK_Long: case RTK_ULong:
        return Value(ReadCell(cell, 8));
    case RTK_Float: {
        float f = 0.0f;
        std::memcpy(&f, cell, sizeof(f));
        return Value(f);
    }
    case RTK_Double: {
        double d = 0.0;
        std::memcpy(&d, cell, sizeof(d));
        return Value(d);
    }
    case RTK_Bool:
        return Value(ReadCell(cell, 4) != 0);
    case RTK_Char:
        return Value(static_cast<char32_t>(ReadCell(cell, 4)));
    default:
        throw BadValue("unsupported declared scalar kind "
            + std::to_string(declaredKind));
    }
}

}  // namespace nlang::embed
