/*---
    SourceCompiler.cpp — 临时目录 RAII＋编译管线（工具 ScriptRunner
    同式，但错误经 LoadError 异常而非 stderr；不合并，D1）。
---*/
#include "SourceCompiler.h"
#include <nlang/embed/NLang.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/runtime/Module.h>
#include <atomic>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>   //GetCurrentProcessId
#else
#include <unistd.h>    //getpid
#endif

namespace nlang {

namespace {

unsigned ProcessId() {
#ifdef _WIN32
    return static_cast<unsigned>(GetCurrentProcessId());
#else
    return static_cast<unsigned>(::getpid());
#endif
}

//逐条诊断拼成多行文本（ListCompileLogger 条目的 FullMessage 自带
//"file:line:col: level: message" 头）。CLL_Info 的进度行不会进
//ListCompileLogger 的 Error/Fatal 面，但 Info 条目也在列表里——
//ad-hoc 场景无进度行价值，跳过。
std::string JoinDiagnostics(const ListCompileLogger& logger) {
    std::string text;
    for (auto it = logger.cbegin(); it != logger.cend(); ++it) {
        if ((*it)->Level() == CLL_Info)
            continue;
        text += (*it)->FullMessage();
        text += '\n';
    }
    return text;
}

}   // namespace

SourceCompiler::SourceCompiler() {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec);
    if (ec)
        throw LoadError("cannot locate temp directory: " + ec.message());
    static std::atomic<unsigned> counter{0};
    m_runDir = root / ("nlang_e_" + std::to_string(ProcessId()) + "_"
        + std::to_string(counter++));
    std::filesystem::create_directories(m_runDir, ec);
    if (ec)
        throw LoadError("cannot create temp run directory: "
            + m_runDir.string() + ": " + ec.message());
}

SourceCompiler::~SourceCompiler() {
    std::error_code ignored;
    std::filesystem::remove_all(m_runDir, ignored);
}

SourceCompiler::Output SourceCompiler::Compile(
        const Input& in, const std::vector<std::string>& searchDirs) {
    //stem 预校验（spec §3 步骤 1）：ModuleManager::Create 的同名
    //LogError 是裸 stderr、不进诊断收集，靠它则 LoadError 无理由
    //文本——入口用公开静态校验抛带明确文案的错误。
    if (!Module::IsValidName(in.moduleName))
        throw LoadError("invalid module name \"" + in.moduleName
            + "\": the module name must be a valid identifier");
    //落源：文件形态直用真实路径；字符串形态写 <runDir>/<moduleName>.n
    //（行号进诊断，D3：诊断读 script.n:1:9）。
    std::filesystem::path sourcePath = in.scriptFile;
    if (sourcePath.empty()) {
        sourcePath = m_runDir / (in.moduleName + ".n");
        std::ofstream out(sourcePath, std::ios::binary);
        out << in.utf8Source;
        if (!out)
            throw LoadError("cannot create temp script file: "
                + sourcePath.string());
    }
    sourcePath = std::filesystem::absolute(sourcePath).lexically_normal();

    //静态表：Runtime::StaticInit 已由 Interpreter 构造完成；cast 表
    //已从 Runtime 拆出且无惰性守卫，漏调＝调试断言/发布静默错编
    //（工具同式前置，ScriptRunner.h:295-296）。
    TypeCastInfo::StaticInit();

    BuildParams params;
    params.m_SourceFiles.push_back(sourcePath.string());
    params.m_sProjectDir = sourcePath.parent_path().string();
    params.m_sOutputModule = in.moduleName;
    params.m_sOutputDir = m_runDir.string();
    params.m_sTempDir = m_runDir.string();
    //D7：留空＝库索引按 import 惰性加载（stdlib 声明经 searchDirs
    //解析）；等价性由 stdlib 集成测试钉死（规格 §6.9），不可接受则
    //回退显式传 stdlib 目录。
    params.m_sStdLibDir.clear();
    //整表替换：构造器种子 "." 会把 cwd 悄悄变成导入目录（工具踩过
    //的坑）；embed 契约＝搜索表只有宿主指的目录＋（文件形态）脚本
    //所在目录，无 cwd 默认（规格 §4）。
    params.m_ImportDirs.assign(searchDirs.begin(), searchDirs.end());

    //D8：CreateModule 把产物模块按名注册进进程级 ModuleManager
    //（注册是 PrepareUnits 的第一步，先于解析——几乎所有编译失败
    //都发生在注册之后、恰需本 guard；唯一未注册窗口＝guard 构造
    //到 builder 进入之间的落源失败，guard 空转）。该注册无任何
    //消费方（Instance() 在 src/ 的唯一调用点就是 CreateModule），
    //但残留条目让后续同名编译 EEXIST——破坏 runSource 重试与同
    //进程多次 ad-hoc 执行。快照式注销：只删本次编译创建的条目；
    //EEXIST 时同名模块是外来注册（宿主直连 nlang_compiler 编过
    //同名模块），不动别人的。
    struct DeregisterOutputModule {
        ModuleManager& manager;
        const IdString name;
        Module* const preexisting;
        DeregisterOutputModule(ModuleManager& mgr,
                               const std::string& moduleName)
            : manager(mgr), name(moduleName),
              preexisting(mgr.Find(name)) {}
        ~DeregisterOutputModule() {
            Module* pCurrent = manager.Find(name);
            if (pCurrent && pCurrent != preexisting)
                manager.Unload(pCurrent);
        }
    } deregisterOutput{ModuleManager::Instance(), in.moduleName};

    ListCompileLogger logger;
    bool built = false;
    try {
        ModuleBuilder builder(params, logger);
        built = builder.BuildArtifacts();
    } catch (const std::exception& e) {
        throw LoadError(std::string("compiler internal error: ")
            + e.what());
    }
    //D9: cloned log items own a Clone-time file-name snapshot, so
    //joining diagnostics after the builder died is safe.
    if (!built || logger.Errors() > 0 || logger.Fatals() > 0)
        throw LoadError(JoinDiagnostics(logger) + "Compilation failed.");
    return {m_runDir / (in.moduleName + ".ncu")};
}

}  // namespace nlang
