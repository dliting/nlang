//Embedding API unit tests: Value scalar semantics and the exception
//hierarchy. Reference-kind construction arrives with the interpreter
//tasks; this file pins the host-side value model (spec §5/§6).
#include "nlang/embed/NLang.h"
#include "Marshalling.h"   //src/embed 内部头（tests 目标 PRIVATE include）
#include <cstdint>
#include <iostream>
#include <string>

using namespace nlang;
using namespace nlang::embed;   //EncodeScalarCell/DecodeScalarCell

static int g_pass = 0, g_fail = 0;
#define TEST(name) do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)
#define CHECK_THROWS(excType, expr, msg) \
    do { bool threw = false; \
         try { (void)(expr); } catch (const excType&) { threw = true; } \
         if (!threw) { FAIL(msg); return; } } while(0)

static void TestDefaultIsNull() {
    TEST(TestDefaultIsNull);
    Value v;
    CHECK(v.kind() == Value::Kind::Null, "default Value must be Null");
    PASS();
}

static void TestScalarRoundTrip() {
    TEST(TestScalarRoundTrip);
    CHECK(Value(int32_t(-7)).kind() == Value::Kind::Int, "int32 kind");
    CHECK(Value(int32_t(-7)).asInt() == -7, "int32 payload");
    CHECK(Value(int64_t(1) << 40).kind() == Value::Kind::Long, "int64 kind");
    CHECK(Value(int64_t(1) << 40).asLong() == (int64_t(1) << 40), "int64 payload");
    CHECK(Value(2.5f).kind() == Value::Kind::Float, "float kind");
    CHECK(Value(2.5f).asFloat() == 2.5f, "float payload");
    CHECK(Value(0.25).kind() == Value::Kind::Double, "double kind");
    CHECK(Value(0.25).asDouble() == 0.25, "double payload");
    CHECK(Value(false).asBool() == false, "bool payload");
    CHECK(Value(char32_t(U'中')).kind() == Value::Kind::Char, "char kind");
    CHECK(Value(char32_t(U'中')).asChar() == U'中', "char payload (Unicode scalar)");
    CHECK(Value(std::string("héllo")).asString() == "héllo", "string payload UTF-8");
    PASS();
}

static void TestCharLiteralOverload() {
    TEST(TestCharLiteralOverload);
    //explicit const char* overload must win over the (bool) trap
    Value v("yes");
    CHECK(v.kind() == Value::Kind::String, "literal must be String, not Bool");
    CHECK(v.asString() == "yes", "literal payload");
    PASS();
}

static void TestAccessorMismatchThrowsBadValue() {
    TEST(TestAccessorMismatchThrowsBadValue);
    CHECK_THROWS(BadValue, Value(std::string("x")).asInt(),
        "asInt on String must throw BadValue");
    CHECK_THROWS(BadValue, Value(int32_t(1)).asString(),
        "asString on Int must throw BadValue");
    CHECK_THROWS(BadValue, Value(1.5).asBool(),
        "asBool on Double must throw BadValue");
    CHECK_THROWS(BadValue, Value(true).asDouble(),
        "asDouble on Bool must throw BadValue");
    PASS();
}

static void TestExceptionHierarchy() {
    TEST(TestExceptionHierarchy);
    Exception ex("boom", "  at main (demo.n:3)", "DivideByZeroException");
    CHECK(ex.message() == "boom", "Exception message");
    CHECK(ex.backtrace() == "  at main (demo.n:3)", "Exception backtrace");
    CHECK(ex.exceptionClass() == "DivideByZeroException", "Exception class");
    CHECK(std::string(ex.what()).find("boom") != std::string::npos,
        "what() carries the message");
    //Taxonomy: BadValue is a logic_error, LoadError a runtime_error
    CHECK_THROWS(BadValue, throw BadValue("x"), "BadValue throwable");
    CHECK_THROWS(LoadError, throw LoadError("y"), "LoadError throwable");
    PASS();
}

