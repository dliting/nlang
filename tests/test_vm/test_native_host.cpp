// NativeHost callback integration tests. Real .n sources are compiled and
// run; the natives they call are registered in-process and exercise every
// NativeHost callback (string creation/read, List<string> creation, output,
// input, exceptions and the PRNG). No VM internals are mocked.
#include "test_support/test_scratch.h"

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/runtime/PrimitiveTypes.h"
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

void HsCreate(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnString(host, ret, std::string("created"));
}
void HsEcho(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    native::ReturnString(host, ret, native::ArgString(host, args, 0));
}
void HsList(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const char* items[] = {"x", "y"};
    const int64_t lens[] = {1, 1};
    native::ReturnInt(ret, host->newListString(host, items, lens, 2));
}
void HsRaiseIo(NativeHost* host, uint8_t*, const uint8_t*, int) {
    native::Raise(host, NEXC_IOException, std::string("io boom"));
}
void HsRaiseBase(NativeHost* host, uint8_t*, const uint8_t*, int) {
    native::Raise(host, NEXC_Base, std::string("base boom"));
}
void HsRawRandom(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, static_cast<int32_t>(host->nextRandom(host)));
}
void HsSeededRandom(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    host->seedRandom(host, native::ArgInt(args, 0));
    native::ReturnInt(ret, static_cast<int32_t>(host->nextRandom(host)));
}
void HsReadLine(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    int64_t len = 0;
    const char* line = host->readLine(host, &len);
    native::ReturnInt(ret, native::NewString(host, line, len));
}

