/*---
    RefValue.h — 参考值的内部状态与构造/装箱工厂（embed 层内部头）。
    RefValue 定义自 Value.cpp 移此：Proxies/InterpreterImpl/
    CallMarshalling 三方共享；公共头仍只见前向声明（不透明句柄纪律）。
---*/
#pragma once
#include "VmExecutor.h"
#include "nlang/embed/NLang.h"
#include "nlang/vm/TypeDesc.h"
#include <memory>
#include <vector>

namespace nlang {

//诊断用 Kind 名与 kind 校验（BadValue 消息源；定义在 Value.cpp，
//标量访问器与参考访问器共用）
const char* KindName(Value::Kind k);
void CheckKind(Value::Kind actual, Value::Kind expected,
               const char* accessor);

//桥翻译守卫：vm 层只抛 std::runtime_error；BadValue（logic_error）
//原样穿透。NLangThrow 继承 runtime_error（VmExecutor.h），须先于
//runtime_error 捕获重抛——否则脚本异常会被误译成宿主用法错误。
template <typename Body>
auto Guarded(Body body) -> decltype(body()) {
    try {
        return body();
    } catch (const NLangThrow&) {
        throw;
    } catch (const std::runtime_error& e) {
        throw BadValue(e.what());
    }
}

namespace detail {

//参考值的内部状态：解释器反引用＋堆句柄＋GC 根登记（扩展点③）。
//有根引用在构造时 PushHostRoot(RTK_Class, heapIdx)、最后一个共享副本
//析构时对称 Pop——登记 kind 恒 RTK_Class：根表只按 string/堆二路分流，
//参考值恒为堆 idx，永不 RTK_String。
//builder 存储（isBuilder=true 时 heapIdx 恒 0、不入根表）：Dict builder
//的 builderElements 按 [k0,v0,k1,v1,...] 成对存放。
struct RefValue {
    VmExecutor* executor = nullptr;
    int32_t heapIdx = 0;
    std::vector<Value> builderElements;
    bool isBuilder = false;

    ~RefValue() {
        if (!isBuilder && executor != nullptr && heapIdx > 0)
            executor->PopHostRoot(RTK_Class, heapIdx);
    }
};

//参考值构造与元素装箱的唯一入口（Value 的友元——宿主代码与适配层
//其余部分皆不可直达私有状态）。vm 桥的 std::runtime_error 由代理层
//翻译为 BadValue；本层自产校验直接抛 BadValue。
class RefFactory {
public:
    //构造有根参考值（已登记 GC 根）
    static Value MakeRooted(VmExecutor& exec, int32_t heapIdx,
                            Value::Kind kind);
    //构造 builder 值（kind 限 List/Dict——调用点 newList/newDict）
    static Value MakeBuilder(VmExecutor& exec, Value::Kind kind);
    static bool IsRef(const Value& v);       // m_ref 非空
    static bool IsBuilder(const Value& v);   // m_ref->isBuilder
    static int32_t HeapIdxOf(const Value& v);
    //一个宿主 Value → 堆句柄：标量/字符串装箱（String＝mint 柄入箱）、
    //参考值句柄直传、嵌套 builder 递归物化（无元素声明可校）
    static int32_t ToHeapHandle(VmExecutor& exec, const Value& v);
    //堆句柄 → 宿主 Value：装箱记录按 tag 解、class/struct/array/func
    //槽构造有根参考值（擦除 List/Dict 实例按运行时类名甄别宿主 Kind）
    static Value FromHeapHandle(VmExecutor& exec, int32_t heapIdx);
    //builder 物化：每次跨越新对象（无缓存）。declaredType 供元素 kind
    //校验（nullptr / NonSerialized＝无约束）；容器 kind 与 builder kind
    //错配抛 BadValue
    static int32_t Materialize(VmExecutor& exec, const Value& v,
                               const TypeDesc* declaredType);
};

}  // namespace detail
}  // namespace nlang