static void TestScalarEncodeDecodeRoundTrip() {
    TEST(TestScalarEncodeDecodeRoundTrip);
    using K = uint16_t;   //RTK_*
    struct Row { K kind; Value v; };
    //12 基元行（RTK 常量值见 CompiledModule.h:88-98 与 PrimitiveTypes 表；
    //用 nlang::RTK_* 常量，不写数字）
    //RTK names pinned at PrimitiveTypes.h:79-90: RTK_Short=12/
    //RTK_UShort=13/RTK_Long=15/RTK_ULong=16 — there are no
    //RTK_Int16/UInt16/Int64/UInt64 constants.
    const Row rows[] = {
        {RTK_Byte,   Value(int32_t(-100))},
        {RTK_UByte,  Value(int32_t(200))},
        {RTK_Short,  Value(int32_t(-30000))},
        {RTK_UShort, Value(int32_t(60000))},
        {RTK_Int32,  Value(int32_t(-123456))},
        {RTK_UInt32, Value(int64_t(4000000000LL))},   //宿主 Long 承载宽 uint
        {RTK_Long,   Value(int64_t(-5000000000LL))},
        {RTK_ULong,  Value(int64_t(12000000000LL))},
        {RTK_Float,  Value(1.5f)},
        {RTK_Double, Value(2.25)},
        {RTK_Bool,   Value(true)},
        {RTK_Char,   Value(char32_t(U'文'))},
    };
    for (const auto& r : rows) {
        uint8_t cell[8] = {0};
        EncodeScalarCell(r.v, r.kind, cell);
        Value back = DecodeScalarCell(cell, r.kind);
        CHECK(back.kind() == r.v.kind(), "round-trip kind");
        //Compare by the DECODED kind — the accessors throw BadValue on
        //other kinds (Task 1 CheckKind contract), so a fixed pair like
        //asLong()/asDouble() would terminate the test instead of
        //failing it on 10 of the 12 rows.
        bool payloadEqual = false;
        switch (back.kind()) {
        case Value::Kind::Int:    payloadEqual = back.asInt() == r.v.asInt(); break;
        case Value::Kind::Long:   payloadEqual = back.asLong() == r.v.asLong(); break;
        case Value::Kind::Float:  payloadEqual = back.asFloat() == r.v.asFloat(); break;
        case Value::Kind::Double: payloadEqual = back.asDouble() == r.v.asDouble(); break;
        case Value::Kind::Bool:   payloadEqual = back.asBool() == r.v.asBool(); break;
        case Value::Kind::Char:   payloadEqual = back.asChar() == r.v.asChar(); break;
        default: break;
        }
        CHECK(payloadEqual, "round-trip payload");
    }
    PASS();
}

static void TestNarrowingOverflowThrows() {
    TEST(TestNarrowingOverflowThrows);
    uint8_t cell[8];
    CHECK_THROWS(BadValue, EncodeScalarCell(Value(int32_t(300)), RTK_Byte, cell),
        "300 into byte must throw BadValue, not truncate");
    CHECK_THROWS(BadValue, EncodeScalarCell(Value(int32_t(70000)), RTK_UShort, cell),
        "70000 into ushort must throw");
    CHECK_THROWS(BadValue, EncodeScalarCell(Value(int64_t(5000000000LL)), RTK_Int32, cell),
        "wide long into int32 must throw");
    CHECK_THROWS(BadValue, EncodeScalarCell(Value(int64_t(-1)), RTK_ULong, cell),
        "negative into unsigned 64 must throw");
    //合法边界不抛
    EncodeScalarCell(Value(int32_t(127)), RTK_Byte, cell);
    EncodeScalarCell(Value(int32_t(255)), RTK_UByte, cell);
    PASS();
}

static void TestKindMismatchThrows() {
    TEST(TestKindMismatchThrows);
    uint8_t cell[8];
    CHECK_THROWS(BadValue, EncodeScalarCell(Value(std::string("x")), RTK_Int32, cell),
        "String into int32 formal must throw");
    CHECK_THROWS(BadValue, EncodeScalarCell(Value(1.5), RTK_Int32, cell),
        "Double into int32 formal must throw (no implicit narrowing)");
    PASS();
}

int main() {
    TestDefaultIsNull();
    TestScalarRoundTrip();
    TestCharLiteralOverload();
    TestAccessorMismatchThrowsBadValue();
    TestExceptionHierarchy();
    TestScalarEncodeDecodeRoundTrip();
    TestNarrowingOverflowThrows();
    TestKindMismatchThrows();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
