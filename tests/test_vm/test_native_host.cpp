// NativeHost callback integration tests. Real .n sources are compiled and
// run; the natives they call are registered in-process and exercise every
// NativeHost callback (string mint/read, List<string> creation, output,
// input, exceptions and the PRNG). No VM internals are mocked.
#include "test_support/test_scratch.h"

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/langservice/SymbolIndex.h"
#include "VmExecutor.h"
#include "IHostFunctions.h"
#include "IHostIo.h"
#include "NcuLoader.h"
#include "NcuLinker.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define TEST_DUP _dup
#define TEST_DUP2 _dup2
#define TEST_CLOSE _close
#define TEST_BINARY_OUT(fd) _setmode(fd, _O_BINARY)
#else
#include <unistd.h>
#define TEST_DUP dup
#define TEST_DUP2 dup2
#define TEST_CLOSE close
#define TEST_BINARY_OUT(fd) ((void)(fd))
#endif

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

fs::path scratchDir() {
    static const auto dir = nlang::test::ScratchDir("nlang_test_native_host");
    fs::create_directories(dir);
    return dir;
}

//--- Test natives (registered in-process) ----------------------------------

void HsMint(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, host->newString(host, "minted"));
}
void HsEcho(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    native::ReturnString(host, ret, native::ArgString(host, args, 0));
}
void HsList(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const char* items[] = {"x", "y"};
    native::ReturnInt(ret, host->newListString(host, items, 2));
}
void HsRaiseIo(NativeHost* host, uint8_t*, const uint8_t*, int) {
    host->raiseException(host, NEXC_IOException, "io boom");
}
void HsRaiseBase(NativeHost* host, uint8_t*, const uint8_t*, int) {
    host->raiseException(host, NEXC_Base, "base boom");
}
void HsRawRandom(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, static_cast<int32_t>(host->nextRandom(host)));
}
void HsSeededRandom(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    host->seedRandom(host, native::ArgInt(args, 0));
    native::ReturnInt(ret, static_cast<int32_t>(host->nextRandom(host)));
}
void HsReadLine(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const char* line = host->readLine(host);
    native::ReturnInt(ret, host->newString(host, line));
}

void RegisterHs(VmExecutor& e, const std::string& pkg) {
    //The package prefix is the TU the `native` declarations were compiled
    //from (runSource writes <tag>.n), because a free function's VM key is
    //now "<package>.<name>" (phase 5). Methods keep bare names - none here.
    e.RegisterNative(pkg + ".hsMint", &HsMint);
    e.RegisterNative(pkg + ".hsEcho", &HsEcho);
    e.RegisterNative(pkg + ".hsList", &HsList);
    e.RegisterNative(pkg + ".hsRaiseIo", &HsRaiseIo);
    e.RegisterNative(pkg + ".hsRaiseBase", &HsRaiseBase);
    e.RegisterNative(pkg + ".hsRawRandom", &HsRawRandom);
    e.RegisterNative(pkg + ".hsSeededRandom", &HsSeededRandom);
    e.RegisterNative(pkg + ".hsReadLine", &HsReadLine);
}

// Compile + run; `configure` registers natives / installs host IO.
// `pError` (optional) receives the exception text when the run ends in a
// host-level failure instead of a normal exit.
int runSource(const std::string& tag, const std::string& source,
              std::function<void(VmExecutor&)> configure,
              std::string* pError = nullptr) {
    const fs::path dir = scratchDir();
    {
        std::ofstream out(dir / (tag + ".n"), std::ios::binary);
        out << "import io;\nimport math;\nimport fs;\n" << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / (tag + ".n")).string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    bool built = false;
    //BuildArtifacts (production path): the entry-unit .ncu carries
    //import slots for the built-in packages; the closure resolves at
    //load time from the scratch dir and the stdlib package.
    try { built = builder.BuildArtifacts(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        return -1;
    }
    if (!built) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::fprintf(stderr, "diag: %s\n", (*it)->FullMessage().c_str());
        return -1;
    }
    try {
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
        NcuLoader::Result loaded = NcuLoader::LoadClosure(
            (dir / (tag + ".ncu")).string(), loaderOpts);
        CompiledModule mod = NcuLinker::Link(std::move(loaded.units),
                                             loaded.entryKey);
        VmExecutor exec;
        configure(exec);
        return exec.Execute(mod);
    } catch (const std::exception& e) {
        if (pError) *pError = e.what();
        else std::fprintf(stderr, "load/link failed: %s\n", e.what());
        return -1;
    }
}

