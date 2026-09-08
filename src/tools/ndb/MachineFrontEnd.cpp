// --- ndb machine-mode front end (line protocol over stdin/stdout) ---
// Command parsing and event emission; all session state lives in the
// controller. The command loops own the exception boundary into the VM:
// every command failure becomes an err event (IDebugFrontEnd contract).

#include "MachineFrontEnd.h"
#include "Disassembler.h"
#include <cstdio>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace nlang {

namespace {

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

const char* ReasonName(StopInfo::Reason reason) {
    switch (reason) {
    case StopInfo::Reason::Breakpoint: return "breakpoint";
    case StopInfo::Reason::Step: return "step";
    case StopInfo::Reason::Throw: return "throw";
    case StopInfo::Reason::Initial: break;
    }
    return "initial";
}

} // namespace

MachineFrontEnd::MachineFrontEnd(const CompiledModule& module,
    std::istream& in, std::ostream& out)
    : m_module(module), m_in(in), m_out(out) {}

// --- command pumps ---

void MachineFrontEnd::PumpUntilRun()
{
    EmitLine(protocol::MakeEvent("hello", {protocol::kProtocolVersion}));
    for (;;) {
        std::string line;
        if (!std::getline(m_in, line))
            QuitSession();   //EOF before run: nothing to report
        line = Trim(line);
        if (line.empty()) continue;
        if (Dispatch(line)) {
            m_started = true;   //`run`: the caller executes the program
            return;
        }
    }
}

void MachineFrontEnd::WaitUntilResume()
{
    for (;;) {
        std::string line;
        if (!std::getline(m_in, line))
            QuitSession();   //EOF behaves like the CLI's q
        line = Trim(line);
        if (line.empty()) continue;
        //Contract: this loop only returns after a resume command went
        //through — quit-by-EOF exits the process instead.
        if (Dispatch(line))
            return;
    }
}

bool MachineFrontEnd::Dispatch(const std::string& line)
{
    size_t sp = line.find_first_of(" \t");
    std::string head = line.substr(0, sp);
    std::string arg = Trim(line.substr(head.size()));
    try {
        if (head == "b") { DoBreakpoint(arg); return false; }
        if (head == "bfunc") { DoBreakFunction(arg); return false; }
        if (head == "d") { DoDelete(arg); return false; }
        if (head == "breakthrow") { DoBreakThrow(arg); return false; }
        if (head == "bt") { DoBacktrace(); return false; }
        if (head == "frame") { DoFrame(arg); return false; }
        if (head == "locals") { DoLocals(); return false; }
        if (head == "run") {
            if (m_started)
                EmitLine(protocol::MakeEvent("err",
                    {"run is only valid before the program starts"}));
            return !m_started;
        }
        if (head == "c") { m_pController->Continue(); return true; }
        if (head == "s") { m_pController->StepInto(); return true; }
        if (head == "n") { m_pController->StepOver(); return true; }
        if (head == "f") { m_pController->StepOut(); return true; }
        EmitLine(protocol::MakeEvent("err",
            {"unknown command '" + head + "'"}));
        return false;
    } catch (const std::exception& e) {
        //Exception boundary: window violations (std::logic_error) and
        //malformed requests surface as err events, never escape.
        EmitLine(protocol::MakeEvent("err", {e.what()}));
        return false;
    }
}

// --- commands ---

void MachineFrontEnd::DoBreakpoint(const std::string& arg)
{
    //Machine form `b <file> <line>`: the line is the tail after the
    //LAST space, so paths containing spaces stay one field (mirrors the
    //CLI's rfind(':') split of <file.n:LINE>). The file part is trimmed:
    //scripts are hand-written, so double spaces are a real input shape.
    const size_t sp = arg.rfind(' ');
    if (sp == std::string::npos || !IsAllDigits(arg.substr(sp + 1)))
        throw std::runtime_error("b expects <file> <line>");
    const std::string file = Trim(arg.substr(0, sp));
    const int line = std::atoi(arg.substr(sp + 1).c_str());
    const int id = m_pController->AddBreakpoint(file, line);
    EmitLine(protocol::MakeEvent("bp",
        {std::to_string(id), file, std::to_string(line),
         id == 0 ? "unbound" : "bound"}));
}

