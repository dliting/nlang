/*---
    InterpreterImpl.cpp — 生命周期/装载/执行/调用解析。
---*/
#include "InterpreterImpl.h"
#include "CallMarshalling.h"
#include "Marshalling.h"
#include <nlang/runtime/Runtime.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nlang {

//--- 进程级 initialize/shutdown ------------------------------------------

static bool s_initialized = false;

void initialize() {
    if (s_initialized)
        return;
    Runtime::StaticInit();
    s_initialized = true;
    //spec §9: shutdown auto-registers via atexit — hosts need not call
    //it, but may. Registration happens once, with initialization.
    std::atexit(&shutdown);
}

void shutdown() {
    if (!s_initialized)
        return;
    Runtime::StaticFini();
    s_initialized = false;
}

//--- Interpreter 公共面 ---------------------------------------------------

Interpreter::Interpreter() : m_upImpl(std::make_unique<Impl>()) {
    initialize();
    //⑤双向接线：表用 executor 编组/抛异常，executor 的 CallNative
    //先问表（宿主注册优先于 DLL native）。
    m_upImpl->hostFunctions.AttachExecutor(&m_upImpl->executor);
    m_upImpl->executor.SetHostFunctions(&m_upImpl->hostFunctions);
}
Interpreter::~Interpreter() = default;

void Interpreter::load(const std::filesystem::path& artifact) {
    m_upImpl->Load(artifact);
}
void Interpreter::addImportDir(const std::filesystem::path& d) {
    if (m_upImpl->loaded)
        throw BadValue(
            "addImportDir must be called before load() (closures "
            "resolve at load time)");
    m_upImpl->importDirs.push_back(d.string());
}
int Interpreter::run() { return m_upImpl->Run(); }
Value Interpreter::call(const char* funcName,
                        const std::vector<Value>& args) {
    return m_upImpl->Call(funcName, args);
}
void Interpreter::registerHostFunction(const char* ns, const char* name,
                                       HostFn fn) {
    //任意时序：表在适配层，不随装载/初始化清空
    m_upImpl->hostFunctions.Register(std::string(ns) + "." + name,
                                      std::move(fn));
}
void Interpreter::setOutputHandler(WriteFn out, WriteFn err) {
    //Task 10 接线（IHostIo 转发器）；本任务不实现
}
//builder 语义（spec §6）：宿主侧暂存元素，每次 call 跨越物化为全新
//堆实例（无缓存）；builder 不入 GC 根表。
Value Interpreter::newList() {
    return detail::RefFactory::MakeBuilder(m_upImpl->executor,
                                           Value::Kind::List);
}
Value Interpreter::newDict() {
    return detail::RefFactory::MakeBuilder(m_upImpl->executor,
                                           Value::Kind::Dict);
}

//--- NLangThrow 翻译 ------------------------------------------------------

bool IsEnvironmentFailure(const std::string& m) {
    static const char* kPrefixes[] = {
        "cannot find native module '",
        "failed to load native module '",
        "native module '",
        "native function not registered: ",
    };
    for (const char* p : kPrefixes)
        if (m.rfind(p, 0) == 0)
            return true;
    return false;
}

//Exception family layout (Exception ctor intrinsic): slot[0]=class idx,
//field 0 (cell 1)=message handle. Reads go through the field bridge; a
//non-instance payload degrades to the carrier text instead of failing
//the translation (the host still learns an exception happened).
Exception Interpreter::Impl::TranslateNLangThrow(
        VmExecutor& executor, const struct NLangThrow& t) {
    std::string message = t.what();
    std::string exceptionClass = "Exception";
    if (t.heapIdx > 0) {
        try {
            exceptionClass = executor.HostClassName(t.heapIdx);
            uint8_t msgCell[8] = {};
            executor.HostFieldCellGet(t.heapIdx, 0, msgCell);
            int32_t msgHandle = 0;
            std::memcpy(&msgHandle, msgCell, sizeof(msgHandle));
            if (msgHandle != 0)
                message = executor.StrValCopy(msgHandle);
        } catch (const std::runtime_error&) {
            //degrade: keep the carrier text
        }
    }
    Exception ex(std::move(message), executor.Backtrace(),
                 std::move(exceptionClass));
    ex.m_heapIdx = t.heapIdx;   //friend: rethrowable identity
    return ex;
}