const std::string kDecls =
    "native string hsMint();\n"
    "native string hsEcho(string s);\n"
    "native List<string> hsList();\n"
    "native void hsRaiseIo();\n"
    "native void hsRaiseBase();\n"
    "native int hsRawRandom();\n"
    "native int hsSeededRandom(int seed);\n"
    "native string hsReadLine();\n";

void TestMintEchoListRandomRaise() {
    const std::string source = kDecls +
        "int main() {\n"
        "  if (hsMint() != \"minted\") return 1;\n"
        "  if (hsEcho(\"echo-me\") != \"echo-me\") return 2;\n"
        "  List<string> l = hsList();\n"
        "  if (l.length() != 2) return 3;\n"
        "  if (l.get(0) != \"x\" || l.get(1) != \"y\") return 4;\n"
        "  if (hsRawRandom() == hsRawRandom()) return 5;\n"
        "  if (hsSeededRandom(42) != hsSeededRandom(42)) return 6;\n"
        "  bool caughtIo = false;\n"
        "  try {\n"
        "    hsRaiseIo();\n"
        "  } catch (IOException e) {\n"
        "    caughtIo = true;\n"
        "  }\n"
        "  if (!caughtIo) return 7;\n"
        "  bool caughtBase = false;\n"
        "  try {\n"
        "    hsRaiseBase();\n"
        "  } catch (Exception e) {\n"
        "    caughtBase = true;\n"
        "  }\n"
        "  if (!caughtBase) return 8;\n"
        "  return 0;\n"
        "}\n";
    int rc = runSource("host_core", source,
                       [](VmExecutor& e) { RegisterHs(e, "host_core"); });
    CHECK(rc == 0, "mint/echo/list/random/raise callbacks (rc)");
}

void TestReadLine() {
    const fs::path dir = scratchDir();
    const fs::path inFile = dir / "stdin.txt";
    {
        std::ofstream out(inFile, std::ios::binary);
        out << "line-from-stdin\n";
    }
    // Redirect the C stdin (and, via default sync, std::cin) to the file.
    if (!std::freopen(inFile.string().c_str(), "r", stdin)) {
        ++g_fail;
        std::fprintf(stderr, "FAIL: could not redirect stdin\n");
        return;
    }
    const std::string source = kDecls +
        "int main() { if (hsReadLine() != \"line-from-stdin\") return 1; return 0; }\n";
    int rc = runSource("host_readline", source,
                       [](VmExecutor& e) { RegisterHs(e, "host_readline"); });
    CHECK(rc == 0, "readLine callback (rc)");
}

struct CapturingIo : IHostIo {
    std::string text;
    void OnOutput(std::string_view v) override { text += v; }
};

void TestWriteOutput() {
    CapturingIo io;
    const std::string source =
        "int main() { io.print(\"write-check\"); return 0; }\n";
    int rc = runSource("host_write", source, [&io](VmExecutor& e) {
        e.SetHostIo(&io);
    });
    CHECK(rc == 0, "writeOutput program runs");
    CHECK(io.text == "write-check\n", "writeOutput routes through host IO");
}

//0.13.4 (ruling D27): the native boundary keeps 0 = null (the ABI's
//existing null-handle convention) — a declared-string native that hands
//back 0 violated the contract and raises on every dispatch arm: direct,
//blocking, and the host-function hook. Internally handle 0 IS the empty
//string (ruling D26); the boundary is where the two domains meet, and
//the Go reference pattern (docs/ref/null_vs_go_string.md) is the model:
//check for null, express absence as an error, never a silent "".
void HsNullString(NativeHost*, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, 0);
}

struct NullStringHook : IHostFunctions {
    bool Call(const CompiledFunction& callee, const uint8_t*, uint32_t,
              uint8_t* resultCell) override {
        if (callee.name.find("hsNullString") == std::string::npos)
            return false;   //not ours: fall back to the native table
        const int32_t nullHandle = 0;
        std::memcpy(resultCell, &nullHandle, sizeof(nullHandle));
        return true;
    }
};

