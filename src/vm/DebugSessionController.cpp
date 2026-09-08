/*---
DebugSessionController.cpp — session state extracted from ndb's v0.4.0
CLI DebugSession: breakpoint table, line/pc resolution and the step-depth
state machine now serve every front end. Behavior contract: the dbg_*
e2e set runs the CLI adapter over this code unchanged.
---*/

#include "DebugSessionController.h"
#include "Disassembler.h"
#include "nlang/vm/CompiledModule.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace nlang {

namespace {

//Both separators: FilePath() records as-compiled (Windows backslashes),
//user input may use either.
std::string Basename(const std::string& path) {
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

//Set-time report label: "main (file.n:9)".
std::string LabelOf(const CompiledFunction& func, int line) {
    return func.name + " ("
        + (func.sourceFile.empty() ? std::string("?")
                                   : Basename(func.sourceFile))
        + ":" + std::to_string(line) + ")";
}

} // namespace

// --- display filter ---

bool DebugSessionController::IsHiddenLocalName(const std::string& name) {
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
        return true;
    if (!name.empty() && name[0] == '$')
        return true;
    return false;
}

std::string DebugSessionController::DisplayName(const std::string& name) {
    if (name == "__this") return "this";
    return name;
}

// --- construction ---

DebugSessionController::DebugSessionController(const CompiledModule& module,
    IDebugFrontEnd& frontEnd)
    : m_module(module), m_frontEnd(frontEnd) {}

// --- breakpoint table ---

int DebugSessionController::AddBreakpoint(const std::string& file, int line,
    bool exactFile) {
    if (line <= 0)
        return 0;   //source lines are 1-based: nothing can bind
    //Table key: one id per (normalized file, line).
    const std::string fileKey = NormalizePath(file);
    for (const auto& bp : m_breakpoints)
        if (bp.line == line && bp.fileKey == fileKey)
            return bp.id;

    //Every function whose recorded source matches contributes ALL of the
    //line's statement anchors: a line may hold several — consecutive
    //(multi-declarators, one-line if/else arms) or non-adjacent (try/
    //finally compiles each finally-body statement twice) — and every
    //anchor is a real execution path (BuildLinePcMap keeps every anchor,
    //no dedup).
    Breakpoint bp;
    for (size_t i = 0; i < m_module.functions.size(); ++i) {
        const auto& func = m_module.functions[i];
        bool match = exactFile
            ? NormalizePath(func.sourceFile) == fileKey
            : SourceFileMatches(func.sourceFile, file);
        if (!match) continue;
        for (const auto& e : BuildLinePcMap(func)) {
            //Int promotion compare: a line beyond the u16 line range must
            //find nothing, not wrap onto a wrong line.
            if (e.line != line) continue;
            if (bp.anchors.empty())
                bp.label = LabelOf(func, e.line);
            bp.anchors.push_back({static_cast<uint16_t>(i), e.pc});
        }
    }
    if (bp.anchors.empty())
        return 0;   //unbound: no statement anchor on that line
    bp.id = m_nextBreakpointId++;
    bp.fileKey = fileKey;
    bp.line = line;
    m_breakpoints.push_back(std::move(bp));
    return m_breakpoints.back().id;
}

int DebugSessionController::AddFunctionBreakpoint(
    const std::string& funcName) {
    //One id covers every same-named function's first statement.
    Breakpoint bp;
    for (size_t i = 0; i < m_module.functions.size(); ++i) {
        const auto& func = m_module.functions[i];
        if (func.name != funcName) continue;
        auto map = BuildLinePcMap(func);
        if (map.empty()) continue;   //no statements: would never hit
        if (bp.anchors.empty())
            bp.label = LabelOf(func, map.front().line);
        bp.anchors.push_back({static_cast<uint16_t>(i), map.front().pc});
    }
    if (bp.anchors.empty())
        return 0;
    bp.id = m_nextBreakpointId++;
    m_breakpoints.push_back(std::move(bp));
    return m_breakpoints.back().id;
}

bool DebugSessionController::DeleteBreakpoint(int id) {
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
        [&](const Breakpoint& bp) { return bp.id == id; });
    if (it == m_breakpoints.end())
        return false;
    m_breakpoints.erase(it);
    return true;
}

void DebugSessionController::SetBreakOnThrow(bool on) {
    m_breakOnThrow = on;
}

// --- frozen window discipline ---

void DebugSessionController::RequireFrozenWindow(const char* who) const {
    if (!m_pView)
        throw std::logic_error(std::string(who)
            + " outside the frozen window (OnStopped..WaitUntilResume)");
}

void DebugSessionController::Continue() {
    RequireFrozenWindow("Continue");
    m_mode = RunMode::Continue;
}

void DebugSessionController::StepInto() {
    RequireFrozenWindow("StepInto");
    m_mode = RunMode::StepInto;
}

void DebugSessionController::StepOver() {
    RequireFrozenWindow("StepOver");
    m_mode = RunMode::StepOver;
    m_stepDepth = m_pView->FrameCount();
}

void DebugSessionController::StepOut() {
    RequireFrozenWindow("StepOut");
    m_mode = RunMode::StepOut;
    m_stepDepth = m_pView->FrameCount();
}

const IVmDebugView& DebugSessionController::View() const {
    RequireFrozenWindow("View");
    return *m_pView;
}

std::vector<DebugSessionController::BreakpointRow>
DebugSessionController::BreakpointRows() const {
    std::vector<BreakpointRow> rows;
    rows.reserve(m_breakpoints.size());
    for (const auto& bp : m_breakpoints)
        rows.push_back({bp.id, bp.label, bp.hits});
    return rows;
}

// --- IDebugHooks ---

void DebugSessionController::OnStatement(const DebugStopInfo& stop,
    IVmDebugView& view) {
    m_pView = &view;

    const std::pair<uint16_t, uint16_t> anchor = {stop.funcIdx, stop.pc};
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
        [&](const Breakpoint& bp) {
            return std::find(bp.anchors.begin(), bp.anchors.end(), anchor)
                != bp.anchors.end();
        });
    if (it != m_breakpoints.end()) {
        it->hits += 1;
        Freeze(StopInfo{StopInfo::Reason::Breakpoint, it->id,
                        stop.pc, stop.line, stop.funcIdx, stop.depth});
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
        const auto reason = (m_mode == RunMode::InitialStop)
            ? StopInfo::Reason::Initial
            : StopInfo::Reason::Step;
        Freeze(StopInfo{reason, 0, stop.pc, stop.line, stop.funcIdx,
                        stop.depth});
    }
    m_pView = nullptr;
}

void DebugSessionController::OnThrow(const DebugStopInfo& stop,
    IVmDebugView& view) {
    //`break on throw`: freeze at the throw site before unwinding starts —
    //the full NLang stack and locals are still alive here.
    if (!m_breakOnThrow)
        return;
    m_pView = &view;
    Freeze(StopInfo{StopInfo::Reason::Throw, 0,
                    stop.pc, stop.line, stop.funcIdx, stop.depth});
    m_pView = nullptr;
}

//Announce the freeze, then block in the front end's pump until a resume
//command comes back (the front end calls the resume methods inside the
//window; WaitUntilResume returning resumes in place).
void DebugSessionController::Freeze(const StopInfo& info) {
    m_frontEnd.OnStopped(info);
    m_frontEnd.WaitUntilResume();
}

} // namespace nlang
