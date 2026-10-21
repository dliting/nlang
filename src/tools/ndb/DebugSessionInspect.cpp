// --- ndb inspection commands (context/thread + frame/locals family) ---
// The state-inspection arm of the CLI session: context enumeration and
// focus (threads / t), backtrace, frame selection, locals and print.
// Parsing and the remaining commands live in DebugSession.cpp; all
// session state stays in the controller + DebugSession's own members.

#include "DebugSession.h"
#include <cstdio>
#include <cstdlib>

namespace nlang {

namespace {

bool IsAllDigits(const std::string& s) {
    return !s.empty()
        && s.find_first_not_of("0123456789") == std::string::npos;
}

} // namespace

//One `threads` row: the focus marker, the id, the scheduler state
//(suffixed @<carrier> while an execution window holds the context —
//the visible difference between running-now and queued runnable), and
//the top-frame location. Frameless contexts (finished) have no
//location.
void DebugSession::PrintContextRow(const DebugContextInfo& ci) {
    m_out << (ci.id == m_contextId ? "*" : " ") << " " << ci.id << "  "
          << ci.stateName;
    if (ci.heldByCarrier >= 0)
        m_out << "@" << ci.heldByCarrier;
    if (!ci.funcName.empty())
        m_out << "  "
              << DebugSessionController::LocationLabel(ci.funcName,
                     DebugSessionController::ShownFile(ci.sourceFile),
                     ci.line);
    m_out << "\n";
}

void DebugSession::DoThreads() {
    const IVmDebugView& view = m_pController->View();
    for (size_t id = 0; id < view.ContextCount(); ++id)
        PrintContextRow(view.ContextInfo(static_cast<int>(id)));
    m_out.flush();
}

//t <id>: switch the context selection — the inspection focus (bt/f/l/
//x/p/i locals address the selected context, parked frames included) AND
//the step target (K5: a later s/n/f steps the selected context; a
//parked one steps on wake). Frame selection resets — frame indices are
//per-context.
void DebugSession::DoContextFocus(const std::string& arg) {
    if (!IsAllDigits(arg)) {
        m_out << "Usage: t <id>\n";
        m_out.flush();
        return;
    }
    const IVmDebugView& view = m_pController->View();
    const int id = std::atoi(arg.c_str());
    if (static_cast<size_t>(id) >= view.ContextCount()) {
        m_out << "No context " << id << ".\n";
        m_out.flush();
        return;
    }
    m_contextId = id;
    m_selectedFrame = 0;
    m_pController->SetStepTargetContext(id);
    PrintContextRow(view.ContextInfo(id));
    m_out.flush();
}

void DebugSession::DoBacktrace() {
    const IVmDebugView& view = m_pController->View();
    size_t count = view.FrameCount(m_contextId);
    //Basename disambiguation: the same basename from two different
    //paths prints the full path for those frames.
    std::vector<std::string> files;
    files.reserve(count);
    for (size_t d = 0; d < count; ++d)
        files.push_back(view.FrameInfo(m_contextId, d).sourceFile);
    for (size_t d = 0; d < count; ++d) {
        DebugFrameInfo fi = view.FrameInfo(m_contextId, d);
        std::string shown = fi.sourceFile.empty()
            ? "?" : DebugSessionController::Basename(fi.sourceFile);
        for (size_t o = 0; o < count; ++o) {
            if (o == d || fi.sourceFile.empty()) continue;
            if (DebugSessionController::Basename(files[o]) == shown
                && DebugSessionController::NormalizePath(files[o])
                    != DebugSessionController::NormalizePath(
                        fi.sourceFile)) {
                shown = fi.sourceFile;
                break;
            }
        }
        //Two spaces after the number, matching DoFrame's "#N  " form.
        m_out << "#" << d << "  "
              << DebugSessionController::LocationLabel(fi.funcName,
                     shown, fi.line)
              << "\n";
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
    if (n >= view.FrameCount(m_contextId)) {
        m_out << "No such frame.\n";
        return;
    }
    m_selectedFrame = n;
    DebugFrameInfo fi = view.FrameInfo(m_contextId, n);
    m_out << "#" << n << "  "
          << DebugSessionController::LocationLabel(fi.funcName,
                 DebugSessionController::ShownFile(fi.sourceFile),
                 fi.line)
          << "\n";
    m_out.flush();
}

void DebugSession::DoInfoLocals() {
    bool any = false;
    for (const auto& l : m_pController->View()
                       .FrameLocals(m_contextId, m_selectedFrame)) {
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
    for (const auto& l : m_pController->View()
                       .FrameLocals(m_contextId, m_selectedFrame)) {
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

} // namespace nlang
