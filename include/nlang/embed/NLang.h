/*---
    NLang.h — C++ 宿主嵌入 API 公共表面（spec 2026-10-08）。
    不透明句柄纪律：本头不引入 VM 内部类型；参考类型的内部状态
    （解释器/堆句柄）藏在 detail::RefValue（头内仅前向声明）。
---*/
#pragma once
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

namespace detail { struct RefValue; }

class ArrayProxy;
class ListProxy;
class DictProxy;
class ObjectProxy;

//类型明确的宿主值。标量不可变；参考类型是 NLang 堆驻留值的读写代理。
//Kind 按存储宽度分档（宿主清洁枚举）：NLang 细粒度标量（byte/ubyte/
//short/ushort/uint/ulong 等）由 marshalling 层折叠到 Int/Long；enum 折叠
//到 Int；装箱记录透明拆箱；RTK_Null → Kind::Null。
class Value {
public:
    Value();                          // Null
    explicit Value(int32_t v);        explicit Value(int64_t v);
    explicit Value(float v);          explicit Value(double v);
    explicit Value(bool v);           explicit Value(char32_t v);
    explicit Value(const std::string& v);  // UTF-8
    explicit Value(const char* v);         // 字面量便捷重载（防静默选中 bool）

    enum class Kind { Null, Int, Long, Float, Double, Bool, Char,
                      String, Array, List, Dict, Object, Struct, Func };

    Kind kind() const;
    //标量访问器：kind 不匹配抛 BadValue。非定宽整型实参（如 uint32_t
    //变量）在重载间歧义，须显式定宽（Value(int32_t(x)) 等）。
    int32_t   asInt()   const;   int64_t  asLong()   const;
    float     asFloat() const;   double   asDouble() const;
    bool      asBool()  const;   char32_t asChar()   const;
    std::string asString() const;   // UTF-8
    //参考类型访问器：kind 不匹配抛 BadValue。返回读写代理（操作堆驻留
    //NLang 值，改动对 NLang 代码可见）；宿主构造的 builder 值（newList/
    //newDict）的代理写在宿主侧存储（spec §6 builder 语义）。
    ArrayProxy  asArray()  const;
    ListProxy   asList()   const;
    DictProxy   asDict()   const;
    ObjectProxy asObject() const;
    //Struct/Func：v1 仅 kind() 可观测、可作值透传（marshalling 原样搬运）；
    //asStruct()/asFunc() 与字段遍历保留为未来增量（非破坏）。

private:
    friend class InterpreterImpl;   //构造参考值的唯一入口在适配层内
    Kind m_kind = Kind::Null;
    int64_t m_scalarBits = 0;       //Int/Long/Bool/Char（Char=Unicode 标量）
    double m_double = 0.0;          //Float/Double
    std::string m_string;           //String（不可变语义＝宿主侧拷贝）
    std::shared_ptr<detail::RefValue> m_ref;  //参考类型（含 builder 存储）
};

//读写代理（spec §6 表）。持 shared_ptr<detail::RefValue>——与源 Value
//共享同一记录（浅拷贝别名，双向可见）；builder 值的代理写宿主侧存储。
//索引一律 int32_t：负值 → BadValue（不静默回绕成大无符号数）。
class ArrayProxy {
public:
    uint32_t size() const;                 // 定长（数组非动态容量）
    Value get(int32_t index) const;        // 越界/负索引 → BadValue
    void set(int32_t index, const Value& v);
private:
    friend class Value;
    explicit ArrayProxy(std::shared_ptr<detail::RefValue> ref);
    std::shared_ptr<detail::RefValue> m_ref;
};

class ListProxy {
public:
    uint32_t size() const;
    Value get(int32_t index) const;
    void set(int32_t index, const Value& v);
    void add(const Value& v);
    void removeAt(int32_t index);
    void clear();
private:
    friend class Value;
    explicit ListProxy(std::shared_ptr<detail::RefValue> ref);
    std::shared_ptr<detail::RefValue> m_ref;
};

