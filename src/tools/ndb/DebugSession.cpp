// --- ndb command session (CLI adapter over DebugSessionController) ---
// Command parsing, text formatting and the source cache; all session
// state lives in the controller. The command loop runs inside the
// frozen window (program frozen); returning from a resume command
// resumes in place. The loop's try/catch is the exception boundary the
// VM relies on: a C++ exception escaping into the executor would cross
// the NLang try/catch boundary (IDebugHooks.h contract).

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

//Basename/NormalizePath mirror DebugSessionController.cpp: the CLI
//formats the frame locations the controller matches breakpoints with.
std::string Basename(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string NormalizePath(const std::string& p) {
    std::string s;
    s.reserve(p.size());
    for (char c : p) {
        s += (c == '\\') ? '/'
            : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
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

// --- IDebugFrontEnd ---

void DebugSession::OnStopped(const StopInfo& stop) {
    m_selectedFrame = 0;
    switch (stop.reason) {
    case StopInfo::Reason::Breakpoint:
        ReportStop("Breakpoint " + std::to_string(stop.breakpointId) + ",");
        break;
    case StopInfo::Reason::Throw:
        ReportStop("Throw:");
        break;
    case StopInfo::Reason::Initial:
    case StopInfo::Reason::Step:
        ReportStop("Stopped:");
        break;
    }
}

//CLI parity: main() prints the exit code / runtime error itself (same
//bytes as before the controller extraction), so the CLI front end has
//nothing to add on session end. (The machine front end reports through
//these instead.)
void DebugSession::OnExited(int) {}
void DebugSession::OnRuntimeError(const std::string&) {}

// --- command loop ---

void DebugSession::WaitUntilResume() {
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
        m_pController->Continue();
        return true;
    } else if (head == "s" || head == "step") {
        m_pController->StepInto();
        return true;
    } else if (head == "n" || head == "next") {
        m_pController->StepOver();
        return true;
    } else if (head == "f" || head == "finish") {
        m_pController->StepOut();
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

// --- stop reporting ---

void DebugSession::ReportStop(const std::string& prefix) {
    DebugFrameInfo fi = m_pController->View().FrameInfo(0);
    m_out << prefix << " " << fi.funcName << " ("
          << (fi.sourceFile.empty() ? std::string("?")
                                    : Basename(fi.sourceFile))
          << ":" << fi.line << ")\n";
    m_out.flush();
}

//Set-time echo: "Breakpoint <id> at <label>" (gdb form).
void DebugSession::ReportBreakpoint(int id) {
    for (const auto& row : m_pController->BreakpointRows()) {
        if (row.id == id) {
            m_out << "Breakpoint " << row.id << " at " << row.label
                  << "\n";
            return;
        }
    }
}

// --- commands ---

void DebugSession::DoBreak(const std::string& arg) {
    //Three address forms: <file.n:LINE>, bare LINE (selected frame's
    //file, exact match), or function name (every same-named function
    //— methods and free functions share the bare-name pool).
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
        fileSpec = m_pController->View()
                       .FrameInfo(m_selectedFrame).sourceFile;
        if (fileSpec.empty()) {
            m_out << "Current frame has no source file; "
                     "use b <file.n:LINE>.\n";
            return;
        }
    }

    if (byLine) {
        //One id per line: every anchor of the line lives under it.
        int id = m_pController->AddBreakpoint(fileSpec, lineNo, exactFile);
        if (id == 0) {
            m_out << "No statement at " << arg << ".\n";
            return;
        }
        ReportBreakpoint(id);
    } else {
        //The controller folds all same-named functions into one id and
        //reports 0 for both "no such function" and "no statements"; the
        //distinction only shapes the message, so scan for it here.
        bool anyFunc = std::any_of(m_module.functions.begin(),
            m_module.functions.end(),
            [&](const CompiledFunction& f) { return f.name == arg; });
        int id = m_pController->AddFunctionBreakpoint(arg);
        if (id == 0) {
            if (!anyFunc)
                m_out << "No function '" << arg << "'.\n";
            else
                m_out << "Function '" << arg << "' has no statements; "
                         "a breakpoint would never hit.\n";
            return;
        }
        ReportBreakpoint(id);
    }
    m_out.flush();
}

void DebugSession::DoInfoBreakpoints() {
    const auto rows = m_pController->BreakpointRows();
    if (rows.empty()) {
        m_out << "No breakpoints.\n";
    } else {
        for (const auto& row : rows)
            m_out << "  " << row.id << "  " << row.label
                  << "  hits=" << row.hits << "\n";
    }
    m_out.flush();
}

void DebugSession::DoDelete(const std::string& arg) {
    if (!IsAllDigits(arg)) {
        m_out << "Usage: d <id>\n";
        return;
    }
    int id = std::atoi(arg.c_str());
    if (!m_pController->DeleteBreakpoint(id)) {
        m_out << "No breakpoint number " << id << ".\n";
        return;
    }
    m_out << "Deleted breakpoint " << id << ".\n";
    m_out.flush();
}

void DebugSession::DoBacktrace() {
    const IVmDebugView& view = m_pController->View();
    size_t count = view.FrameCount();
    //Basename disambiguation: the same basename from two different
    //paths prints the full path for those frames.
    std::vector<std::string> files;
    files.reserve(count);
    for (size_t d = 0; d < count; ++d)
        files.push_back(view.FrameInfo(d).sourceFile);
    for (size_t d = 0; d < count; ++d) {
        DebugFrameInfo fi = view.FrameInfo(d);
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
    const IVmDebugView& view = m_pController->View();
    if (arg.empty()) {
        m_out << "Frame " << m_selectedFrame << " selected.\n";
        return;
    }
    if (!IsAllDigits(arg)) {
        m_out << "Usage: frame <n>\n";
        return;
    }
    size_t n = static_cast<size_t>(std::atoi(arg.c_str()));
    if (n >= view.FrameCount()) {
        m_out << "No such frame.\n";
        return;
    }
    m_selectedFrame = n;
    DebugFrameInfo fi = view.FrameInfo(n);
    m_out << "#" << n << "  " << fi.funcName << " ("
          << (fi.sourceFile.empty() ? std::string("?")
                                    : Basename(fi.sourceFile))
          << ":" << fi.line << ")\n";
    m_out.flush();
}

void DebugSession::DoInfoLocals() {
    bool any = false;
    for (const auto& l
            : m_pController->View().FrameLocals(m_selectedFrame)) {
        if (DebugSessionController::IsHiddenLocalName(l.name)) continue;
        m_out << DebugSessionController::DisplayName(l.name) << " = "
              << l.display << "\n";
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
    //p searches ALL names (hidden included — it is the escape hatch
    //when the filtered display hides something relevant); `this`
    //aliases the __this slot.
    std::string alias = (arg == "this") ? "__this" : "";
    for (const auto& l
            : m_pController->View().FrameLocals(m_selectedFrame)) {
        if (l.name == arg || (!alias.empty() && l.name == alias)) {
            m_out << DebugSessionController::DisplayName(l.name) << " = "
                  << l.display << "\n";
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
    if (!arg.empty() && !IsAllDigits(arg)) {
        m_out << "Usage: l [line]\n";
        return;
    }
    DebugFrameInfo fi = m_pController->View().FrameInfo(m_selectedFrame);
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
    DebugFrameInfo fi = m_pController->View().FrameInfo(m_selectedFrame);
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
        m_pController->SetBreakOnThrow(true);
    } else if (arg == "off") {
        m_pController->SetBreakOnThrow(false);
    } else if (!arg.empty()) {
        m_out << "Usage: catch on|off\n";
        m_out.flush();
        return;
    }
    m_out << "Break on throw: "
          << (m_pController->BreakOnThrow() ? "on" : "off")
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
