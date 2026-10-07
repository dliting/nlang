// Input-side unit tests: InputLineSource over an injected FILE* (line
// assembly, trailing partial line, CRLF, EOF stickiness, probes) and
// TokenView over a scripted LineSource (token slicing, mixed-read
// remainder semantics, readChar UTF-8 scalars, hasInput contract).
// Executor-level behavior is covered by test_native_host and e2e.

#include "InputLineSource.h"
#include "TokenView.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

// A readable FILE* whose contents we control; closed on destruction.
struct TempStream {
    std::FILE* file = nullptr;
    explicit TempStream(const std::string& bytes) {
        file = std::tmpfile();
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::rewind(file);
    }
    ~TempStream() { if (file) std::fclose(file); }
};

// Scripted line source: pops lines in order, Eof when empty.
struct ScriptedSource : LineSource {
    std::vector<std::string> lines;
    size_t next = 0;
    explicit ScriptedSource(std::initializer_list<std::string> scripted)
        : lines(scripted) {}
    InputReadStatus PullLine(std::string& line) override {
        if (next >= lines.size()) return InputReadStatus::Eof;
        line = lines[next++];
        return InputReadStatus::Ok;
    }
    bool HasMore() override { return next < lines.size(); }
};

InputReadStatus Pull(InputLineSource& src, std::string& line) {
    return src.PullLine(line);
}

} // namespace

//--- InputLineSource --------------------------------------------------------

void TestLinesAssembledAndEofSticky() {
    TempStream ts("a\nbb\nccc\n");
    InputLineSource src(ts.file);
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "a", "line 1");
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "bb", "line 2");
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "ccc", "line 3");
    CHECK(Pull(src, line) == InputReadStatus::Eof, "eof");
    CHECK(Pull(src, line) == InputReadStatus::Eof, "eof sticky");
    CHECK(!src.HasMore(), "hasMore false after eof");
}

void TestTrailingPartialLineOnce() {
    TempStream ts("abc");
    InputLineSource src(ts.file);
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "abc",
          "partial line delivered once");
    CHECK(Pull(src, line) == InputReadStatus::Eof, "then eof");
}

void TestCrLfStripped() {
    TempStream ts("x\r\ny\r\n");
    InputLineSource src(ts.file);
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "x", "crlf 1");
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "y", "crlf 2");
}

void TestEmptyLineIsALine() {
    TempStream ts("\n\n");
    InputLineSource src(ts.file);
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line.empty(), "empty line 1");
    CHECK(Pull(src, line) == InputReadStatus::Ok && line.empty(), "empty line 2");
    CHECK(Pull(src, line) == InputReadStatus::Eof, "eof after empties");
}

void TestHasMoreSeesUnslicedBufferedBytes() {
    //One line pulled, the rest still unsliced in the source's buffer:
    //the probe must answer from the buffer, not just the stream.
    TempStream ts("one\ntwo\n");
    InputLineSource src(ts.file);
    std::string line;
    Pull(src, line);
    CHECK(src.HasMore(), "buffered second line visible");
    Pull(src, line);
    CHECK(!src.HasMore(), "drained disk stream reports no more");
}

void TestHasInputEmptyStream() {
    TempStream ts("");
    InputLineSource src(ts.file);
    CHECK(!src.HasMore(), "empty stream: no input");
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Eof, "empty stream: eof pull");
}

void TestOpenPipeDeliversLinesImmediately() {
    //The nide regression pin: a line written to a STILL-OPEN pipe must
    //be deliverable right away. A fill-until-full read (fread) blocks
    //here until the write end closes — exactly the deadlock the nide
    //stdin row hit.
#if defined(_WIN32)
    int fds[2];
    if (_pipe(fds, 4096, _O_BINARY) != 0) {
        CHECK(false, "pipe create failed");
        return;
    }
    std::FILE* readEnd = _fdopen(fds[0], "rb");
    std::FILE* writeEnd = _fdopen(fds[1], "wb");
#else
    int fds[2];
    if (pipe(fds) != 0) {
        CHECK(false, "pipe create failed");
        return;
    }
    std::FILE* readEnd = fdopen(fds[0], "rb");
    std::FILE* writeEnd = fdopen(fds[1], "wb");
#endif
    std::fputs("Alice\n", writeEnd);
    std::fflush(writeEnd);   //line is in the pipe; write end STAYS OPEN
    {
        InputLineSource src(readEnd);
        std::string line;
        CHECK(Pull(src, line) == InputReadStatus::Ok && line == "Alice",
              "line delivered while pipe is open");
        CHECK(!src.HasMore(), "open pipe with no pending line: no input");
        std::fclose(writeEnd);
        CHECK(Pull(src, line) == InputReadStatus::Eof,
              "eof once the write end closes");
    }
    std::fclose(readEnd);
}