void TestNullStringHandleRejected() {
    const std::string decl = "native string hsNullString();\n";
    const std::string body =
        "int main() { string s = hsNullString();\n"
        "  if (s == \"\") return 1;\n"
        "  return 2;\n"
        "}\n";

    std::string errDirect;
    const int rcDirect = runSource("ns_direct", decl + body,
                                   [](VmExecutor& e) {
                                       e.RegisterNative(
                                           "ns_direct.hsNullString",
                                           &HsNullString);
                                   }, &errDirect);
    CHECK(rcDirect == -1 &&
          errDirect.find("null string") != std::string::npos,
          "direct native returning handle 0 raises at the boundary");

    std::string errBlocking;
    const int rcBlocking = runSource("ns_blocking", decl + body,
                                     [](VmExecutor& e) {
                                         e.RegisterNativeBlocking(
                                             "ns_blocking.hsNullString",
                                             &HsNullString);
                                     }, &errBlocking);
    CHECK(rcBlocking == -1 &&
          errBlocking.find("null string") != std::string::npos,
          "blocking native returning handle 0 raises at the boundary");

    NullStringHook hook;
    std::string errHook;
    const int rcHook = runSource("ns_hook", decl + body,
                                 [&hook](VmExecutor& e) {
                                     e.SetHostFunctions(&hook);
                                 }, &errHook);
    CHECK(rcHook == -1 && errHook.find("null string") != std::string::npos,
          "host-hook native returning handle 0 raises at the boundary");
}

//Ruling D27 companion: newString never hands out 0, so a native CAN
//legally return the empty string (net's "no peer address yet" shape) —
//the fresh non-interned "" reads as "" and compares equal to the
//literal. Without the non-zero mint the boundary check would reject
//these legitimate empty results.
void HsEmptyString(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, host->newString(host, ""));
}

void TestNativeEmptyStringRoundTrips() {
    const std::string source =
        "native string hsEmptyString();\n"
        "int main() { string s = hsEmptyString();\n"
        "  if (s != \"\") return 1;\n"
        "  if (s.length() != 0) return 2;\n"
        "  return 0;\n"
        "}\n";
    std::string err;
    const int rc = runSource("ns_empty", source, [](VmExecutor& e) {
        e.RegisterNative("ns_empty.hsEmptyString", &HsEmptyString);
    }, &err);
    CHECK(rc == 0, "native newString(\"\") returns a usable empty string");

    //The blocking arm stages the result cell through the blocking
    //protocol — the fresh empty handle must survive the staging memcpy
    //and the boundary check there too.
    std::string errBlk;
    const int rcBlk = runSource("ns_empty_blk", source, [](VmExecutor& e) {
        e.RegisterNativeBlocking("ns_empty_blk.hsEmptyString",
                                 &HsEmptyString);
    }, &errBlk);
    CHECK(rcBlk == 0,
          "blocking native newString(\"\") round-trips too");
}

//Ruling D26 at the argument side of the boundary: a native receiving the
//empty string reads a valid C string, never NULL (the Go pattern:
//C.CString("") also never returns nil).
void HsEmptyArgCheck(NativeHost* host, uint8_t* ret,
                     const uint8_t* args, int) {
    //Raw getString view (not the ArgString std::string wrapper): the
    //contract under test is the C pointer itself — non-NULL, empty body.
    const char* s = host->getString(host, native::ArgInt(args, 0));
    native::ReturnInt(ret, (s && s[0] == '\0') ? 0 : 1);
}

void TestNativeEmptyStringArgIsNotNull() {
    const std::string source =
        "native int hsEmptyArgCheck(string s);\n"
        "int main() {\n"
        "  if (hsEmptyArgCheck(\"\") != 0) return 1;\n"
        "  string e = \"\";\n"
        "  if (hsEmptyArgCheck(e) != 0) return 2;\n"
        "  return 0;\n"
        "}\n";
    std::string err;
    const int rc = runSource("ns_arg_empty", source, [](VmExecutor& e) {
        e.RegisterNative("ns_arg_empty.hsEmptyArgCheck", &HsEmptyArgCheck);
    }, &err);
    CHECK(rc == 0,
          "native getString on the empty string yields \"\", not NULL");
}

