#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/runtime/Module.h>
#include <nlang/runtime/Runtime.h>
#include "VmBackend.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include "TestNatives.h"
#include "CrashReporter.h"
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace nlang;

static void PrintUsage() {
    std::cerr << "Usage:\n"
              << "  ncc <source.n> [-I <dir>...]  Compile and execute\n"
              << "  ncc build <source.n> [-o out.nmod] [-I <dir>...]  Compile only\n"
              << "  ncc run <module.nmod>       Execute only\n"
              << "  -I <dir>                    Add directory to .nmod import search path\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

#ifdef _WIN32
    nlang::InstallCrashReporter("ncc");
#endif
    //Suppress error/crash popup dialogs so failures terminate
    //immediately instead of blocking automated testing.
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    Runtime::StaticInit();

    std::string command = argv[1];

    // ncc run <module.nmod>
    if (command == "run") {
        if (argc < 3) {
            std::cerr << "Error: 'run' requires a module file path.\n";
            return 1;
        }
        CompiledModule mod;
        VmExecutor executor;
        //Phase 9f: host-provided natives (e2e test surface).
        RegisterTestNatives(executor);
        try {
            mod = ModuleLoader::Load(argv[2]);
            int result = executor.Execute(mod);
#ifdef _WIN32
            ExitProcess(static_cast<UINT>(result));
#else
            return result;
#endif
        } catch (const std::exception& e) {
            std::cerr << "Runtime error: " << e.what() << "\n";
            const auto& bt = executor.Backtrace();
            if (!bt.empty())
                std::cerr << "Backtrace:\n" << bt;
            return 1;
        }
    }

    // ncc build <source.n> [-o out.nmod] [-I <dir>...]
    // ncc <source.n> [-I <dir>...] (compile + run)
    bool compileOnly = (command == "build");
    std::string sourceFile;
    std::string outputFile;
    std::vector<std::string> importDirs;

    if (compileOnly) {
        if (argc < 3) {
            std::cerr << "Error: 'build' requires a source file path.\n";
            return 1;
        }
        sourceFile = argv[2];
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-o" && i + 1 < argc)
                outputFile = argv[++i];
            else if (arg == "-I" && i + 1 < argc)
                importDirs.push_back(argv[++i]);
            else if (arg.size() > 2 && arg.compare(0, 2, "-I") == 0)
                importDirs.push_back(arg.substr(2));
        }
    } else {
        sourceFile = argv[1];
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-I" && i + 1 < argc)
                importDirs.push_back(argv[++i]);
            else if (arg.size() > 2 && arg.compare(0, 2, "-I") == 0)
                importDirs.push_back(arg.substr(2));
        }
    }

    // Determine module name from source file
    std::string moduleName = sourceFile;
    auto dotPos = moduleName.rfind('.');
    if (dotPos != std::string::npos)
        moduleName = moduleName.substr(0, dotPos);
    auto slashPos = moduleName.find_last_of("/\\");
    if (slashPos != std::string::npos)
        moduleName = moduleName.substr(slashPos + 1);

    if (outputFile.empty())
        outputFile = moduleName + ".nmod";

    // Compile
    BuildParams params;
    params.m_SourceFiles.push_back(sourceFile);
    params.m_sOutputModule = moduleName;
    //Phase 9c cross-module: import search path. "." is already in m_ImportDirs
    //(BuildParams default); append user -I dirs after.
    for (const auto& dir : importDirs)
        params.m_ImportDirs.push_back(dir);

    // If -o specifies a path with directory, set m_sOutputDir
    auto lastSep = outputFile.find_last_of("/\\");
    if (lastSep != std::string::npos) {
        params.m_sOutputDir = outputFile.substr(0, lastSep);
        std::string outName = outputFile.substr(lastSep + 1);
        if (outName.size() > 5 && outName.substr(outName.size() - 5) == ".nmod")
            outName = outName.substr(0, outName.size() - 5);
        if (!outName.empty())
            params.m_sOutputModule = outName;
    }

    StreamCompileLogger logger(std::cerr);
    ModuleBuilder builder(params, logger);

    //Exception boundary: codegen internal errors (e.g. resolver/codegen
    //binding divergence) throw std::runtime_error. Without this catch the
    //exception escapes main as an unhandled MSVC C++ exception — the process
    //aborts with exit code 3 and buffered diagnostics are lost silently.
    try {
        if (!builder.Build()) {
            std::cerr << "Compilation failed.\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Compiler internal error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Compiled successfully: " << outputFile << "\n";

    if (compileOnly) {
        //On Windows, static destructors from Runtime::StaticInit() can
        //corrupt the process exit code. ExitProcess() bypasses this.
#ifdef _WIN32
        ExitProcess(0);
#else
        return 0;
#endif
    }

    // Execute
    CompiledModule mod;
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    try {
        mod = ModuleLoader::Load(outputFile);
        int result = executor.Execute(mod);
        //On Windows, static destructors from Runtime::StaticInit() can
        //corrupt the process exit code. ExitProcess() bypasses this.
#ifdef _WIN32
        ExitProcess(static_cast<UINT>(result));
#else
        return result;
#endif
    } catch (const std::exception& e) {
        std::cerr << "Runtime error: " << e.what() << "\n";
        const auto& bt = executor.Backtrace();
        if (!bt.empty())
            std::cerr << "Backtrace:\n" << bt;
        return 1;
    }
}
