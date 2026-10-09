/*---
    Value.cpp — 宿主值标量实现＋异常实现。
    参考值内部状态与工厂声明在 RefValue.h（Proxies.cpp 定义工厂）。
---*/
#include "RefValue.h"
#include <cstring>
#include <new>
#include <utility>

namespace nlang {

//诊断用 Kind 名（BadValue 消息；公共头不暴露——纯实现细节）
const char* KindName(Value::Kind k) {
    switch (k) {
    case Value::Kind::Null: return "Null";   case Value::Kind::Int: return "Int";
    case Value::Kind::Long: return "Long";   case Value::Kind::Float: return "Float";
    case Value::Kind::Double: return "Double"; case Value::Kind::Bool: return "Bool";
    case Value::Kind::Char: return "Char";   case Value::Kind::String: return "String";
    case Value::Kind::Array: return "Array"; case Value::Kind::List: return "List";
    case Value::Kind::Dict: return "Dict";   case Value::Kind::Object: return "Object";
    case Value::Kind::Struct: return "Struct"; case Value::Kind::Func: return "Func";
    }
    return "?";
}

//--- Value 标量 ---------------------------------------------------------

Value::Value() = default;
Value::Value(int32_t v)  : m_kind(Kind::Int),   m_scalarBits(v) {}
Value::Value(int64_t v)  : m_kind(Kind::Long),  m_scalarBits(v) {}
Value::Value(bool v)     : m_kind(Kind::Bool),  m_scalarBits(v ? 1 : 0) {}
Value::Value(char32_t v) : m_kind(Kind::Char),  m_scalarBits(static_cast<int64_t>(v)) {}
Value::Value(float v)    : m_kind(Kind::Float), m_double(static_cast<double>(v)) {}
Value::Value(double v)   : m_kind(Kind::Double),m_double(v) {}
Value::Value(const std::string& v) : m_kind(Kind::String), m_string(v) {}
Value::Value(const char* v)
    : m_kind(Kind::String), m_string(v ? v : "") {}

Value::Kind Value::kind() const { return m_kind; }

void CheckKind(Value::Kind actual, Value::Kind expected,
               const char* accessor) {
    if (actual != expected)
        throw BadValue(std::string(accessor) + ": value is "
            + KindName(actual) + ", not " + KindName(expected));
}

int32_t Value::asInt() const {
    CheckKind(m_kind, Kind::Int, "asInt");
    return static_cast<int32_t>(m_scalarBits);
}
int64_t Value::asLong() const {
    CheckKind(m_kind, Kind::Long, "asLong");
    return m_scalarBits;
}
float Value::asFloat() const {
    CheckKind(m_kind, Kind::Float, "asFloat");
    return static_cast<float>(m_double);
}
double Value::asDouble() const {
    CheckKind(m_kind, Kind::Double, "asDouble");
    return m_double;
}
bool Value::asBool() const {
    CheckKind(m_kind, Kind::Bool, "asBool");
    return m_scalarBits != 0;
}
char32_t Value::asChar() const {
    CheckKind(m_kind, Kind::Char, "asChar");
    return static_cast<char32_t>(m_scalarBits);
}
std::string Value::asString() const {
    CheckKind(m_kind, Kind::String, "asString");
    return m_string;
}

//参考访问器在 Proxies.cpp 定义（Task 9）。

//--- 异常 ---------------------------------------------------------------

Exception::Exception(std::string message, std::string backtrace,
                     std::string exceptionClass)
    : m_message(std::move(message)), m_backtrace(std::move(backtrace)),
      m_class(std::move(exceptionClass)) {}

const char* Exception::what() const noexcept {
    //消息即 what（异常类名单独经 exceptionClass()）
    return m_message.c_str();
}
std::string Exception::message() const { return m_message; }
std::string Exception::backtrace() const { return m_backtrace; }
std::string Exception::exceptionClass() const { return m_class; }

}  // namespace nlang
