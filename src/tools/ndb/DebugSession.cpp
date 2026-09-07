// --- ndb command session ---
// Implements IDebugHooks for the CLI debugger. The command loop runs
// inside the callback (program frozen); returning resumes in place.
// RunCommandLoop's try/catch is the exception boundary the VM relies
// on: a C++ exception escaping into the executor would cross the
// NLang try/catch boundary (IDebugHooks.h contract).

#include "DebugSession.h"
#include "Disassembler.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace nlang {

namespace {

//Display filter: synthesized locals (statement lowering, foreach
//expansion, finally trampolines) stay internal to the compiler.
bool IsHiddenLocalName(const std::string& name) {
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
        return true;
    if (!name.empty() && name[0] == '$')
        return true;
    return false;
}

std::string DisplayName(const std::string& name) {
    if (name == "__this") return "this";
    return name;
}

std::string Basename(const std::string& path) {
    //Both separators: FilePath() records as-compiled (Windows
    //backslashes), user input may use either.
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

//Normalize for matching: forward slashes + lowercase (Windows paths
//are case-insensitive).
std::string NormalizePath(const std::string& p) {
    std::string s;
    s.reserve(p.size());
    for (char c : p) {
        s += (c == '\\') ? '/'
            : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

//Does a function's recorded source match the user's file spec? The
//spec may be a full path or a suffix like "mathutil.n".
bool SourceFileMatches(const std::string& sourceFile,
    const std::string& fileSpec) {
    if (sourceFile.empty() || fileSpec.empty()) return false;
    std::string sf = NormalizePath(sourceFile);
    std::string spec = NormalizePath(fileSpec);
    if (sf == spec) return true;
    return sf.size() > spec.size()
        && sf.compare(sf.size() - spec.size(), spec.size(), spec) == 0
        && sf[sf.size() - spec.size() - 1] == '/';
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool IsAllDigits(const std::string& s) {
    return !s.empty()
        && s.find_first_not_of("0123456789") == std::string::npos;
}

} // namespace

DebugSession::DebugSession(const CompiledModule& module,
    std::string modulePath, std::istream& in, std::ostream& out)
    : m_module(module), m_modulePath(std::move(modulePath)),
      m_in(in), m_out(out), m_sourceCache(m_modulePath) {}

DebugSession::~DebugSession() = default;

// --- hooks ---

void DebugSession::OnStatement(const DebugStopInfo& stop,
    IVmDebugView& view) {
    m_pView = &view;
    m_selectedFrame = 0;

    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
        [&](const Breakpoint& bp) {
            return bp.funcIdx == stop.funcIdx && bp.pc == stop.pc;
        });
    if (it != m_breakpoints.end()) {
        it->hits += 1;
        ReportStop("Breakpoint " + std::to_string(it->id) + ",", stop);
        RunCommandLoop();
        m_pView = nullptr;
        return;
    }
    bool stopHere = false;
    switch (m_mode) {
    case RunMode::Continue:
        break;
    case RunMode::InitialStop:
    case RunMode::StepInto:
        stopHere = true;
        break;
    case RunMode::StepOver:
        stopHere = (stop.depth <= m_stepDepth);
        break;
    case RunMode::StepOut:
        stopHere = (stop.depth < m_stepDepth);
        break;
    }
    if (stopHere) {
        ReportStop("Stopped:", stop);
        RunCommandLoop();
    }
    m_pView = nullptr;
}

void DebugSession::OnThrow(const DebugStopInfo& stop, IVmDebugView& view) {
    //`catch on`: freeze at the throw site before unwinding starts —
    //the full NLang stack and locals are still alive here.
    if (!m_breakOnThrow)
        return;
    m_pView = &view;
    m_selectedFrame = 0;
    ReportStop("Throw:", stop);
    RunCommandLoop();
    m_pView = nullptr;
}

// --- stop reporting ---

void DebugSession::ReportStop(const std::string& prefix,
    const DebugStopInfo& stop) {
    DebugFrameInfo fi = m_pView->FrameInfo(0);
    m_out << prefix << " " << fi.funcName << " ("
          << (fi.sourceFile.empty() ? std::string("?")
                                    : Basename(fi.sourceFile))
          << ":" << fi.line << ")\n";
    m_out.flush();
}

// --- command loop ---

void DebugSession::RunCommandLoop() {
    for (;;) {
        m_out << "(ndb) ";
        m_out.flush();
        std::string line;
        if (!std::getline(m_in, line)) {
            Quit();  //EOF behaves like q (e2e relies on this)
        }
        line = Trim(line);
        if (line.empty()) continue;
        try {
            if (RunCommand(line))
                return;  //resume command issued
        } catch (const std::exception& e) {
            //Exception boundary: nothing may escape into the VM.
            m_out << "ndb error: " << e.what() << "\n";
        }
    }
}

bool DebugSession::RunCommand(const std::string& cmd) {
    size_t sp = cmd.find_first_of(" \t");
    std::string head = cmd.substr(0, sp);
    std::string arg = Trim(cmd.substr(head.size()));

    if (head == "b" || head == "break") {
        if (arg.empty())
            m_out << "Usage: b <file.n:LINE | LINE | funcName>\n";
        else
            DoBreak(arg);
    } else if (head == "i" || head == "info") {
        if (arg == "b" || arg == "break")
            DoInfoBreakpoints();
        else if (arg == "locals")
            DoInfoLocals();
        else
            m_out << "Usage: i b | i locals\n";
    } else if (head == "d" || head == "delete") {
        DoDelete(arg);
    } else if (head == "c" || head == "continue") {
        m_mode = RunMode::Continue;
        return true;
    } else if (head == "s" || head == "step") {
        m_mode = RunMode::StepInto;
        return true;
    } else if (head == "n" || head == "next") {
        m_mode = RunMode::StepOver;
        m_stepDepth = m_pView ? m_pView->FrameCount() : 1;
        return true;
    } else if (head == "f" || head == "finish") {
        m_mode = RunMode::StepOut;
        m_stepDepth = m_pView ? m_pView->FrameCount() : 1;
        return true;
    } else if (head == "bt" || head == "backtrace") {
        DoBacktrace();
    } else if (head == "frame") {
        DoFrame(arg);
    } else if (head == "p" || head == "print") {
        DoPrint(arg);
    } else if (head == "l" || head == "list") {
        DoList(arg);
    } else if (head == "x") {
        DoDisassemble();
    } else if (head == "catch") {
        DoCatch(arg);
    } else if (head == "q" || head == "quit") {
        Quit();
    } else if (head == "help") {
        DoHelp();
    } else {
        m_out << "Unknown command '" << head << "'. Type 'help'.\n";
    }
    return false;
}

// --- commands ---

void DebugSession::DoBreak(const std::string& arg) {
    //Three address forms: <file.n:LINE>, bare LINE (selected frame's
    //file, exact match), or function name (every same-named function
    //— methods and free functions share the bare-name pool).
    struct Candidate { uint16_t funcIdx; uint16_t pc; uint16_t line; };
    std::vector<Candidate> found;
    std::string fileSpec;
    int lineNo = 0;
    bool byLine = false;
    bool exactFile = false;

    //rfind: Windows drive colon is in the FILE part, LINE is the tail.
    size_t colon = arg.rfind(':');
    if (colon != std::string::npos
        && IsAllDigits(arg.substr(colon + 1))) {
        fileSpec = arg.substr(0, colon);
        lineNo = std::atoi(arg.substr(colon + 1).c_str());
        byLine = true;
    } else if (IsAllDigits(arg)) {
        lineNo = std::atoi(arg.c_str());
        byLine = true;
        exactFile = true;
        if (m_pView)
            fileSpec = m_pView->FrameInfo(m_selectedFrame).sourceFile;
        if (fileSpec.empty()) {
            m_out << "Current frame has no source file; "
                     "use b <file.n:LINE>.\n";
            return;
        }
    }

    for (size_t i = 0; i < m_module.functions.size(); ++i) {
        const auto& func = m_module.functions[i];
        if (byLine) {
            bool match = exactFile
                ? NormalizePath(func.sourceFile) == NormalizePath(fileSpec)
                : SourceFileMatches(func.sourceFile, fileSpec);
            if (!match) continue;
            //ALL entries of the line become breakpoints: a line may hold
            //several statement anchors — consecutive (multi-declarators,
            //one-line if/else arms) or non-adjacent (try/finally compiles
            //each finally-body statement twice) — and every anchor is a
            //real execution path (see fact table, BuildLinePcMap same-line
            //semantics; the map keeps every anchor, no dedup).
            for (const auto& e : BuildLinePcMap(func)) {
                //Int promotion compare: a lineNo beyond the u16 line
                //range must find nothing, not wrap onto a wrong line.
                if (e.line == lineNo)
                    found.push_back({static_cast<uint16_t>(i),
                                     e.pc, e.line});
            }
        } else {
            if (func.name != arg) continue;
            auto map = BuildLinePcMap(func);
            if (map.empty()) {
                m_out << "Function '" << arg << "' has no statements; "
                         "a breakpoint would never hit.\n";
                continue;
            }
            found.push_back({static_cast<uint16_t>(i),
                             map.front().pc, map.front().line});
        }
    }

    if (found.empty()) {
        if (byLine)
            m_out << "No statement at " << arg << ".\n";
        else
            m_out << "No function '" << arg << "'.\n";
        return;
    }
    for (const auto& c : found) {
        const auto& func = m_module.functions[c.funcIdx];
        Breakpoint bp;
        bp.id = m_nextBreakpointId++;
        bp.funcIdx = c.funcIdx;
        bp.pc = c.pc;
        bp.label = func.name + " ("
            + (func.sourceFile.empty() ? std::string("?")
                                       : Basename(func.sourceFile))
            + ":" + std::to_string(c.line) + ")";
        m_out << "Breakpoint " << bp.id << " at " << bp.label << "\n";
        m_breakpoints.push_back(std::move(bp));
    }
    m_out.flush();
}

void DebugSession::DoInfoBreakpoints() {
    if (m_breakpoints.empty()) {
        m_out << "No breakpoints.\n";
    } else {
        for (const auto& bp : m_breakpoints)
            m_out << "  " << bp.id << "  " << bp.label
                  << "  hits=" << bp.hits << "\n";
    }
    m_out.flush();
}

void DebugSession::DoDelete(const std::string& arg) {
    if (!IsAllDigits(arg)) {
        m_out << "Usage: d <id>\n";
        return;
    }
    int id = std::atoi(arg.c_str());
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
        [&](const Breakpoint& bp) { return bp.id == id; });
    if (it == m_breakpoints.end()) {
        m_out << "No breakpoint number " << id << ".\n";
        return;
    }
    m_breakpoints.erase(it);
    m_out << "Deleted breakpoint " << id << ".\n";
    m_out.flush();
}

void DebugSession::DoBacktrace() {
    if (!m_pView) return;
    size_t count = m_pView->FrameCount();
    //Basename disambiguation: the same basename from two different
    //paths prints the full path for those frames.
    std::vector<std::string> files;
    files.reserve(count);
    for (size_t d = 0; d < count; ++d)
        files.push_back(m_pView->FrameInfo(d).sourceFile);
    for (size_t d = 0; d < count; ++d) {
        DebugFrameInfo fi = m_pView->FrameInfo(d);
        std::string shown = fi.sourceFile.empty()
            ? "?" : Basename(fi.sourceFile);
        for (size_t o = 0; o < count; ++o) {
            if (o == d || fi.sourceFile.empty()) continue;
            if (Basename(files[o]) == shown
                && NormalizePath(files[o])
                    != NormalizePath(fi.sourceFile)) {
                shown = fi.sourceFile;
                break;
            }
        }
        char buf[512];
        //Two spaces after the number, matching DoFrame's "#N  " form.
        std::snprintf(buf, sizeof(buf), "#%zu  %s (%s:%u)",
            d, fi.funcName.c_str(), shown.c_str(),
            static_cast<unsigned>(fi.line));
        m_out << buf << "\n";
    }
    m_out.flush();
}

void DebugSession::DoFrame(const std::string& arg) {
    if (!m_pView) return;
    if (arg.empty()) {
        m_out << "Frame " << m_selectedFrame << " selected.\n";
        return;
    }
    if (!IsAllDigits(arg)) {
        m_out << "Usage: frame <n>\n";
        return;
    }
    size_t n = static_cast<size_t>(std::atoi(arg.c_str()));
    if (n >= m_pView->FrameCount()) {
        m_out << "No such frame.\n";
        return;
    }
    m_selectedFrame = n;
    DebugFrameInfo fi = m_pView->FrameInfo(n);
    m_out << "#" << n << "  " << fi.funcName << " ("
          << (fi.sourceFile.empty() ? std::string("?")
                                    : Basename(fi.sourceFile))
          << ":" << fi.line << ")\n";
    m_out.flush();
}

void DebugSession::DoInfoLocals() {
    if (!m_pView) return;
    bool any = false;
    for (const auto& l : m_pView->FrameLocals(m_selectedFrame)) {
        if (IsHiddenLocalName(l.name)) continue;
        m_out << DisplayName(l.name) << " = " << l.display << "\n";
        any = true;
    }
    if (!any) m_out << "No visible locals.\n";
    m_out.flush();
}

void DebugSession::DoPrint(const std::string& arg) {
    if (arg.empty()) {
        m_out << "Usage: p <name>\n";
        return;
    }
    if (!m_pView) return;
    //p searches ALL names (hidden included — it is the escape hatch
    //when the filtered display hides something relevant); `this`
    //aliases the __this slot.
    std::string alias = (arg == "this") ? "__this" : "";
    for (const auto& l : m_pView->FrameLocals(m_selectedFrame)) {
        if (l.name == arg || (!alias.empty() && l.name == alias)) {
            m_out << DisplayName(l.name) << " = " << l.display << "\n";
            m_out.flush();
            return;
        }
    }
    m_out << "No local '" << arg << "' in frame "
          << m_selectedFrame << ".\n";
    m_out.flush();
}

void DebugSession::DoHelp() {
    m_out <<
        "b <file.n:LINE | LINE | funcName>  set breakpoint\n"
        "i b                                list breakpoints\n"
        "d <id>                             delete breakpoint\n"
        "c                                  continue\n"
        "s / n / f                          step into / over / out\n"
        "bt                                 backtrace\n"
        "frame <n>                          select frame\n"
        "info locals                        locals of selected frame\n"
        "p <name>                           print one local\n"
        "l [line]                           list source around line\n"
        "x                                  disassemble current frame\n"
        "catch on|off                       break on throw (default off)\n"
        "q                                  quit (kills the program)\n"
        "help                               this text\n";
    m_out.flush();
}

void DebugSession::DoList(const std::string& arg) {
    if (!m_pView) return;
    if (!arg.empty() && !IsAllDigits(arg)) {
        m_out << "Usage: l [line]\n";
        return;
    }
    DebugFrameInfo fi = m_pView->FrameInfo(m_selectedFrame);
    if (fi.sourceFile.empty()) {
        m_out << "No source file for this frame.\n";
        return;
    }
    //Window centered on the stop line (or the given line).
    const int kListWindowLines = 10;
    int center = fi.line;
    if (!arg.empty())
        center = std::atoi(arg.c_str());
    int begin = center - kListWindowLines / 2;
    if (begin < 1)
        begin = 1;
    const auto& lines = m_sourceCache.Lines(fi.sourceFile);
    for (int n = begin; n < begin + kListWindowLines; ++n) {
        const char* marker = (n == fi.line) ? "->" : "  ";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%5d", marker, n);
        if (n >= 1 && static_cast<size_t>(n) <= lines.size())
            m_out << buf << "\t" << lines[static_cast<size_t>(n) - 1]
                  << "\n";
        else
            m_out << buf << "\n";  //degraded / past EOF: number only
    }
    m_out.flush();
}

void DebugSession::DoDisassemble() {
    if (!m_pView) return;
    DebugFrameInfo fi = m_pView->FrameInfo(m_selectedFrame);
    if (fi.funcIdx >= m_module.functions.size())
        return;
    const auto& func = m_module.functions[fi.funcIdx];
    if (func.bytecode.empty()) {
        m_out << "No bytecode for " << fi.funcName << ".\n";
        m_out.flush();
        return;
    }
    //Shared Disassembler (same source as ndisasm); >> marks the
    //frame's current statement anchor pc.
    for (const auto& dl : DisassembleCode(func, m_module))
        m_out << (dl.pc == fi.pc ? ">>" : "  ") << "  " << dl.text
              << "\n";
    m_out << DisassembleTryBlocks(func);
    m_out.flush();
}

void DebugSession::DoCatch(const std::string& arg) {
    if (arg == "on") {
        m_breakOnThrow = true;
    } else if (arg == "off") {
        m_breakOnThrow = false;
    } else if (!arg.empty()) {
        m_out << "Usage: catch on|off\n";
        m_out.flush();
        return;
    }
    m_out << "Break on throw: " << (m_breakOnThrow ? "on" : "off")
          << "\n";
    m_out.flush();
}

void DebugSession::Quit() {
    //Hard exit, static destructors bypassed (nvm discipline — the
    //runtime's static dtors can corrupt the exit code). Exit code 0:
    //q/EOF is a deliberate session end, not a program result.
    m_out.flush();
#ifdef _WIN32
    ExitProcess(0);
#else
    std::_Exit(0);
#endif
}

} // namespace nlang
