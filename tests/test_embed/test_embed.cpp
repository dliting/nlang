//Embedding API unit tests: Value scalar semantics and the exception
//hierarchy. Reference-kind construction arrives with the interpreter
//tasks; this file pins the host-side value model (spec §5/§6).
#include "nlang/embed/NLang.h"
#include <cstdint>
#include <iostream>
#include <string>

using namespace nlang;

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

int main() {
    TestDefaultIsNull();
    TestScalarRoundTrip();
    TestCharLiteralOverload();
    TestAccessorMismatchThrowsBadValue();
    TestExceptionHierarchy();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
