/*---
    nlang -- run NLang scripts ad hoc, python-style (v0.9.1): top-level
    statements execute without an explicit main() (the compiler wraps
    them in a synthesized entry). Compiles and executes in process; the
    entry unit rides a temp .ncu through the same loader/linker seam
    "ncc <source.n>" uses, removed on exit (no artifact is left behind).
---*/
#include "ScriptRunner.h"
#include "CrashReporter.h"
#include <nlang_version.h>  // generated from the repo VERSION file
#ifdef _WIN32
#include <windows.h>  //SetErrorMode/ExitProcess/CommandLineToArgvW
#include <crtdbg.h>
#include <shellapi.h>  //CommandLineToArgvW
#endif
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace nlangtool;

namespace {

const char* const kUsage =
    "usage: nlang <script.n> [-I <dir>]...\n"
    "       nlang -c \"<statements>\" [-I <dir>]...\n"
    "       nlang --version\n";

#ifdef _WIN32
//Per-stream console UTF-8 setup. GetConsoleMode is the is-a-console
//test, not GetFileType: GetFileType misclassifies the NUL device as a
//character device, so `nlang x > NUL` would otherwise rewrite the code
//page of the user's interactive console from a redirected run. Each
//half is judged independently, so a half-redirected invocation only
//switches the half that is a real console. Originals are restored at
//exit (std::atexit on plain returns, and explicitly before
//ExitProcess -- see main; a crash skips both, which is acceptable).
UINT g_originalInputCp = 0;
UINT g_originalOutputCp = 0;

void RestoreConsoleCodePages() {
    if (g_originalInputCp != 0) SetConsoleCP(g_originalInputCp);
    if (g_originalOutputCp != 0) SetConsoleOutputCP(g_originalOutputCp);
}

void SetupConsoleUtf8() {
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
        g_originalInputCp = GetConsoleCP();
        SetConsoleCP(CP_UTF8);
    }
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
        g_originalOutputCp = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
    }
    std::atexit(RestoreConsoleCodePages);
}

//UTF-8 argv. The C runtime's char* argv went through the ANSI code page,
//so non-ASCII script statements (`nlang -c "string s = \"你好\";"`) would
//arrive mojibake before the compiler ever sees them. Decode the wide
//command line instead and convert each argument to UTF-8 here.
std::vector<std::string> Utf8Argv() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide)
        return out;
    out.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1,
            nullptr, 0, nullptr, nullptr);
        std::string arg;
        if (bytes > 1) {
            arg.resize(bytes - 1);   //drop the trailing NUL
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(),
                bytes, nullptr, nullptr);
        }
        out.push_back(std::move(arg));
    }
    LocalFree(wide);
    return out;
}

//Crash diagnostics + console UTF-8 for the run ahead (mirrors the nvm
//startup sequence). SetErrorMode first, then the crash reporter
//(CrashReporter.h contract) so a crash during startup can't pop a WER
//dialog first.
void InstallStartupGuards() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    nlang::InstallCrashReporter("nlang");
    SetupConsoleUtf8();
}
#endif

//Parsed command line: exactly one run target (a positional script path
//or -c statements; an empty -c value is a valid empty script), plus
//import dirs in both -I spellings (mirroring ncc/nvm).
struct CliOptions {
    bool hasInline = false;
    std::string inlineStmts;
    std::string scriptPath;
    std::vector<std::filesystem::path> importDirs;
};

//Scan args (program name at [0]). Rejects unknown flags, -c/-I without
//a value, two run targets, or none; prints the diagnostic + usage and
//returns false on rejection.
bool ParseArgs(const std::vector<std::string>& args, CliOptions& out) {
    bool hasScript = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-c") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "nlang: option -c requires a value\n");
                return false;
            }
            out.inlineStmts = args[++i];
            out.hasInline = true;
        } else if (a == "-I") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "nlang: option -I requires a value\n");
                return false;
            }
            out.importDirs.emplace_back(args[++i]);
        } else if (a.size() > 2 && a.compare(0, 2, "-I") == 0) {
            out.importDirs.emplace_back(a.substr(2));
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "nlang: unknown option '%s'\n%s",
                a.c_str(), kUsage);
            return false;
        } else if (!hasScript && !out.hasInline) {
            out.scriptPath = a;
            hasScript = true;
        } else {
            std::fprintf(stderr,
                "nlang: exactly one script (path or -c) may be given\n%s",
                kUsage);
            return false;
        }
    }
    if (hasScript == out.hasInline) {   //both present, or neither
        std::fprintf(stderr,
            "nlang: exactly one script (path or -c) must be given\n%s",
            kUsage);
        return false;
    }
    return true;
}

//Resolve the run target and execute. Inline statements ride a temp .n
//file (line numbers survive diagnostics); a script path runs as given.
//Diagnostics stream to stderr from inside RunScript.
RunOutcome RunTarget(const CliOptions& opts) {
    if (opts.hasInline) {
        TempScriptFile temp(opts.inlineStmts);
        if (temp.Path().empty())
            return {};   //diagnostic already printed (no usable temp root)
        return RunScript(temp.Path(), opts.importDirs);
    }
    if (!std::filesystem::exists(opts.scriptPath)) {
        std::fprintf(stderr, "nlang: script not found: %s\n",
            opts.scriptPath.c_str());
        return {};
    }
    return RunScript(opts.scriptPath, opts.importDirs);
}

} // namespace

#ifdef _WIN32
int main(int, char*[]) {
    const std::vector<std::string> args = Utf8Argv();
#else
int main(int argc, char* argv[]) {
    //POSIX argv is already byte-transparent UTF-8. argv[0] (program
    //name) stays in so ParseArgs sees the same [0]=program-name
    //layout as the Win32 branch.
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(argv[i]);
#endif
    if (args.size() < 2) {
        std::cerr << kUsage;
        return 1;
    }
    //Version gate first: no runtime is initialized yet, so the plain
    //return is safe (the ExitProcess notes below only apply after
    //Runtime::StaticInit registers static destructors).
    if (args[1] == "--version") {
        std::cout << "nlang (NLang) " << NLANG_VERSION << "\n";
        return 0;
    }

#ifdef _WIN32
    InstallStartupGuards();
#endif

    CliOptions opts;
    if (!ParseArgs(args, opts))
        return 1;

    const RunOutcome outcome = RunTarget(opts);
    const int result = outcome.ok ? outcome.exitCode : 1;
#ifdef _WIN32
    //ExitProcess latches the exit code before CRT teardown, so a
    //crashing static destructor cannot corrupt it (nvm/ncc precedent).
    //Whether the atexit chain still runs under ExitProcess depends on
    //the CRT linkage (dynamic UCRT: yes, via DLL_PROCESS_DETACH;
    //static: no) -- restore the console code pages explicitly instead
    //of betting on the linkage, so every exit path behaves the same.
    RestoreConsoleCodePages();
    ExitProcess(static_cast<UINT>(result));
#else
    return result;
#endif
}