void TestInputReadersOverStdin() {
    const fs::path dir = scratchDir();
    const fs::path inFile = dir / "stdin_tokens.txt";
    {
        std::ofstream out(inFile, std::ios::binary);
        out << "10 20\n30\n";
    }
    if (!std::freopen(inFile.string().c_str(), "r", stdin)) {
        ++g_fail;
        std::fprintf(stderr, "FAIL: could not redirect stdin\n");
        return;
    }
    //The manual's mixing example, end to end through the real io
    //package: hasInput, two tokens, empty remainder, next line, EOF,
    //and the demanding-read raise.
    const std::string source =
        "int main() {\n"
        "  if (!io.hasInput()) return 1;\n"
        "  if (io.readInt() != 10) return 2;\n"
        "  if (io.readInt() != 20) return 3;\n"
        "  if (io.readLine() != \"\") return 4;\n"
        "  if (io.readLine() != \"30\") return 5;\n"
        "  if (io.hasInput()) return 6;\n"
        "  bool caught = false;\n"
        "  try { io.readToken(); } catch (IOException e) { caught = true; }\n"
        "  if (!caught) return 7;\n"
        "  return 0;\n"
        "}\n";
    int rc = runSource("host_tokens", source, [](VmExecutor&) {});
    CHECK(rc == 0, "token/line readers over redirected stdin (rc)");
}

void TestReadCharScalars() {
    const fs::path dir = scratchDir();
    const fs::path inFile = dir / "stdin_chars.txt";
    {
        std::ofstream out(inFile, std::ios::binary);
        out << "x \xe4\xb8\xad y\n";   // 'x', ' ', U+4E2D, ' ', 'y'
    }
    if (!std::freopen(inFile.string().c_str(), "r", stdin)) {
        ++g_fail;
        std::fprintf(stderr, "FAIL: could not redirect stdin\n");
        return;
    }
    const std::string source =
        "int main() {\n"
        "  if (io.readChar() != 'x') return 1;\n"
        "  if (io.readChar() != '中') return 2;\n"
        "  if (io.readChar() != 'y') return 3;\n"
        "  bool caught = false;\n"
        "  try { io.readChar(); } catch (IOException e) { caught = true; }\n"
        "  if (!caught) return 4;\n"
        "  return 0;\n"
        "}\n";
    int rc = runSource("host_chars", source, [](VmExecutor&) {});
    CHECK(rc == 0, "readChar full-code-point scalars (rc)");
}

//The interactive prompt must never reach a redirected stdout. stdin is
//pointed at the null DEVICE — a character device, so the line source's
//probe cannot see ready data and the wait cue (the prompt hook)
//genuinely fires — while stdout goes to a file. The read then answers
//EOF without blocking, and the captured stdout must hold exactly the
//program's own bytes: any "> " here is the prompt leaking into a
//pipeline (the byte-exactness contract the e2e runs rely on).
void TestInputPromptSilentOnRedirectedStdout() {
    const fs::path dir = scratchDir();
    const fs::path outFile = dir / "stdout_prompt.txt";
#if defined(_WIN32)
    const char* const nullDevice = "NUL";
#else
    const char* const nullDevice = "/dev/null";
#endif

    const int savedStdout = TEST_DUP(_fileno(stdout));
    if (!std::freopen(outFile.string().c_str(), "w", stdout)
        || !std::freopen(nullDevice, "r", stdin)
        || savedStdout < 0) {
        ++g_fail;
        std::fprintf(stderr, "FAIL: could not redirect the streams\n");
        return;
    }
    //Byte-exact comparison: no CRLF translation of the program's output.
    TEST_BINARY_OUT(_fileno(stdout));

    const std::string source =
        "int main() {\n"
        "  io.print(\"out\");\n"
        "  if (io.readLine() != \"\") return 1;\n"
        "  return 0;\n"
        "}\n";
    const int rc = runSource("host_prompt", source, [](VmExecutor&) {});
    std::fflush(stdout);
    std::string captured;
    {
        std::ifstream in(outFile, std::ios::binary);
        captured.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
    }
    std::fflush(stdout);
    TEST_DUP2(savedStdout, _fileno(stdout));   //restore the real stdout
    TEST_CLOSE(savedStdout);

    CHECK(rc == 0, "program over null-device stdin runs to exit 0");
    CHECK(captured == "out\n", "redirected stdout stays byte-exact");
}

