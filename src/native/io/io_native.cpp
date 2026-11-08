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
using nlang::native::ArgLong;
using nlang::native::ArgString;
using nlang::native::NewArray;
using nlang::native::ReturnInt;
using nlang::native::ReturnLong;
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

// v0.13.0 net (r1 note 5): the generic IO surface — IO parking and the
// byte helpers moved out of the net package (string is a byte container;
// UTF-8 bytes pass through untouched). The monotonic clock moved to
// system.monoMs in v0.13.4 (the host ABI nowMs callback stays: it is the
// deadline domain of parkForIo).

// parkForIo: registered on the blocking channel, so the GIL is
// released around this call and the host can park the context and
// release the carrier. The ABI return value (0 = parked, 1 = re-entry
// after wake) is the park protocol's private answer: the script
// surface returns void — on wake the VM re-executes this call and the
// script resumes after it. deadlineMs <= 0 means no deadline.
void IoParkForIo(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    (void)ret;
    const int64_t deadlineMs = ArgLong(a, 2);
    h->parkForIo(h, ArgInt(a, 0),
                 static_cast<unsigned>(ArgInt(a, 1)),
                 deadlineMs, deadlineMs > 0 ? 1 : 0);
}

// toBytes: string bytes -> rank-1 byte array. Cells are int32 each
// and the low byte carries the value (the array store's narrow-cell
// convention); the minted array is zero-initialized and every cell
// below is written.
void IoToBytes(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    const std::string text = ArgString(h, a, 0);
    const int32_t len = static_cast<int32_t>(text.size());
    const int32_t handle = NewArray(h, NEK_Byte, len);
    NativeArrayView view;
    h->getArrayView(h, handle, &view);
    int32_t* cells = static_cast<int32_t*>(view.data);
    for (int32_t i = 0; i < len; ++i)
        cells[i] = static_cast<unsigned char>(
            text[static_cast<size_t>(i)]);
    ReturnInt(ret, handle);
}

// fromBytes: rank-1 byte/ubyte array -> string from the cells' low
// bytes. A view (slice syntax) is a legal argument: the ABI view
// reports offset/strides into the owning payload. Symmetric port of
// the committed net.fromBytes semantics (r1 note 5 moved the surface).
void IoFromBytes(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NativeArrayView view;
    nlang::native::ArgArray(h, a, 0, &view);
    if (view.rank != 1
        || (view.elemKind != NEK_Byte && view.elemKind != NEK_UByte)) {
        RaiseIo(h, "io.fromBytes: expected a rank-1 byte array");
        return;
    }
    const int32_t* cells = static_cast<const int32_t*>(view.data);
    const int64_t o = view.offset;
    const int64_t step = view.strides[0];
    std::string content;
    content.reserve(static_cast<size_t>(view.elemCount));
    for (int64_t i = 0; i < view.elemCount; ++i)
        content.push_back(static_cast<char>(static_cast<unsigned char>(
            cells[o + i * step])));
    ReturnString(h, ret, content);
}

} // namespace

// Module entry point. Registration channel per function: print/write/
// eprint only forward already-built strings through the host callbacks
// (non-blocking, run under the VM's lock); every input and file function
// may wait unbounded on the outside world (stdin, disk) and registers
// through the blocking channel — the VM parks the calling context and
// releases its lock around those calls.
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "io", "print",      &IoPrint);
    reg(registry, "io", "write",      &IoWrite);
    reg(registry, "io", "eprint",     &IoEprint);
    regBlocking(registry, "io", "readLine",   &IoReadLine);
    regBlocking(registry, "io", "hasInput",   &IoHasInput);
    regBlocking(registry, "io", "readToken",  &IoReadToken);
    regBlocking(registry, "io", "readChar",   &IoReadChar);
    regBlocking(registry, "io", "readFile",   &IoReadFile);
    regBlocking(registry, "io", "writeFile",  &IoWriteFile);
    regBlocking(registry, "io", "appendFile", &IoAppendFile);
    regBlocking(registry, "io", "parkForIo",  &IoParkForIo);
    reg(registry, "io", "toBytes",    &IoToBytes);
    reg(registry, "io", "fromBytes",  &IoFromBytes);
    return NLANG_HOST_ABI_VERSION;
}