class DictProxy {
public:
    bool containsKey(const Value& key) const;
    bool containsKey(const char* key) const;      // string 键便捷重载
    Value get(const Value& key) const;
    Value get(const char* key) const;
    void set(const Value& key, const Value& v);
    void set(const char* key, const Value& v);
    void remove(const Value& key);
    void remove(const char* key);
    void clear();
    std::vector<Value> keys() const;   //键作 Value 返回（键可为任意标量）
private:
    friend class Value;
    explicit DictProxy(std::shared_ptr<detail::RefValue> ref);
    std::shared_ptr<detail::RefValue> m_ref;
};

class ObjectProxy {
public:
    Value getField(const char* name) const;
    Value getField(const std::string& name) const;
    void setField(const char* name, const Value& v);
    void setField(const std::string& name, const Value& v);
private:
    friend class Value;
    explicit ObjectProxy(std::shared_ptr<detail::RefValue> ref);
    std::shared_ptr<detail::RefValue> m_ref;
};

//宿主用法错误：类型错配、生命周期误用（二次 load/run、load 后
//addImportDir、再入）、marshalling 不匹配（kind/值域）。
class BadValue : public std::logic_error {
public: using std::logic_error::logic_error;
};

//载入/环境失败（坏路径、坏产物、闭包解析失败、无 main、原生绑定缺失
//——最后一项惰性检出，无论何时检出均映射本类，见 spec §5）。
class LoadError : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};

using WriteFn = std::function<void(const char* text)>;        // I/O 重定向（UTF-8）
using HostFn  = std::function<Value(const std::vector<Value>& args)>; // 宿主函数

class Interpreter {
public:
    Interpreter();              // 幂等 initialize + 建 VmExecutor + 宿主函数表
    ~Interpreter();             // 拆 executor（不拆进程级 Runtime）
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void load(const std::filesystem::path& artifact);  // .ncu 或 .npkg；仅一次
    void addImportDir(const std::filesystem::path& d); // 须在 load() 之前
    void registerHostFunction(const char* ns, const char* name, HostFn fn);

    int   run();                // 调模块 main()，返回退出码；至多一次；未捕获异常抛 Exception
    Value call(const char* funcName, const std::vector<Value>& args); // 通用基元

    Value newList();            // 宿主构造 List（builder 语义，spec §6）
    Value newDict();            // 宿主构造 Dict（builder 语义，spec §6）

    void setOutputHandler(WriteFn out, WriteFn err);   // 只重定向输出通道

private:
    struct Impl;
    std::unique_ptr<Impl> m_upImpl;
};

//未捕获的 NLang 异常（脚本侧错误）。定义在 Interpreter 之后、友元指名
//外围类：写 m_heapIdx 的是嵌套 Interpreter::Impl 的成员（翻译
//NLangThrow），嵌套类经外围类的友元资格获得访问（嵌套类成员与外围类
//成员同权，[class.access.nest]）；指名私有嵌套类型本身则不可行。
class Exception : public std::exception {
public:
    Exception(std::string message, std::string backtrace,
              std::string exceptionClass);
    const char* what() const noexcept override;
    std::string message()        const;
    std::string backtrace()      const;   //栈回溯文本
    std::string exceptionClass() const;   // "Exception"/"IOException"/...
private:
    friend class Interpreter;         //经嵌套 Impl 成员构造（翻译 NLangThrow，置 heapIdx）
    friend class HostFunctionTable;   //读 m_heapIdx 决定原样重抛（Task 7）
    std::string m_message, m_backtrace, m_class;
    int32_t m_heapIdx = 0;   //>0＝可原样重抛的 NLang 异常实例（⑤双向语义）
};

void initialize();  // 幂等，包 Runtime::StaticInit
void shutdown();    // 幂等，包 Runtime::StaticFini；经 atexit 自动注册

}  // namespace nlang
