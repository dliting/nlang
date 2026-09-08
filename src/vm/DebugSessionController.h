/*---
DebugSessionController.h — front-end-agnostic debug session state: the
breakpoint table (one id per source line, every statement anchor of the
line under that id), function breakpoints, source-path matching, and the
step-depth state machine. The executor talks to IDebugHooks (this
class); front ends (ndb CLI, machine mode, in-process tests) implement
IDebugFrontEnd and are driven through StopInfo callbacks. Consumed via
PRIVATE include, like IDebugHooks.h.
Contract: single-threaded with the hosting executor. The frozen window
runs from OnStopped until WaitUntilResume returns; resume commands and
View() are valid only inside it (outside is a front-end programming
error — std::logic_error). Breakpoint table edits are also valid before
the first run (a front end may preset breakpoints).
---*/
#pragma once
#include "IDebugHooks.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nlang {

//Payload of one freeze. reason selects the front end's report flavor;
//line/funcIdx/depth mirror the engine's DebugStopInfo; breakpointId is
//set only for Reason::Breakpoint. (No pc: front ends that need it read
//FrameInfo, which also carries the func/file the reports are built
//from.)
struct StopInfo
{
    enum class Reason { Initial, Breakpoint, Step, Throw };
    Reason reason = Reason::Initial;
    int breakpointId = 0;
    uint16_t line = 0, funcIdx = 0;
    size_t depth = 0;
};

//Terminal/UI front end of a debug session. OnStopped opens the frozen
//window; WaitUntilResume blocks in the front end's own pump (command
//loop, protocol stdin) and its return resumes the program. OnExited/
//OnRuntimeError are session-end notifications fired by the embedder
//around Execute() — the controller never calls them (it does not own
//the run loop).
//Contract: OnStopped/WaitUntilResume must not let C++ exceptions escape
//(they would cross the NLang try/catch boundary into the executor — the
//front end now owns its command pump's exception boundary). And
//WaitUntilResume must issue a resume command — or end the process —
//before returning; returning without a resume leaves the run mode
//undefined.
class IDebugFrontEnd
{
public:
    virtual ~IDebugFrontEnd() = default;
    virtual void OnStopped(const StopInfo&) = 0;
    virtual void OnExited(int code) = 0;
    virtual void OnRuntimeError(const std::string& backtrace) = 0;
    virtual void WaitUntilResume() = 0;
};

struct CompiledModule;   //matches the definition in the public header

class DebugSessionController : public IDebugHooks
{
public:
    DebugSessionController(const CompiledModule& module,
        IDebugFrontEnd& frontEnd);

    //Breakpoint on one source line. Every statement anchor of the line
    //shares the returned id — hitting any of them is one hit of that id.
    //A repeated (file, line) request returns the existing id. 0 = no
    //executable anchor on that line (unbound request, nothing stored).
    //exactFile matches the full recorded path (a front end resolving a
    //bare line number against the selected frame); default matches by
    //path suffix ("mathutil.n" matches any directory).
    int AddBreakpoint(const std::string& file, int line,
        bool exactFile = false);

    //Breakpoint on the first statement of every same-named function
    //(methods and free functions share the bare-name pool) under one id.
    //0 = no function of that name has statements.
    int AddFunctionBreakpoint(const std::string& funcName);

    //False = no breakpoint with that id.
    bool DeleteBreakpoint(int id);
    void SetBreakOnThrow(bool on);   //freeze at throw sites before unwinding
    bool BreakOnThrow() const { return m_breakOnThrow; }

    //Resume commands. Valid only inside the frozen window; elsewhere
    //they throw std::logic_error (a resume queued outside the window
    //would silently corrupt the next stop decision).
    void Continue();
    void StepInto();
    void StepOver();
    void StepOut();

    //Read-only view over the frozen VM state. Valid only inside the
    //frozen window (std::logic_error outside).
    const IVmDebugView& View() const;

    //Read-only breakpoint projection for front-end display (ndb `i b`).
    struct BreakpointRow
    {
        int id = 0;
        std::string label;   //"main (file.n:9)"
        int hits = 0;
    };
    std::vector<BreakpointRow> BreakpointRows() const;

    //Display filter shared by the front ends: synthesized locals
    //(statement lowering, foreach expansion, finally trampolines) stay
    //internal; __this shows as this.
    static bool IsHiddenLocalName(const std::string& name);
    static std::string DisplayName(const std::string& name);

    //Path/location formatting shared by the front ends: the CLI prints
    //the frame locations the controller matches breakpoints with, under
    //the same rules (both separators, case-insensitive paths).
    static std::string Basename(const std::string& path);
    static std::string NormalizePath(const std::string& path);
    //Frame file for display: basename, "?" when there is none.
    static std::string ShownFile(const std::string& sourceFile);
    //"func (file:line)" — the shared stop/frame location format.
    static std::string LocationLabel(const std::string& funcName,
        const std::string& shownFile, unsigned line);

    //IDebugHooks — install via VmExecutor::SetDebugHooks.
    void OnStatement(const DebugStopInfo& stop, IVmDebugView& view) override;
    void OnThrow(const DebugStopInfo& stop, IVmDebugView& view) override;

private:
    enum class RunMode
    {
        InitialStop,   //stop at the first statement
        Continue,
        StepInto,      //s: next statement, any depth
        StepOver,      //n: next statement with depth <= recorded
        StepOut,       //f: next statement with depth <  recorded
    };

    struct Breakpoint
    {
        int id = 0;
        std::string label;   //"main (file.n:9)" for front-end reports
        int hits = 0;
        std::string key;   //table key: normalized file (line bp) / func name
        int line = 0;      //line bp's source line; 0 marks a function bp
        std::vector<std::pair<uint16_t, uint16_t>> anchors;  //(funcIdx, pc)
    };

    //Refuse window-bound calls made outside the frozen window.
    void RequireFrozenWindow(const char* who) const;
    //Announce the freeze, then block in the front end's pump.
    void Freeze(const StopInfo& info);

    const CompiledModule& m_module;
    IDebugFrontEnd& m_frontEnd;
    IVmDebugView* m_pView = nullptr;  //valid only inside the frozen window
    RunMode m_mode = RunMode::InitialStop;
    size_t m_stepDepth = 0;     //depth captured when StepOver/Out was issued
    int m_nextBreakpointId = 1;
    std::vector<Breakpoint> m_breakpoints;
    bool m_breakOnThrow = false;   //`catch on|off` (default off)
};

} // namespace nlang
