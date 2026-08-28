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
#include "ProjectFile.h"
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace nlang;

static void PrintUsage() {
    std::cerr << "Usage:\n"
              << "  ncc <source.n> [-o out.nmod] [-I <dir>...]  Compile and execute\n"
              << "  ncc build <source.n> [-o out.nmod] [-I <dir>...]  Compile only\n"
              << "  ncc -p <project.nproj> [-o out.nmod] [-I <dir>...]  Compile and execute a project\n"
              << "  ncc build -p <project.nproj> [-o out.nmod]  Compile a project\n"
              << "  ncc run <module.nmod>       Execute only\n"
              << "  -I <dir>                    Add directory to .nmod import search path\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    //Suppress error/crash popup dialogs so failures terminate
    //immediately instead of blocking automated testing.
    //Order matters: SetErrorMode first, then the crash reporter
    //(CrashReporter.h contract) so a crash during startup can't pop
    //a WER dialog before the reporter owns the failure path.
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    nlang::InstallCrashReporter("ncc");
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
    // ncc build -p <project.nproj> [-o out.nmod] [-I <dir>...]
    // ncc <source.n> [-I <dir>...] (compile + run)
    // ncc -p <project.nproj> [-I <dir>...] (compile + run)
    bool compileOnly = (command == "build");
    std::string sourceFile;
    std::string projectFile;
    std::string outputFile;
    std::vector<std::string> importDirs;

    //Unified parse: flags in any order after the (optional) subcommand;
    //the first positional is the source file. A trailing option with no
    //value fails loudly — letting it fall into the positional slot
    //surfaces as a bogus "invalid module name" two steps later. A second
    //positional or a repeated -o/-p is a command-line mistake, not
    //something to silently drop or override.
    int first = compileOnly ? 2 : 1;
    for (int i = first; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "-p" || arg == "-I") {
            if (i + 1 >= argc) {
                std::cerr << "Error: option " << arg
                          << " requires a value.\n";
                return 1;
            }
            const char* value = argv[++i];
            if (!*value) {
                std::cerr << "Error: option " << arg
                          << " requires a value.\n";
                return 1;
            }
            if (arg == "-o") {
                if (!outputFile.empty()) {
                    std::cerr << "Error: option -o given more than once.\n";
                    return 1;
                }
                outputFile = value;
            } else if (arg == "-p") {
                if (!projectFile.empty()) {
                    std::cerr << "Error: option -p given more than once.\n";
                    return 1;
                }
                projectFile = value;
            } else {
                importDirs.push_back(value);
            }
        } else if (arg.size() > 2 && arg.compare(0, 2, "-I") == 0)
            importDirs.push_back(arg.substr(2));
        else if (sourceFile.empty()) {
            //A .nproj fed positionally would reach the NLang parser and
            //die with a bare syntax error — point at -p instead.
            if (arg.size() > 6 && arg.substr(arg.size() - 6) == ".nproj") {
                std::cerr << "Error: '" << arg << "' looks like a project"
                          << " file; use -p " << arg << "\n";
                return 1;
            }
            sourceFile = arg;
        }
        else {
            std::cerr << "Error: unexpected extra argument '" << arg
                      << "'.\n";
            return 1;
        }
    }
    if (sourceFile.empty() && projectFile.empty()) {
        std::cerr << "Error: a source file or -p <project.nproj> is required.\n";
        PrintUsage();
        return 1;
    }
    if (!sourceFile.empty() && !projectFile.empty()) {
        std::cerr << "Error: -p <project.nproj> cannot be combined with a "
                     "source file argument.\n";
        return 1;
    }

    //Phase 10 Step 0: project mode — load .nproj, collect sources.
    ProjectFile project;
    std::string projectName;
    if (!projectFile.empty()) {
        std::string projErr;
        if (!ProjectFile::Load(projectFile, project, projErr)) {
            std::cerr << "Error: " << projErr << "\n";
            return 1;
        }
        projectName = project.name;
    }

    // Determine module name from source file (project mode: from .nproj)
    std::string moduleName;
    if (!projectName.empty()) {
        moduleName = projectName;
    } else {
        moduleName = sourceFile;
        auto dotPos = moduleName.rfind('.');
        if (dotPos != std::string::npos)
            moduleName = moduleName.substr(0, dotPos);
        auto slashPos = moduleName.find_last_of("/\\");
        if (slashPos != std::string::npos)
            moduleName = moduleName.substr(slashPos + 1);
    }

    //Default output: <name>.nmod beside the sources (project mode honors
    //the .nproj's outputDir; fs::path composition so an absolute outputDir
    //replaces the project dir instead of concatenating onto it).
    if (outputFile.empty()) {
        if (!projectFile.empty()) {
            fs::path dir(project.projectDir);
            if (!project.outputDir.empty())
                dir /= project.outputDir;
            outputFile = (dir / (moduleName + ".nmod")).string();
        } else {
            outputFile = moduleName + ".nmod";
        }
    }

    // Compile
    BuildParams params;
    if (!projectFile.empty()) {
        for (const auto& s : project.sources)
            params.m_SourceFiles.push_back(s);
        //Project mode: module paths are computed relative to the
        //.nproj directory (utils/helper.n -> "utils.helper").
        params.m_sProjectDir = project.projectDir;
    } else {
        params.m_SourceFiles.push_back(sourceFile);
    }
    params.m_sOutputModule = moduleName;
    //Phase 9c cross-module: import search path. "." is already in m_ImportDirs
    //(BuildParams default); append user -I dirs after.
    for (const auto& dir : importDirs)
        params.m_ImportDirs.push_back(dir);

    //Derive the save path parts from the whole outputFile via fs::path
    //(find_last_of/substr drops the separator of a root-only path like
    //"/out.nmod", silently redirecting the write to the CWD):
    //`ncc build src.n -o out.nmod` must write out.nmod, not <stem>.nmod
    //while the success message names out.nmod.
    fs::path outPath(outputFile);
    if (!outPath.parent_path().empty())
        params.m_sOutputDir = outPath.parent_path().string();
    std::string outName = outPath.filename().string();
    if (outName.size() > 5 && outName.substr(outName.size() - 5) == ".nmod")
        outName = outName.substr(0, outName.size() - 5);
    if (!outName.empty())
        params.m_sOutputModule = outName;

    //Recompute outputFile from the derived parts: ModuleSaver always writes
    //<m_sOutputDir>/<module>.nmod, so the success message and the run-mode
    //Load must target that composed path — not the raw -o value (which may
    //lack the extension or name only a directory).
    fs::path saved = fs::path(params.m_sOutputModule + ".nmod");
    if (!params.m_sOutputDir.empty())
        saved = fs::path(params.m_sOutputDir) / saved;
    outputFile = saved.string();

    //Create the output directory up front (nested -o paths and multi-level
    //.nproj outputDir): SaveModule only creates a single level. The
    //non-throwing overload keeps a bad path (an existing file in the chain,
    //a read-only parent) a clean CLI error — this runs before the exception
    //boundary below.
    fs::path outParent = fs::path(outputFile).parent_path();
    if (!outParent.empty()) {
        std::error_code dirErr;
        fs::create_directories(outParent, dirErr);
        if (dirErr) {
            std::cerr << "Error: cannot create output directory "
                      << outParent.string() << ": " << dirErr.message()
                      << "\n";
            return 1;
        }
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
