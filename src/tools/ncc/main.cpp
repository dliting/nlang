#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/runtime/Module.h>
#include <nlang/runtime/Runtime.h>
#include "VmBackend.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif
#include <iostream>
#include <string>
#include <vector>

using namespace nlang;

static void PrintUsage() {
    std::cerr << "Usage:\n"
              << "  ncc <source.n>              Compile and execute\n"
              << "  ncc build <source.n> [-o out.nmod]  Compile only\n"
              << "  ncc run <module.nmod>       Execute only\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

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
        try {
            CompiledModule mod = ModuleLoader::Load(argv[2]);
            VmExecutor executor;
            int result = executor.Execute(mod);
#ifdef _WIN32
            ExitProcess(static_cast<UINT>(result));
#else
            return result;
#endif
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    }

    // ncc build <source.n> [-o out.nmod]
    // ncc <source.n> (compile + run)
    bool compileOnly = (command == "build");
    std::string sourceFile;
    std::string outputFile;

    if (compileOnly) {
        if (argc < 3) {
            std::cerr << "Error: 'build' requires a source file path.\n";
            return 1;
        }
        sourceFile = argv[2];
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "-o" && i + 1 < argc)
                outputFile = argv[++i];
        }
    } else {
        sourceFile = argv[1];
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

    if (!builder.Build()) {
        std::cerr << "Compilation failed.\n";
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
    try {
        CompiledModule mod = ModuleLoader::Load(outputFile);
        VmExecutor executor;
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
        return 1;
    }
}
