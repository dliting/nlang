// --- ndb debugger unit tests: shared harness implementation ---
#include "test_debugger_common.h"

using namespace nlang;

int g_pass = 0, g_fail = 0;

//Scratch directory for temp sources and modules.
std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_debugger";
    std::filesystem::create_directories(dir);
    return dir;
}

//Write <tag>.n with the raw source (no stdlib imports prepended —
//debugger tests target plain programs), build <tag>.ncu.
BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << source;
    }

    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    //Signatures resolve from stdlib/*.n (STDLIB_DIR is a compile def).
    params.m_sStdLibDir = STDLIB_DIR;

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);

    BuildOutcome outcome;
    try {
        //Plain import-free programs produce a complete entry-unit .ncu
        //(BuildArtifacts; the merged Build path is gone), so the direct
        //Load below stays valid.
        outcome.ok = builder.BuildArtifacts()
            && std::filesystem::exists(modPath);
    } catch (const std::exception& e) {
        outcome.diagnostics += std::string("internal error: ") + e.what();
        return outcome;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

//Build a consumer TU from raw source with the scratch dir on the
//import path (shared by the import-shaped tests; the imported lib must
//already exist as a compiled .ncu — see the FindModuleFile note in
//test_v19_import_roundtrip).
BuildOutcome buildConsumer(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    {
        std::ofstream out(dir / (tag + ".n"), std::ios::binary);
        out << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / (tag + ".n")).string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs.push_back(dir.string());
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    BuildOutcome outcome;
    outcome.ok = builder.BuildArtifacts();
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

//加载刚构建的 <tag>.ncu。
CompiledModule loadBuilt(const std::string& tag)
{
    return ModuleLoader::Load(
        (scratchDir() / (tag + ".ncu")).string());
}

//加载并链接一个带导入的刚构建产物：闭包从 scratch 目录与 stdlib 包
//目录解析（外部 .ncu 与产物同目录；io/math/fs 单元在 stdlib.npkg 里），
//nlink 合并为唯一运行期模块。加载期链接是导入形态的唯一执行路径
//（产物自身不含库代码）。
CompiledModule loadLinked(const std::string& tag)
{
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {scratchDir().string(), STDLIB_DIR};
    NcuLoader::Result loaded = NcuLoader::LoadClosure(
        (scratchDir() / (tag + ".ncu")).string(), loaderOpts);
    return NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
}