void MachineFrontEnd::DoBreakFunction(const std::string& name)
{
    if (name.empty())
        throw std::runtime_error("bfunc expects <funcName>");
    const int id = m_pController->AddFunctionBreakpoint(name);
    //Resolved first-statement location: mirror the controller's binding
    //scan (first same-named function that owns statement anchors).
    std::string file;
    int line = 0;
    for (const auto& func : m_module.functions) {
        if (func.name != name) continue;
        const auto map = BuildLinePcMap(func);
        if (map.empty()) continue;
        file = func.sourceFile;
        line = map.front().line;
        break;
    }
    EmitLine(protocol::MakeEvent("bp",
        {std::to_string(id), file, std::to_string(line),
         id == 0 ? "unbound" : "bound"}));
}

void MachineFrontEnd::DoDelete(const std::string& arg)
{
    if (!IsAllDigits(arg))
        throw std::runtime_error("d expects <id>");
    if (!m_pController->DeleteBreakpoint(std::atoi(arg.c_str())))
        throw std::runtime_error("no breakpoint " + arg);
    EmitLine(protocol::MakeEvent("done", {"d"}));
}

void MachineFrontEnd::DoBreakThrow(const std::string& arg)
{
    if (arg == "on")
        m_pController->SetBreakOnThrow(true);
    else if (arg == "off")
        m_pController->SetBreakOnThrow(false);
    else
        throw std::runtime_error("breakthrow expects on|off");
    EmitLine(protocol::MakeEvent("done", {"breakthrow"}));
}

void MachineFrontEnd::DoBacktrace()
{
    const IVmDebugView& view = m_pController->View();
    const size_t count = view.FrameCount();
    for (size_t depth = 0; depth < count; ++depth)
        EmitFrame(depth);
    EmitLine(protocol::MakeEvent("done", {"bt"}));
}

void MachineFrontEnd::DoFrame(const std::string& arg)
{
    if (!IsAllDigits(arg))
        throw std::runtime_error("frame expects <n>");
    //View() first: outside the frozen window this errs like bt/locals.
    const IVmDebugView& view = m_pController->View();
    const size_t depth = static_cast<size_t>(std::atoi(arg.c_str()));
    if (depth >= view.FrameCount())
        throw std::runtime_error("no frame " + arg);
    m_selectedFrame = depth;   //selection routes later locals requests
    EmitFrame(depth);
    EmitLine(protocol::MakeEvent("done", {"frame"}));
}

void MachineFrontEnd::DoLocals()
{
    for (const auto& local
            : m_pController->View().FrameLocals(m_selectedFrame)) {
        if (DebugSessionController::IsHiddenLocalName(local.name))
            continue;
        EmitLine(protocol::MakeEvent("local",
            {DebugSessionController::DisplayName(local.name),
             local.kindName, local.display}));
    }
    EmitLine(protocol::MakeEvent("done", {"locals"}));
}

// --- IDebugFrontEnd ---

void MachineFrontEnd::OnStopped(const StopInfo& stop)
{
    m_selectedFrame = 0;
    const DebugFrameInfo frame = m_pController->View().FrameInfo(0);
    EmitLine(protocol::MakeEvent("stopped",
        {ReasonName(stop.reason), std::to_string(stop.breakpointId),
         frame.funcName, frame.sourceFile, std::to_string(frame.line),
         std::to_string(stop.depth),
         std::to_string(m_pController->View().FrameCount())}));
}

void MachineFrontEnd::OnExited(int code)
{
    EmitLine(protocol::MakeEvent("exited", {std::to_string(code)}));
}

void MachineFrontEnd::OnRuntimeError(const std::string& report)
{
    EmitLine(protocol::MakeEvent("error", {report}));
}

// --- IHostIo ---

void MachineFrontEnd::OnOutput(std::string_view text)
{
    EmitLine(protocol::MakeEvent("output", {std::string(text)}));
}

// --- emission ---

void MachineFrontEnd::EmitFrame(size_t depth)
{
    const DebugFrameInfo frame = m_pController->View().FrameInfo(depth);
    EmitLine(protocol::MakeEvent("frame",
        {std::to_string(depth), frame.funcName, frame.sourceFile,
         std::to_string(frame.line)}));
}

void MachineFrontEnd::EmitLine(const std::string& line)
{
    m_out << line << '\n';
    //Windows pipes buffer fully: an unflushed protocol line deadlocks
    //the IDE side, so force every event out at once.
    m_out.flush();
    std::fflush(stdout);
}

void MachineFrontEnd::QuitSession()
{
    //Hard exit, static destructors bypassed (CLI discipline — the
    //runtime's static dtors can corrupt the exit code). Exit code 0:
    //EOF is a deliberate session end, not a program result.
    m_out.flush();
#ifdef _WIN32
    ExitProcess(0);
#else
    std::_Exit(0);
#endif
}

} // namespace nlang
