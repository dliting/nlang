/*---
    SourceCompiler.h — ad-hoc 源码入口的编译步骤（spec 2026-10-15 §3
    步骤 1-2）：落源＋唯一临时目录＋BuildParams 组装＋诊断收集。
    临时目录 RAII：构造建、析构删（含异常路径）；Impl::Load 完成后
    目录即不再被引用。不触碰 Interpreter 状态（失败可重试）。
---*/
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace nlang {

class SourceCompiler {
public:
    //两入口归一：utf8Source 非空＝字符串形态（合成 <moduleName>.n
    //写入临时目录）；scriptFile 非空＝文件形态（真实路径直用，不向
    //脚本所在目录写任何产物）。moduleName：字符串形态固定
    //"script"（D3），文件形态＝文件 stem（须为合法标识符，否则编译
    //器 invalid module name 诊断 → LoadError）。
    struct Input {
        std::string utf8Source;
        std::filesystem::path scriptFile;
        std::string moduleName;
    };
    struct Output {
        std::filesystem::path artifact;   // <临时目录>/<moduleName>.ncu
    };

    SourceCompiler();   // 建唯一临时目录；不可用抛 LoadError
    ~SourceCompiler();  // 删临时目录（含异常路径）
    SourceCompiler(const SourceCompiler&) = delete;
    SourceCompiler& operator=(const SourceCompiler&) = delete;

    //编译进临时目录。失败（诊断含 Error/Fatal，或 BuildArtifacts
    //返回 false，或编译器内部异常）抛 LoadError：文本＝逐条诊断
    //（每条一行，file:line:col 头由 logger 条目自带）＋末行
    //"Compilation failed."。
    Output Compile(const Input& in,
                   const std::vector<std::string>& searchDirs);

private:
    std::filesystem::path m_runDir;
};

}  // namespace nlang