void RegisterHs(VmExecutor& e, const std::string& pkg) {
    //The package prefix is the TU the `native` declarations were compiled
    //from (runSource writes <tag>.n), because a free function's VM key is
    //now "<package>.<name>" (phase 5). Methods keep unqualified names - none here.
    e.RegisterNative(pkg + ".hsCreate", &HsCreate);
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
    "native string hsCreate();\n"
    "native string hsEcho(string s);\n"
    "native List<string> hsList();\n"
    "native void hsRaiseIo();\n"
    "native void hsRaiseBase();\n"
    "native int hsRawRandom();\n"
    "native int hsSeededRandom(int seed);\n"
    "native string hsReadLine();\n";

void TestCreateEchoListRandomRaise() {
    const std::string source = kDecls +
        "int main() {\n"
        "  if (hsCreate() != \"created\") return 1;\n"
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
    CHECK(rc == 0, "create/echo/list/random/raise callbacks (rc)");
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
//literal. Without the non-zero create the boundary check would reject
//these legitimate empty results.
void HsEmptyString(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, native::NewString(host, std::string()));
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
    //contract under test is the C pointer and the answered length —
    //non-NULL, zero-length body.
    int64_t len = -1;
    const char* s = host->getString(host, native::ArgInt(args, 0), &len);
    native::ReturnInt(ret,
        (s && len == 0 && s[0] == '\0') ? 0 : 1);
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

//A string is the UTF-8 byte sequence the VM stores, so a NUL byte is
//ordinary content: the language's `\0` escape makes one and
//`string.length()` counts it (a 3-byte "a\0b" literal reports length 3).
//Every NativeHost string transfer therefore carries a byte length. While
//they were NUL-terminated, a "a\0b" crossing the boundary arrived as one
//byte in both directions while the VM still reported length 3 — the two
//faces of the same string disagreed, which the string chapter does not
//allow.
void HsNulMade(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnString(host, ret, std::string("a\0b", 3));
}

void HsNulArgLength(NativeHost* host, uint8_t* ret,
                    const uint8_t* args, int) {
    //Raw getString (not the ArgString std::string wrapper): the contract
    //under test is the byte length the out-param answers.
    int64_t len = 0;
    const char* s = host->getString(host, native::ArgInt(args, 0), &len);
    native::ReturnInt(ret, s ? static_cast<int32_t>(len) : -1);
}

void HsNulList(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const char* items[] = {"a\0b", "c"};
    const int64_t lens[] = {3, 1};
    native::ReturnInt(ret, host->newListString(host, items, lens, 2));
}

void HsNulRaise(NativeHost* host, uint8_t*, const uint8_t*, int) {
    native::Raise(host, NEXC_IOException, std::string("a\0b", 3));
}

struct NulCapturingIo : IHostIo {
    std::string out;
    std::string err;
    void OnOutput(std::string_view v) override { out.append(v); }
    void OnError(std::string_view v) override { err.append(v); }
};

void TestNativeEmbeddedNulBytes() {
    const std::string source =
        "native string hsEcho(string s);\n"
        "native string hsNulMade();\n"
        "native int hsNulArgLength(string s);\n"
        "native List<string> hsNulList();\n"
        "native void hsNulRaise();\n"
        "int main() {\n"
        //newString out, getString and the echo helper back: one byte
        //sequence in both directions.
        "  if (hsNulMade().length() != 3) return 1;\n"
        "  if (hsEcho(\"a\\0b\") != \"a\\0b\") return 2;\n"
        "  if (hsNulArgLength(\"a\\0b\") != 3) return 3;\n"
        "  List<string> l = hsNulList();\n"
        "  if (l.length() != 2) return 4;\n"
        "  if (l.get(0).length() != 3 || l.get(1).length() != 1) return 5;\n"
        //The io package's byte conversions are the user-visible face of the
        //same two transfers.
        "  if (io.toBytes(\"a\\0b\").length() != 3) return 6;\n"
        "  byte[] raw = byte[3];\n"
        "  raw[0] = 97;\n"
        "  raw[1] = 0;\n"
        "  raw[2] = 98;\n"
        "  if (io.fromBytes(raw).length() != 3) return 7;\n"
        //The raise side carries the message bytes too.
        "  int excLen = 0;\n"
        "  bool caught = false;\n"
        "  try {\n"
        "    hsNulRaise();\n"
        "  } catch (IOException e) {\n"
        "    caught = true;\n"
        "    excLen = e.message.length();\n"
        "  }\n"
        "  if (!caught) return 8;\n"
        "  if (excLen != 3) return 9;\n"
        //The console channels write bytes verbatim (the host seam already
        //takes a string_view, so only the ABI boundary was dropping them).
        "  io.write(\"a\\0b\");\n"
        "  io.eprint(\"x\\0y\");\n"
        "  return 0;\n"
        "}\n";
    NulCapturingIo nulIo;
    std::string err;
    const int rc = runSource("host_nul", source, [&nulIo](VmExecutor& e) {
        RegisterHs(e, "host_nul");
        e.RegisterNative("host_nul.hsNulMade", &HsNulMade);
        e.RegisterNative("host_nul.hsNulArgLength", &HsNulArgLength);
        e.RegisterNative("host_nul.hsNulList", &HsNulList);
        e.RegisterNative("host_nul.hsNulRaise", &HsNulRaise);
        e.SetHostIo(&nulIo);
    }, &err);
    if (rc != 0) {
        std::fprintf(stderr, "NUL rows: program rc=%d err=[%s]\n", rc,
                     err.c_str());
    }
    CHECK(rc == 0, "embedded NUL survives every native string transfer (rc)");
    CHECK(nulIo.out == std::string("a\0b", 3),
          "writeOutput writes the NUL bytes verbatim");
    CHECK(nulIo.err == std::string("x\0y\n", 4),
          "writeError writes the NUL bytes verbatim");
}

void TestReadLineKeepsNulBytes() {
    const fs::path dir = scratchDir();
    const fs::path inFile = dir / "stdin_nul.txt";
    {
        std::ofstream out(inFile, std::ios::binary);
        out.write("a\0b\n", 4);
    }
    if (!std::freopen(inFile.string().c_str(), "r", stdin)) {
        ++g_fail;
        std::fprintf(stderr, "FAIL: could not redirect stdin (NUL line)\n");
        return;
    }
    const std::string source =
        "int main() {\n"
        "  string s = io.readLine();\n"
        "  if (s.length() != 3) return 1;\n"
        "  return 0;\n"
        "}\n";
    int rc = runSource("host_nul_line", source, [](VmExecutor&) {});
    CHECK(rc == 0, "readLine keeps a NUL byte inside the line (rc)");
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

//v12 ABI (D8): a native instance method reads its receiver's runtime
//instantiation through the three type-argument callbacks — count, the
//per-argument kind (the RTK code) and the per-argument name (the
//canonical name). One composed answer pins all three at once:
//"<count>|<kind>:<name>|..." with the out-of-range sentinels (kind
//0xFF, name null) probed right past the end. A non-template receiver
//answers count 0 and the same sentinels. The receiver arrives as the
//method's slot-0 argument (the __this handle), which CallNative fills
//for unqualified non-static callees (D8: method keys never carry a
//package prefix).
void HsTargs(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const uint32_t count = host->typeArgCount(host);
    std::string s = std::to_string(count) + "|";
    for (uint32_t i = 0; i < count; ++i)
        s += std::to_string(static_cast<int>(host->typeArgKind(host, i)))
           + ":" + host->typeArgName(host, i) + "|";
    if (host->typeArgKind(host, count) != 0xFF
        || host->typeArgName(host, count) != nullptr)
        s += "SENTINEL-FAIL|";
    native::ReturnString(host, ret, s.c_str());
}

void TestNativeTypeArgs() {
    const std::string intAnswer =
        "1|" + std::to_string(static_cast<int>(RTK_Int32)) + ":int|";
    const std::string stringAnswer =
        "1|" + std::to_string(static_cast<int>(RTK_String)) + ":string|";
    const std::string source =
        "class Box<T> {\n"
        "    public Box() { }\n"
        "    public native string targs();\n"
        "}\n"
        "class Plain {\n"
        "    public Plain() { }\n"
        "    public native string targs();\n"
        "}\n"
        "native string hstargs();\n"
        "int main() {\n"
        "    Box<int> bi = Box();\n"
        "    Box<string> bs = Box();\n"
        "    Plain p = Plain();\n"
        "    if (bi.targs() != \"" + intAnswer + "\") return 1;\n"
        "    if (bs.targs() != \"" + stringAnswer + "\") return 2;\n"
        "    if (p.targs() != \"0|\") return 3;\n"
        "    if (hstargs() != \"0|\") return 4;\n"
        "    return 40;\n"
        "}\n";
    int rc = runSource("host_type_args", source, [](VmExecutor& e) {
        e.RegisterNative("targs", &HsTargs);   //methods keep bare keys
        e.RegisterNative("host_type_args.hstargs", &HsTargs);   //free function
    });
    CHECK(rc == 40,
          "v12 type-argument callbacks answer count/kind/name (rc)");
}

} // namespace

int main() {
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    std::fprintf(stderr, "=== NativeHost Callback Integration Tests ===\n");
    TestCreateEchoListRandomRaise();
    TestReadLine();
    TestWriteOutput();
    TestNullStringHandleRejected();
    TestNativeEmptyStringRoundTrips();
    TestNativeEmptyStringArgIsNotNull();
    TestNativeEmbeddedNulBytes();
    TestReadLineKeepsNulBytes();
    TestInputReadersOverStdin();
    TestReadCharScalars();
    TestInputPromptSilentOnRedirectedStdout();
    TestGenericMemberDefault();
    TestGenericUnaryAndThisDefaults();
    TestNativeTypeArgs();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
