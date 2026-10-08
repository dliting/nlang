/*---
    InterpreterImpl.h — Interpreter pimpl：生命周期/装载/执行/调用
    解析与宿主函数表、I/O 转发器的持有点。
---*/
#pragma once
#include "nlang/embed/NLang.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "VmExecutor.h"
#include <memory>

namespace nlang {

struct Interpreter::Impl {
    VmExecutor executor;
    CompiledModule module;
    bool loaded = false;
    bool ran = false;
    bool running = false;      //再入守卫（HostFn 内再入 run/call → BadValue）
    std::vector<std::string> importDirs;

    void Load(const std::filesystem::path& artifact);
    int Run();
    Value Call(const char* funcName, const std::vector<Value>& args);

    //call() 的编组两翼：实参 → 帧单元格（字符串铸柄/null=柄 0/标量按
    //形参 kind）；返回单元格 → Value（void→Null/string→拷出/其余标量）。
    std::vector<uint8_t> EncodeArgs(const CompiledFunction& f,
                                    const std::vector<Value>& args);
    Value DecodeResult(const CompiledFunction& f,
                       const uint8_t resultCell[kFrameSlotBytes]) const;

    //call/run 共享：NLangThrow → nlang::Exception（读实例的 message
    //字段与类名；backtrace 取 executor.Backtrace()）。Impl 的成员——
    //Exception 的友元是本类型，成员函数才能写 m_heapIdx。
    static Exception TranslateNLangThrow(VmExecutor& executor,
                                         const struct NLangThrow& t);
};

//环境类失败的判别（spec §5：原生绑定族 → LoadError；集中一处维护）
bool IsEnvironmentFailure(const std::string& message);

}  // namespace nlang
