// io native module — the dynamically-loaded implementation of the `io`
// namespace (console IO and whole-text file IO). Built as nlang_io.dll /
// libnlang_io.so and loaded through the same mechanism as a third-party
// native library.
//
// Depends only on the NativeHost ABI and the C++ standard library. Console
// IO goes through the host callbacks (the host owns stdin/stdout and the
// session's IO policy); file IO uses std::fstream and raises IOException via
// the host on failure.
#include <nlang/vm/NativeHost.h>

#include <cstdint>
#include <fstream>
#include <string>

using nlang::native::ArgInt;
using nlang::native::ArgString;
using nlang::native::ReturnInt;
using nlang::native::ReturnString;

namespace {

// DoS bound for readFile: same 16 MiB cap the VM enforces for untrusted
// length prefixes. A larger file is a program bug, not a reason to OOM.
constexpr unsigned long long kMaxReadBytes = 16ull * 1024ull * 1024ull;

void RaiseIo(NativeHost* h, const std::string& msg) {
    nlang::native::Raise(h, NEXC_IOException, msg);
}

// print: the call site has already converted the `any` argument to a string
// handle, so here it is read and written verbatim followed by a newline.
void IoPrint(NativeHost* h, uint8_t*, const uint8_t* a, int) {
    const std::string text = ArgString(h, a, 0);
    h->writeOutput(h, text.c_str());
    h->writeOutput(h, "\n");
}

// write: print's prompt-building counterpart — same bytes, no newline.
// The host flushes each write, so the prompt reaches the reader before
// readLine blocks on it.
void IoWrite(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    (void)ret;
    const std::string text = ArgString(h, a, 0);
    h->writeOutput(h, text.c_str());
}

// eprint: print's diagnostic twin. The host routes the bytes to stderr
// in console mode and to the session's merged output view under a host;
// the newline mirrors print's shape.
void IoEprint(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    (void)ret;
    const std::string text = ArgString(h, a, 0);
    //Two calls, not one concatenation: same bytes, no temp alloc.
    h->writeError(h, text.c_str());
    h->writeError(h, "\n");
}

// readLine: delegate to the host (it owns stdin and the session
// policy). null = end of input folds to the "" sentinel — the NLang
// level cannot distinguish them without hasInput.
void IoReadLine(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    const char* line = h->readLine(h);
    ReturnInt(ret, h->newString(h, line ? line : ""));
}

// readToken/readChar/hasInput: thin delegations — the host owns the
// cursor state (only it can keep mixed reads coherent). End of input
// is raised host-side, so the folds below only defend against a host
// that somehow returns without raising.
void IoReadToken(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    const char* token = h->readToken(h);
    ReturnInt(ret, h->newString(h, token ? token : ""));
}

void IoReadChar(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    uint32_t codePoint = 0;
    if (h->readChar(h, &codePoint) != 0)
        codePoint = 0;   //end of input raised host-side; fold only
    ReturnInt(ret, static_cast<int32_t>(codePoint));
}

void IoHasInput(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    ReturnInt(ret, h->hasInput(h) ? 1 : 0);
}

// readFile: whole binary file, with an open check, a size cap and a short-read
// check; every failure raises IOException.
void IoReadFile(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    const std::string path = ArgString(h, a, 0);
    std::ifstream in(path, std::ios_base::binary);
    if (!in.is_open())
        RaiseIo(h, "io.readFile: cannot open \"" + path + "\".");

    in.seekg(0, std::ios_base::end);
    const std::streamoff size = in.tellg();
    if (size < 0)
        RaiseIo(h, "io.readFile: cannot determine size of \"" + path + "\".");
    if (static_cast<unsigned long long>(size) > kMaxReadBytes)
        RaiseIo(h, "io.readFile: \"" + path + "\" is too large to read.");

    std::string content(static_cast<size_t>(size), '\0');
    in.seekg(0, std::ios_base::beg);
    if (size > 0)
        in.read(&content[0], size);
    if (in.bad() || in.gcount() != static_cast<std::streamsize>(size))
        RaiseIo(h, "io.readFile: read error on \"" + path + "\".");

    ReturnString(h, ret, content);
}

// Shared body for writeFile (truncate) and appendFile (append).
void WriteLike(NativeHost* h, const uint8_t* a, bool append,
               const char* funcName) {
    const std::string path = ArgString(h, a, 0);
    const std::string content = ArgString(h, a, 1);
    std::ofstream out(path,
        std::ios_base::binary | std::ios_base::out
        | (append ? std::ios_base::app : std::ios_base::trunc));
    if (!out.is_open())
        RaiseIo(h, std::string("io.") + funcName
                + ": cannot open \"" + path + "\".");
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (out.fail())
        RaiseIo(h, std::string("io.") + funcName
                + ": write error on \"" + path + "\".");
}

void IoWriteFile(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    (void)ret;
    WriteLike(h, a, /*append=*/false, "writeFile");
}
void IoAppendFile(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    (void)ret;
    WriteLike(h, a, /*append=*/true, "appendFile");
}

} // namespace

// Module entry point.
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "io", "print",      &IoPrint);
    reg(registry, "io", "write",      &IoWrite);
    reg(registry, "io", "eprint",     &IoEprint);
    reg(registry, "io", "readLine",   &IoReadLine);
    reg(registry, "io", "hasInput",   &IoHasInput);
    reg(registry, "io", "readToken",  &IoReadToken);
    reg(registry, "io", "readChar",   &IoReadChar);
    reg(registry, "io", "readFile",   &IoReadFile);
    reg(registry, "io", "writeFile",  &IoWriteFile);
    reg(registry, "io", "appendFile", &IoAppendFile);
    return NLANG_HOST_ABI_VERSION;
}
