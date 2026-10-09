/*---
    HostFunctionTable.cpp — 注册表查名＋编组调用＋异常翻译链。
---*/
#include "HostFunctionTable.h"
#include "CallMarshalling.h"
#include "VmExecutor.h"

namespace nlang {

namespace {
//方法 callee 判别：方法帧 `this` 占槽 0——paramCount 计入它而
//paramTypeDescs/defaultValues 不计（CompiledModule.h 尺寸契约），故
//「描述符数 + 1 == 形参计数」是方法形态的可靠信号（零形参方法
//1==0+1 成立；自由函数 N 形参 N+1!=N、零形参 1!=0 均不命中）。
bool IsMethodCallee(const CompiledFunction& callee) {
    return callee.paramTypeDescs.size() + 1 == callee.paramCount;
}
}  // namespace

void HostFunctionTable::Register(const std::string& qualifiedKey, HostFn fn) {
    m_functions[qualifiedKey] = std::move(fn);
}

bool HostFunctionTable::Call(const CompiledFunction& callee,
                             const uint8_t* argCells, uint32_t argc,
                             uint8_t* resultCell) {
    const auto it = m_functions.find(callee.name);
    if (it == m_functions.end())
        return false;   //not ours: fall back to the native/DLL path
    //边界守卫（须在表命中之后：未命中须继续落 DLL native 路径，它
    //合法服务 native 方法）。宿主注册今天只覆盖自由函数——公共注册
    //键恒带分隔点而方法派发名是裸名，方法 callee 不进表只是两个相
    //距甚远事实的巧合交集；本守卫将其升级为显式边界契约，未来任何
    //能命中方法名的注册形态在此响亮失败，而非静默错位编组（ArgsFromCells
    //按声明 kind 配对每个 cell，方法帧的 this 槽会吃掉第一个描述符）。
    //真正的 this 感知编组属未设计的特性语义，留给未来功能设计。
    if (IsMethodCallee(callee))
        throw BadValue("host functions cannot dispatch native method '"
                       + callee.name + "': register a free function "
                         "(method callees carry an implicit this slot)");
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
