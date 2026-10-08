/*---
    InterpreterImpl.cpp — 生命周期/装载/执行/调用解析。
---*/
#include "InterpreterImpl.h"
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
void Interpreter::setOutputHandler(WriteFn out, WriteFn err) {
    //Task 10 接线（IHostIo 转发器）；本任务不实现
}
Value Interpreter::newList() { return Value(); }   //Task 9 实现
Value Interpreter::newDict() { return Value(); }   //Task 9 实现

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
    running = true;
    int exitCode = 0;
    try {
        uint8_t resultCell[kFrameSlotBytes] = {};
        executor.CallFunctionByIdx(
            static_cast<uint16_t>(module.entryPoint), nullptr, 0,
            resultCell);
        std::memcpy(&exitCode, resultCell, sizeof(exitCode));
    } catch (const NLangThrow& t) {
        running = false;
        throw TranslateNLangThrow(executor, t);
    } catch (std::runtime_error& e) {
        running = false;
        if (IsEnvironmentFailure(e.what()))
            throw LoadError(e.what());
        throw;
    }
    running = false;
    ran = true;
    return exitCode;
}

//call() 最小实现（Task 6 完整化：重载 kind 消歧、out 拒绝、参考值）。
//本任务覆盖：守卫、按名查找（数量可吸收）、标量/字符串编组、
//标量/字符串/void 结果解码、NLangThrow/环境失败翻译。
Value Interpreter::Impl::Call(const char* funcName,
                              const std::vector<Value>& args) {
    if (!loaded)
        throw BadValue("call() before load()");
    if (running)
        throw BadValue("re-entering the interpreter is not supported");
    running = true;
    try {
        int target = -1;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const CompiledFunction& f = module.functions[i];
            if (f.name == funcName && args.size() <= f.paramCount) {
                target = static_cast<int>(i);
                break;
            }
        }
        if (target < 0)
            throw BadValue(std::string("no function ") + funcName);
        const CompiledFunction& f =
            module.functions[static_cast<size_t>(target)];

        std::vector<uint8_t> cells = EncodeArgs(f, args);
        uint8_t resultCell[kFrameSlotBytes] = {};
        executor.CallFunctionByIdx(static_cast<uint16_t>(target),
                                   cells.data(),
                                   static_cast<uint32_t>(args.size()),
                                   resultCell);
        Value result = DecodeResult(f, resultCell);
        running = false;
        return result;
    } catch (const NLangThrow& t) {
        running = false;
        throw TranslateNLangThrow(executor, t);
    } catch (std::runtime_error& e) {
        running = false;
        if (IsEnvironmentFailure(e.what()))
            throw LoadError(e.what());
        throw;
    }
}

//实参编组：String 铸运行时字符串柄（值语义，宿主侧字符串拷入），
//Null 即引用柄 0，其余按形参声明 kind 编码（无描述表时按 Int32）。
std::vector<uint8_t> Interpreter::Impl::EncodeArgs(
        const CompiledFunction& f, const std::vector<Value>& args) {
    std::vector<uint8_t> cells(args.size() * kFrameSlotBytes, 0);
    for (size_t i = 0; i < args.size(); ++i) {
        const Value& v = args[i];
        uint8_t* cell = cells.data() + i * kFrameSlotBytes;
        if (v.kind() == Value::Kind::String) {
            const int32_t handle = executor.MintHostString(v.asString());
            std::memcpy(cell, &handle, sizeof(handle));
        } else if (v.kind() == Value::Kind::Null) {
            //null reference = handle 0（cells 已零初始化）
        } else {
            embed::EncodeScalarCell(v, f.paramTypeDescs.size() > i
                    ? f.paramTypeDescs[i].type.kind
                    : static_cast<uint16_t>(RTK_Int32),
                cell);
        }
    }
    return cells;
}

//结果解码：void→Null、string→宿主拷贝（值语义）、其余按返回 kind。
Value Interpreter::Impl::DecodeResult(
        const CompiledFunction& f,
        const uint8_t resultCell[kFrameSlotBytes]) const {
    switch (f.returnTypeKind) {
    case RTK_Void:
        return Value();
    case RTK_String: {
        int32_t handle = 0;
        std::memcpy(&handle, resultCell, sizeof(handle));
        return Value(executor.StrValCopy(handle));
    }
    default:
        return embed::DecodeScalarCell(resultCell, f.returnTypeKind);
    }
}

}  // namespace nlang