//--- Impl ------------------------------------------------------------------

//Re-entry guard as RAII: every exit path from run()/call() — including
//the BadValue throws from the resolver/marshaller, which inherit
//logic_error and therefore bypass the runtime_error/NLangThrow catch
//chains — must reset `running`. The pre-RAII version left the flag set
//after a failed call() and every later call on the same Interpreter
//failed with a bogus "re-entering" BadValue.
namespace {
class RunningGuard {
public:
    explicit RunningGuard(bool& flag) : m_flag(flag) { m_flag = true; }
    ~RunningGuard() { m_flag = false; }
private:
    bool& m_flag;
};
}   // namespace

void Interpreter::Impl::Load(const std::filesystem::path& artifact) {
    if (loaded)
        throw BadValue("load() may be called once per Interpreter");
    NcuLoader::Options opts;
    opts.searchDirs = importDirs;
    opts.searchDirs.push_back(artifact.parent_path().string());
    try {
        const NcuLoader::Result loaded =
            NcuLoader::LoadClosure(artifact.string(), opts);
        module = NcuLinker::Link(loaded.units, loaded.entryKey);
    } catch (const std::runtime_error& e) {
        //装载器契约：全部失败抛 runtime_error（NcuLoader.h:37-39）
        throw LoadError(e.what());
    }
    if (module.entryPoint < 0)
        throw LoadError("module has no entry point (no main)");
    executor.InitializeForRun(module);
    loaded = true;
}

int Interpreter::Impl::Run() {
    if (!loaded)
        throw BadValue("run() before load()");
    if (ran)
        throw BadValue("run() may be called once per Interpreter");
    if (running)
        throw BadValue("re-entering the interpreter is not supported");
    RunningGuard guard(running);
    int exitCode = 0;
    try {
        uint8_t resultCell[kFrameSlotBytes] = {};
        executor.CallFunctionByIdx(
            static_cast<uint16_t>(module.entryPoint), nullptr, 0,
            resultCell);
        std::memcpy(&exitCode, resultCell, sizeof(exitCode));
    } catch (const NLangThrow& t) {
        throw TranslateNLangThrow(executor, t);
    } catch (std::runtime_error& e) {
        if (IsEnvironmentFailure(e.what()))
            throw LoadError(e.what());
        throw;
    }
    ran = true;
    return exitCode;
}

Value Interpreter::Impl::Call(const char* funcName,
                              const std::vector<Value>& args) {
    if (!loaded)
        throw BadValue("call() before load()");
    if (running)
        throw BadValue("re-entering the interpreter is not supported");
    RunningGuard guard(running);
    try {
        const int target = ResolveCallTarget(funcName, args);
        const CompiledFunction& f =
            module.functions[static_cast<size_t>(target)];
        std::vector<uint8_t> cells = EncodeArgs(f, args);
        uint8_t resultCell[kFrameSlotBytes] = {};
        executor.CallFunctionByIdx(static_cast<uint16_t>(target),
                                   cells.data(),
                                   static_cast<uint32_t>(args.size()),
                                   resultCell);
        return DecodeResult(f, resultCell);
    } catch (const NLangThrow& t) {
        throw TranslateNLangThrow(executor, t);
    } catch (std::runtime_error& e) {
        if (IsEnvironmentFailure(e.what()))
            throw LoadError(e.what());
        throw;
    }
}

