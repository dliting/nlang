#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/runtime/Module.h>
#include <nlang/runtime/Runtime.h>
#include <nlang/langservice/SymbolIndex.h>
#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "TestNatives.h"
#include "CrashReporter.h"
#ifdef _WIN32
#include <windows.h>  //SetErrorMode/ExitProcess (was transitive via CrashReporter.h)
#endif
#include <nlang_version.h>  // generated from the repo VERSION file
#include "ProjectFile.h"
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include "nlang/common/LibrarySearchPath.h"
#include "nlang/vm/CompiledModule.h"

namespace fs = std::filesystem;

using namespace nlang;

static void PrintUsage() {
    std::cerr << "Usage:\n"
              << "  ncc <source.n> [-o out.ncu] [-I <dir>...]  Compile and execute\n"
              << "  ncc build <source.n> [-o out.ncu] [-I <dir>...]  Compile only\n"
              << "  ncc -p <project.nproj> [-o out.npkg] [-I <dir>...]  Compile and execute a project\n"
              << "  ncc build -p <project.nproj> [-o out.npkg]  Compile a project\n"
              << "  ncc run <program.ncu|.npkg>  Execute only\n"
              << "  -I <dir>                    Add directory to import search path\n"
              << "  ncc --version               Print the compiler version\n";
}