// An open pipe pair as FILE* ends (the interactive-stdin stand-in).
struct PipePair {
    std::FILE* readEnd = nullptr;
    std::FILE* writeEnd = nullptr;
    PipePair() {
#if defined(_WIN32)
        int fds[2];
        if (_pipe(fds, 4096, _O_BINARY) != 0) return;
        readEnd = _fdopen(fds[0], "rb");
        writeEnd = _fdopen(fds[1], "wb");
#else
        int fds[2];
        if (pipe(fds) != 0) return;
        readEnd = fdopen(fds[0], "rb");
        writeEnd = fdopen(fds[1], "wb");
#endif
    }
    ~PipePair() {
        if (readEnd) std::fclose(readEnd);
        if (writeEnd) std::fclose(writeEnd);
    }
    bool valid() const { return readEnd && writeEnd; }
};

void TestInputWaitCueFiresBeforeBlockingRead() {
    //Empty pipe, write end open: the pull must WAIT for input. The cue
    //fires first; acting as the typing user, its write then unblocks
    //the read — proving cue-before-block ordering without a console.
    PipePair pipe;
    if (!pipe.valid()) {
        CHECK(false, "pipe create failed");
        return;
    }
    InputLineSource src(pipe.readEnd);
    int fired = 0;
    src.SetOnInputWait([&] {
        ++fired;
        std::fputs("Alice\n", pipe.writeEnd);
        std::fflush(pipe.writeEnd);
    });
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "Alice",
          "line delivered after the cue's write");
    CHECK(fired == 1, "cue fired exactly once before the wait");
}

void TestInputWaitCueSilentWhenInputReady() {
    //A pre-fed line needs no wait: the cue must stay silent for it,
    //then fire for the follow-up pull on the drained pipe.
    PipePair pipe;
    if (!pipe.valid()) {
        CHECK(false, "pipe create failed");
        return;
    }
    std::fputs("Alice\n", pipe.writeEnd);
    std::fflush(pipe.writeEnd);
    InputLineSource src(pipe.readEnd);
    int fired = 0;
    src.SetOnInputWait([&] {
        ++fired;
        std::fputs("Bob\n", pipe.writeEnd);
        std::fflush(pipe.writeEnd);
    });
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "Alice",
          "pre-fed line");
    CHECK(fired == 0, "no cue while input is ready");
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "Bob",
          "second pull waits and is fed by the cue");
    CHECK(fired == 1, "cue fired for the waiting pull only");
}

void TestInputWaitCueSilentAtEof() {
    //EOF never waits: disk pulls that discover or re-report EOF must
    //not cue (nothing will block, and a program ending its input must
    //not see a last phantom prompt).
    TempStream ts("a\n");
    InputLineSource src(ts.file);
    int fired = 0;
    src.SetOnInputWait([&] { ++fired; });
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Ok && line == "a", "line");
    CHECK(Pull(src, line) == InputReadStatus::Eof, "eof pull");
    CHECK(Pull(src, line) == InputReadStatus::Eof, "eof sticky");
    CHECK(fired == 0, "no cue around eof");
}

void TestInputWaitCueSilentAtClosedPipe() {
    //A pipe whose write end is already closed never blocks: the pull
    //answers EOF immediately and must not cue (the phantom trailing
    //prompt a program would otherwise flash before exiting).
    PipePair pipe;
    if (!pipe.valid()) {
        CHECK(false, "pipe create failed");
        return;
    }
    std::fclose(pipe.writeEnd);
    pipe.writeEnd = nullptr;
    InputLineSource src(pipe.readEnd);
    int fired = 0;
    src.SetOnInputWait([&] { ++fired; });
    std::string line;
    CHECK(Pull(src, line) == InputReadStatus::Eof, "eof on closed pipe");
    CHECK(fired == 0, "no cue for an eof-bound pull");
}

//--- TokenView ---------------------------------------------------------------

void TestTokensSliceAndCrossLines() {
    ScriptedSource s{"10 20", "30"};
    TokenView v(s);
    std::string tok;
    CHECK(v.ReadToken(tok) == InputReadStatus::Ok && tok == "10", "token 1");
    CHECK(v.ReadToken(tok) == InputReadStatus::Ok && tok == "20", "token 2");
    CHECK(v.ReadToken(tok) == InputReadStatus::Ok && tok == "30", "token crosses to next line");
    CHECK(v.ReadToken(tok) == InputReadStatus::Eof, "token at eof");
}

void TestMixedReadRemainderKeepsLeadingSpace() {
    ScriptedSource s{"10 20"};
    TokenView v(s);
    std::string tok, line;
    v.ReadToken(tok);
    CHECK(v.ReadLine(line) == InputReadStatus::Ok && line == " 20",
          "remainder keeps leading space");
    CHECK(v.ReadLine(line) == InputReadStatus::Eof, "then eof");
}