//v0.12.3 Task 1: a generic instance's cloned default must EMIT, not
//merely bind — a member-expr default (`n = p.x`) references the formal
//p and Payload.x; the clone carries both as transferred bindings, and
//the caller-side staging evaluates p.x through the name-keyed formal
//override. The native returns the staged default (arg index 2),
//so a clone that silently emits nothing stages scratch garbage and the
//run result drifts off 11. The bodied ctor keeps the receiver a real
//allocated object (zero-arg construction bare-allocates in this
//window), so the native call itself runs.
//Method-native arg layout is [this, param1, param2, ...] — index 2 is
//the second declared parameter, the one the cloned default fills.
void HsReflectArg2(NativeHost*, uint8_t* ret, const uint8_t* args, int) {
    native::ReturnInt(ret, native::ArgInt(args, 2));
}

void TestGenericMemberDefault() {
    const std::string source =
        "class Payload {\n"
        "    public int x;\n"
        "    public Payload() { x = 11; }\n"
        "}\n"
        "class Box<T> {\n"
        "    public Box() { }\n"
        "    public native int f(Payload p, int n = p.x);\n"
        "}\n"
        "int main() {\n"
        "    Box<int> b = Box();\n"
        "    Payload p = Payload();\n"
        "    return b.f(p);\n"
        "}\n";
    int rc = runSource("host_generic_default", source, [](VmExecutor& e) {
        e.RegisterNative("f", &HsReflectArg2);
    });
    CHECK(rc == 11, "generic clone stages the member-expr default (p.x)");
}

//v0.12.3 Task 1 rounds 4-5: two emit-value pins for the cloned-default
//family. A unary default (`n = -p.x`) shares the operator arm — unary
//trees are SnBinaryExpr with a null right operand — so this pins that
//path end to end (expect -11). A this-receiver operand inside an
//operator default (`n = this.count + p.x`) pins the this-expression
//rebuild with its type channel: the field read goes through the
//caller-side receiver. VALUE LIMITATION of this window: instance
//fields stay zero until Task 5 lands initializers and ctor bodies, so
//the correct path stages 0 + Payload.x — a null type channel (the
//field read silently emitting nothing, staging an unwritten cell)
//yields the same sum while frames come up zeroed. The DISCRIMINATING
//pin for that failure is the structural one
//(declGenericCloneDefaultKeepsThisType); strengthen this value pin to
//a nonzero count (e.g. count = 7 expecting 18) when Task 5 flips
//initializers on.
void TestGenericUnaryAndThisDefaults() {
    const std::string unary =
        "class Payload {\n"
        "    public int x;\n"
        "    public Payload() { x = 11; }\n"
        "}\n"
        "class Box<T> {\n"
        "    public Box() { }\n"
        "    public native int f(Payload p, int n = -p.x);\n"
        "}\n"
        "int main() {\n"
        "    Box<int> b = Box();\n"
        "    Payload p = Payload();\n"
        "    return b.f(p);\n"
        "}\n";
    int rcUnary = runSource("host_generic_unary", unary,
                            [](VmExecutor& e) {
                                e.RegisterNative("f", &HsReflectArg2);
                            });
    CHECK(rcUnary == -11,
          "generic clone stages the unary default (-p.x)");

    const std::string thisForm =
        "class Payload {\n"
        "    public int x;\n"
        "    public Payload() { x = 11; }\n"
        "}\n"
        "class Box<T> {\n"
        "    public Box() { }\n"
        "    public int count;\n"
        "    public native int f(Payload p, int n = this.count + p.x);\n"
        "}\n"
        "int main() {\n"
        "    Box<int> b = Box();\n"
        "    Payload p = Payload();\n"
        "    return b.f(p);\n"
        "}\n";
    int rcThis = runSource("host_generic_this", thisForm,
                           [](VmExecutor& e) {
                               e.RegisterNative("f", &HsReflectArg2);
                           });
    CHECK(rcThis == 11,
          "generic clone stages the this-receiver operand "
          "(this.count + p.x)");
}

} // namespace

int main() {
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    std::fprintf(stderr, "=== NativeHost Callback Integration Tests ===\n");
    TestMintEchoListRandomRaise();
    TestReadLine();
    TestWriteOutput();
    TestNullStringHandleRejected();
    TestNativeEmptyStringRoundTrips();
    TestNativeEmptyStringArgIsNotNull();
    TestInputReadersOverStdin();
    TestReadCharScalars();
    TestInputPromptSilentOnRedirectedStdout();
    TestGenericMemberDefault();
    TestGenericUnaryAndThisDefaults();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