//Assemble the unified, ordered library search path for a compile/build
//invocation: CLI -I > project <ImportPaths> > local source/project dir >
//NLANG_PATH > stdlib/exe/cwd. The same dirs drive .n source discovery and
//native DLL loading. Pure (the resolver does no existence checks).
static std::vector<std::string> ResolveCompileImportDirs(
    const std::vector<std::string>& cliDirs,
    const ProjectFile& project,
    const std::string& sourceFile,
    const std::string& stdlibDir,
    const fs::path& exePath) {
    SearchPathInput search;
    search.explicitDirs = cliDirs;
    if (!project.projectDir.empty()) {
        search.configuredDirs = project.importPaths;
        search.baseDirs.push_back(project.projectDir);
    } else {
        const fs::path parent = fs::path(sourceFile).parent_path();
        if (!parent.empty())
            search.baseDirs.push_back(parent.string());
    }
    if (const char* env = std::getenv("NLANG_PATH"))
        search.pathEnv = env;
    search.systemDirs = {
        stdlibDir, exePath.parent_path().string(), "."
    };
    return BuildLibrarySearchPath(search);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    //Version gate: reported before any runtime/compiler machinery starts,
    //so the Windows ExitProcess/static-destructor notes below don't apply
    //to this path (plain return flushes stdout normally).
    if (std::string(argv[1]) == "--version") {
        std::cout << "ncc (NLang) " << NLANG_VERSION << "\n";
        return 0;
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

    // ncc run <program.ncu|.npkg> [-I <dir>...]
    if (command == "run") {
        if (argc < 3) {
            std::cerr << "Error: 'run' requires a module file path.\n";
            return 1;
        }
        //Optional -I dirs (a native package may live off-module); any other
        //extra argument is a command-line mistake.
        std::vector<std::string> runDirs;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "-I") {
                if (i + 1 >= argc) {
                    std::cerr << "Error: option -I requires a value.\n";
                    return 1;
                }
                runDirs.push_back(argv[++i]);
            } else if (a.size() > 2 && a.compare(0, 2, "-I") == 0) {
                runDirs.push_back(a.substr(2));
            } else {
                std::cerr << "Error: unexpected extra argument '" << a
                          << "'.\n";
                return 1;
            }
        }
        CompiledModule mod;
        VmExecutor executor;
        //Phase 9f: host-provided natives (e2e test surface).
        RegisterTestNatives(executor);
        try {
            //Phase 6: run the artifact through the closure loader and the
            //load-time linker (same call sequence as nvm; see the note
            //there).
            fs::path modPath(argv[2]);
            SearchPathInput search;
            search.explicitDirs = runDirs;
            search.baseDirs.push_back(modPath.parent_path().string());
            if (const char* env = std::getenv("NLANG_PATH"))
                search.pathEnv = env;
            search.systemDirs = {
                NativeLibraryLoader::ExecutableDir(), "."
            };
            const std::vector<std::string> searchPath =
                BuildLibrarySearchPath(search);
            const std::string stdlibDir = langservice::FindStdLibDir(
                NativeLibraryLoader::ExecutableDir());
            std::vector<std::string> allDirs = searchPath;
            if (!stdlibDir.empty())
                allDirs.push_back(stdlibDir);
            for (const auto& d : allDirs)
                executor.AddNativeSearchDir(d);
            NcuLoader::Options loaderOpts;
            loaderOpts.searchDirs = std::move(allDirs);
            const NcuLoader::Result loaded =
                NcuLoader::LoadClosure(argv[2], loaderOpts);
            mod = NcuLinker::Link(loaded.units, loaded.entryKey);
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

    // ncc build <source.n> [-o out.ncu] [-I <dir>...]
    // ncc build -p <project.nproj> [-o out.npkg] [-I <dir>...]
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
            //(fs::path::extension(), not length arithmetic: the suffix
            //set has changed before and may change again.)
            if (fs::path(arg).extension() == ".nproj") {
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

    //Default output beside the sources: the .npkg in project mode, the
    //.ncu otherwise (project mode honors the .nproj's outputDir;
    //fs::path composition so an absolute outputDir replaces the project
    //dir instead of concatenating onto it).
    if (outputFile.empty()) {
        const char* defaultExt = projectFile.empty()
            ? NCU_EXTENSION : NPKG_EXTENSION;
        if (!projectFile.empty()) {
            fs::path dir(project.projectDir);
            if (!project.outputDir.empty())
                dir /= project.outputDir;
            outputFile = (dir / (moduleName + defaultExt)).string();
        } else {
            outputFile = moduleName + defaultExt;
        }
    }

    // Compile
    BuildParams params;
    if (!projectFile.empty()) {
        for (const auto& s : project.sources)
            params.m_SourceFiles.push_back(s);
        //Project mode: module paths are computed relative to the
        //.nproj directory (utils/helper.n -> "utils.helper"), and the
        //build also packs the .npkg distribution archive.
        params.m_sProjectDir = project.projectDir;
        params.m_bProjectMode = true;
    } else {
        params.m_SourceFiles.push_back(sourceFile);
    }
    params.m_sOutputModule = moduleName;

    //Locate the standard library declarations (stdlib/*.n) relative to this
    //executable; they are the authority for stdlib function signatures.
#ifdef _WIN32
    char exeBuf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
    fs::path exePath(exeBuf);
#else
    fs::path exePath = fs::read_symlink("/proc/self/exe");
#endif
    params.m_sStdLibDir =
        langservice::FindStdLibDir(exePath.parent_path().string());

    //Unified library search path (CLI -I > project <ImportPaths> > local dir
    //> NLANG_PATH > stdlib/exe/cwd); drives both .n discovery and DLL loading.
    const auto resolvedDirs = ResolveCompileImportDirs(importDirs, project,
        sourceFile, params.m_sStdLibDir, exePath);
    params.m_ImportDirs.assign(resolvedDirs.begin(), resolvedDirs.end());

    //Derive the save path parts from the whole outputFile via fs::path
    //(find_last_of/substr drops the separator of a root-only path like
    //"/out.ncu", silently redirecting the write to the CWD):
    //`ncc build src.n -o out.ncu` must write out.ncu, not <stem>.ncu
    //while the success message names out.ncu.
    fs::path outPath(outputFile);
    if (!outPath.parent_path().empty())
        params.m_sOutputDir = outPath.parent_path().string();
    std::string outName = outPath.filename().string();
    //Length-agnostic strip of either artifact extension: fs::path::
    //extension() compares the suffix itself, so renaming an artifact
    //cannot desynchronize a count. `-o app.npkg` in project mode must
    //not wrap into "app.npkg.npkg".
    if (outPath.extension() == NCU_EXTENSION
        || outPath.extension() == NPKG_EXTENSION)
        outName = outPath.stem().string();
    if (!outName.empty())
        params.m_sOutputModule = outName;

    //Recompute outputFile from the derived parts: the artifact writer
    //always writes <m_sOutputDir>/<module>.<ext>, so the success message
    //and the run-mode Load must target that composed path — not the raw
    //-o value (which may lack the extension or name only a directory).
    //The extension is the mode's artifact form: project mode packs the
    //.npkg archive; single-file mode writes the entry unit .ncu.
    const char* artifactExt = params.m_bProjectMode
        ? NPKG_EXTENSION : NCU_EXTENSION;
    fs::path saved = fs::path(params.m_sOutputModule + artifactExt);
    if (!params.m_sOutputDir.empty())
        saved = fs::path(params.m_sOutputDir) / saved;
    outputFile = saved.string();

    //Create the output directory up front (nested -o paths and multi-level
    //.nproj outputDir): the artifact writers only create a single level.
    //The non-throwing overload keeps a bad path (an existing file in the
    //chain, a read-only parent) a clean CLI error — this runs before the
    //exception boundary below.
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
        //Phase 6: per-unit codegen writes the unit images as the
        //artifacts (single-file .ncu / project .npkg); cross-unit
        //references stay as import slots resolved by the load-time
        //linker in every executor below.
        if (!builder.BuildArtifacts()) {
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
    //Native libraries (nlang_<ns>.dll) are searched in the import dirs in
    //addition to the executable directory, so a third-party package ships
    //its DLL beside its .n source. The just-built artifact executes
    //through the same closure loader + load-time linker as nvm (phase 6).
    try {
        //One ordered list feeds both consumers (native search dirs and
        //the closure loader — the shape nvm/ndb use). The ORDER is the
        //compile-time list (m_ImportDirs: CLI -I, project import paths,
        //source/project dir, NLANG_PATH, stdlib, exe, cwd) with the
        //artifact's own directory appended — run-time resolution here
        //reuses the compile-time order so a unit resolves where it
        //compiled from; nvm/ndb instead start from the module dir. The
        //trailing stdlib entry repeats the one inside m_ImportDirs — a
        //harmless re-probe that keeps the run path explicit about the
        //stdlib package even if the compile-time composition changes.
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs.assign(params.m_ImportDirs.begin(),
                                     params.m_ImportDirs.end());
        loaderOpts.searchDirs.push_back(
            fs::path(outputFile).parent_path().string());
        const std::string stdlibDir = langservice::FindStdLibDir(
            NativeLibraryLoader::ExecutableDir());
        if (!stdlibDir.empty())
            loaderOpts.searchDirs.push_back(stdlibDir);
        for (const auto& dir : loaderOpts.searchDirs)
            if (!dir.empty())
                executor.AddNativeSearchDir(dir);
        const NcuLoader::Result loaded =
            NcuLoader::LoadClosure(outputFile, loaderOpts);
        mod = NcuLinker::Link(loaded.units, loaded.entryKey);
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