//缺席形参（超出实参个数的形参）须全部有可常量重建的默认值。
//RTK_Void＝无默认；RTK_Unfoldable＝有默认表达式但不可折叠重建
//（生产方 VmBackend.cpp:143/:184 盖章）——嵌入入口无法重建，
//spec §13 定为 BadValue，在此前置拒绝而非放行到防御性 runtime_error。
namespace {
bool MissingParamsHaveUsableDefaults(const CompiledFunction& f,
                                     size_t argc) {
    for (uint32_t i = static_cast<uint32_t>(argc); i < f.paramCount; ++i) {
        if (i >= f.defaultValues.size())
            return false;
        const uint16_t tag = f.defaultValues[i].tag;
        if (tag == RTK_Void || tag == RTK_Unfoldable)
            return false;
    }
    return true;
}
}   // namespace

//键规则即防线：模块链接后的自由函数键是「TU 词干.名」，类方法不走
//本入口（宿主方法调用属未来增量），故不另查 classes[].methodIndices。
//intrinsics 无独立可调体（CallFunctionByIdx 亦拒之），扫描时跳过。
int Interpreter::Impl::ResolveCallTarget(const char* funcName,
                                         const std::vector<Value>& args) {
    if (!std::strchr(funcName, '.'))
        throw BadValue(std::string(funcName) +
            ": call() keys are package-qualified (unit-stem.name)");
    std::vector<int> candidates;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const CompiledFunction& f = module.functions[i];
        if (f.name != funcName || f.intrinsicId != INTR_None)
            continue;
        if (args.size() <= f.paramCount
                && MissingParamsHaveUsableDefaults(f, args.size()))
            candidates.push_back(static_cast<int>(i));
    }
    if (candidates.empty())
        throw BadValue(std::string("no overload of ") + funcName
            + " accepts " + std::to_string(args.size()) + " argument(s)");
    if (candidates.size() > 1) {
        //多候选淘汰：试探编组（EncodeScalarCell 的 kind/值域检查抛
        //BadValue 者出局）；淘汰后仍多则歧义。
        std::vector<int> survivors;
        for (int idx : candidates) {
            try {
                EncodeArgs(module.functions[static_cast<size_t>(idx)],
                           args);
                survivors.push_back(idx);
            } catch (const BadValue&) {
            }
        }
        candidates = std::move(survivors);
        if (candidates.empty())
            throw BadValue(std::string("no overload of ") + funcName
                + " accepts these argument kinds");
        if (candidates.size() > 1)
            throw BadValue(std::string("ambiguous call to ") + funcName);
    }
    const CompiledFunction& f =
        module.functions[static_cast<size_t>(candidates.front())];
    for (const ParamTypeDesc& p : f.paramTypeDescs)
        if (p.flags & PTDF_Out)
            throw BadValue(std::string(funcName)
                + " has out-parameters; the Value model cannot carry them");
    return candidates.front();
}

//实参编组：委托共享单元编组（与宿主函数直派同一声明驱动规则；
//完整 TypeDesc 供 builder 物化的元素校验与容器错配检查）。
std::vector<uint8_t> Interpreter::Impl::EncodeArgs(
        const CompiledFunction& f, const std::vector<Value>& args) {
    std::vector<uint8_t> cells(args.size() * kFrameSlotBytes, 0);
    for (size_t i = 0; i < args.size(); ++i)
        embed::ValueIntoCell(executor, args[i], embed::DeclaredParamKind(f, i),
                             cells.data() + i * kFrameSlotBytes,
                             embed::DeclaredParamType(f, i));
    return cells;
}

//结果解码：void→Null、string→宿主拷贝（值语义）、参考→有根代理。
Value Interpreter::Impl::DecodeResult(
        const CompiledFunction& f,
        const uint8_t resultCell[kFrameSlotBytes]) {
    if (f.returnTypeKind == RTK_Void)
        return Value();
    return embed::ValueFromCell(executor, f.returnTypeKind, resultCell);
}

}  // namespace nlang
