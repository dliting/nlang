/*---
    HostFunctionTable.cpp — 注册表查名＋编组调用＋异常翻译链。
---*/
#include "HostFunctionTable.h"
#include "CallMarshalling.h"
#include "VmExecutor.h"

namespace nlang {

void HostFunctionTable::Register(const std::string& qualifiedKey, HostFn fn) {
    m_functions[qualifiedKey] = std::move(fn);
}

bool HostFunctionTable::Call(const CompiledFunction& callee,
                             const uint8_t* argCells, uint32_t argc,
                             uint8_t* resultCell) {
    const auto it = m_functions.find(callee.name);
    if (it == m_functions.end())
        return false;   //not ours: fall back to the native/DLL path
    const std::vector<Value> args =
        embed::ArgsFromCells(*m_pExecutor, callee, argCells, argc);
    Value result;
    //翻译链（顺序即语义）——BadValue 是宿主用法错误（如 HostFn 内
    //再入 call() 的再入拒绝），必须原样穿透到宿主；携 heapIdx 的
    //nlang::Exception 是脚本异常的原样回流（同堆实例重抛）；宿主
    //手造 Exception 与其余 std::exception 一律铸成可捕获的脚本
    //Exception（⑤双向语义）。
    try {
        result = it->second(args);
    } catch (BadValue&) {
        throw;
    } catch (Exception& e) {
        if (e.m_heapIdx > 0)
            throw NLangThrow(e.m_heapIdx, e.message());
        m_pExecutor->RaiseHostException(e.message());
    } catch (std::exception& e) {
        m_pExecutor->RaiseHostException(e.what());
    }
    embed::ResultIntoCell(*m_pExecutor, callee, result, resultCell);
    return true;
}

}  // namespace nlang
