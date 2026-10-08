/*---
    HostFunctionTable.h — 扩展点⑤的适配层实现：宿主注册表＋声明驱动
    编组。HostFn 的 C++ 异常在此翻译为 NLang 异常再入 VM（禁止穿透
    VM 执行栈）；nlang::Exception 携 heapIdx 者按同实例重抛。
---*/
#pragma once
#include "IHostFunctions.h"
#include "nlang/embed/NLang.h"
#include <map>
#include <string>

namespace nlang {

class VmExecutor;   //内部类型不进公共头

class HostFunctionTable : public IHostFunctions {
public:
    //编组需要 executor（铸柄/拷串/抛 NLang 异常）；Impl 同时拥有
    //两者，构造时接线。
    void AttachExecutor(VmExecutor* executor) { m_pExecutor = executor; }
    void Register(const std::string& qualifiedKey, HostFn fn);
    bool Call(const CompiledFunction& callee, const uint8_t* argCells,
              uint32_t argc, uint8_t* resultCell) override;

private:
    VmExecutor* m_pExecutor = nullptr;
    std::map<std::string, HostFn> m_functions;
};

}  // namespace nlang