void TestSpecExampleFullSequence() {
    //The manual's mixing example: input "10 20\n30\n" reads
    //10, 20, "", "30", then eof.
    ScriptedSource s{"10 20", "30"};
    TokenView v(s);
    std::string tok, line;
    CHECK(v.ReadToken(tok) == InputReadStatus::Ok && tok == "10", "spec 10");
    CHECK(v.ReadToken(tok) == InputReadStatus::Ok && tok == "20", "spec 20");
    CHECK(v.ReadLine(line) == InputReadStatus::Ok && line.empty(),
          "spec empty remainder");
    CHECK(v.ReadLine(line) == InputReadStatus::Ok && line == "30", "spec 30");
    CHECK(v.ReadLine(line) == InputReadStatus::Eof, "spec eof");
}

void TestWhitespaceDefinition() {
    ScriptedSource s{" \t\r x"};
    TokenView v(s);
    std::string tok;
    CHECK(v.ReadToken(tok) == InputReadStatus::Ok && tok == "x",
          "space, tab and mid-line CR are whitespace");
}

void TestReadCharScalars() {
    ScriptedSource s{"x y", "\xe4\xb8\xad"};   // "x y" then U+4E2D
    TokenView v(s);
    uint32_t ch = 0;
    bool invalid = true;
    CHECK(v.ReadChar(ch, invalid) == InputReadStatus::Ok && ch == 'x' && !invalid,
          "char x");
    CHECK(v.ReadChar(ch, invalid) == InputReadStatus::Ok && ch == 'y' && !invalid,
          "char y skips space");
    CHECK(v.ReadChar(ch, invalid) == InputReadStatus::Ok && ch == 0x4E2D && !invalid,
          "full utf-8 scalar");
    CHECK(v.ReadChar(ch, invalid) == InputReadStatus::Eof, "char at eof");
}

void TestReadCharInvalidUtf8() {
    ScriptedSource s{"\xff\xfe"};
    TokenView v(s);
    uint32_t ch = 0;
    bool invalid = false;
    CHECK(v.ReadChar(ch, invalid) == InputReadStatus::Ok && invalid,
          "bad lead byte flagged");
    ScriptedSource s2{"\xc0\x80"};   // overlong encoding of U+0000
    TokenView v2(s2);
    CHECK(v2.ReadChar(ch, invalid) == InputReadStatus::Ok && invalid,
          "overlong form flagged");
}

void TestHasInputContract() {
    ScriptedSource s{"a", ""};
    TokenView v(s);
    std::string line;
    CHECK(v.HasInput(), "pending first line");
    CHECK(v.ReadLine(line) == InputReadStatus::Ok && line == "a", "read a");
    CHECK(v.HasInput(), "pending empty final line");
    CHECK(v.ReadLine(line) == InputReadStatus::Ok && line.empty(),
          "empty final line is a line");
    CHECK(!v.HasInput(), "eof: no input");
    CHECK(v.ReadLine(line) == InputReadStatus::Eof, "readLine eof sentinel path");
}

void TestNoChannelPassthrough() {
    //TokenView is source-agnostic: NoChannel crosses it untouched (the
    //executor callback raises for it).
    struct NoChannelSource : LineSource {
        InputReadStatus PullLine(std::string&) override {
            return InputReadStatus::NoChannel;
        }
        bool HasMore() override { return false; }
    } s;
    TokenView v(s);
    std::string tok, line;
    uint32_t ch = 0;
    bool invalid = false;
    CHECK(v.ReadLine(line) == InputReadStatus::NoChannel, "readLine passthrough");
    CHECK(v.ReadToken(tok) == InputReadStatus::NoChannel, "readToken passthrough");
    CHECK(v.ReadChar(ch, invalid) == InputReadStatus::NoChannel,
          "readChar passthrough");
    CHECK(!v.HasInput(), "no channel: no input");
}

int main() {
    std::fprintf(stderr, "=== Input Source / Token View Tests ===\n");
    TestLinesAssembledAndEofSticky();
    TestTrailingPartialLineOnce();
    TestCrLfStripped();
    TestEmptyLineIsALine();
    TestHasMoreSeesUnslicedBufferedBytes();
    TestHasInputEmptyStream();
    TestOpenPipeDeliversLinesImmediately();
    TestInputWaitCueFiresBeforeBlockingRead();
    TestInputWaitCueSilentWhenInputReady();
    TestInputWaitCueSilentAtEof();
    TestInputWaitCueSilentAtClosedPipe();
    TestTokensSliceAndCrossLines();
    TestMixedReadRemainderKeepsLeadingSpace();
    TestSpecExampleFullSequence();
    TestWhitespaceDefinition();
    TestReadCharScalars();
    TestReadCharInvalidUtf8();
    TestHasInputContract();
    TestNoChannelPassthrough();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
