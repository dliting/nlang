#pragma once
#include "IDebugHooks.h"
#include "nlang/vm/CompiledModule.h"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace nlang {

//CLI front end implementing IDebugHooks. Owns every terminal
//concept — breakpoint table, step state, frame selection, display
//filtering — so the VM-side interfaces stay front-end-agnostic (a
//future DAP adapter or the IDE reuses them untouched).
//
//Lifecycle: main() constructs the session, wires it into VmExecutor
//via SetDebugHooks, then calls Execute. All interaction happens
//inside OnStatement/OnThrow callbacks (the program is frozen there);
//the callback returns to resume. The session starts in InitialStop
//mode, so the first OP_DebugInfo opens the command loop (gdb
//`start` behavior).
class DebugSession : public IDebugHooks {
public:
    //in/out injectable so unit tests drive the loop with string
    //streams (cin/cout in the ndb tool).
    DebugSession(const CompiledModule& module, std::string modulePath,
        std::istream& in, std::ostream& out);
    ~DebugSession() override;

    void OnStatement(const DebugStopInfo& stop, IVmDebugView& view) override;
    void OnThrow(const DebugStopInfo& stop, IVmDebugView& view) override;

private:
    enum class RunMode {
        InitialStop,   //stop at the first statement
        Continue,
        StepInto,      //s: next statement, any depth
        StepOver,      //n: next statement with depth <= recorded
        StepOut,       //f: next statement with depth <  recorded
    };

    struct Breakpoint {
        int id = 0;
        uint16_t funcIdx = 0;
        uint16_t pc = 0;
        std::string label;   //"main (file.n:9)" for reports
        int hits = 0;
    };

    //Command dispatch; returns true for resume commands (c/s/n/f).
    bool RunCommand(const std::string& cmd);
    void RunCommandLoop();
    void ReportStop(const std::string& prefix, const DebugStopInfo& stop);
    void DoBreak(const std::string& arg);
    void DoInfoBreakpoints();
    void DoDelete(const std::string& arg);
    void DoBacktrace();
    void DoFrame(const std::string& arg);
    void DoInfoLocals();
    void DoPrint(const std::string& arg);
    void DoHelp();
    [[noreturn]] void Quit();

    const CompiledModule& m_module;
    std::string m_modulePath;   //.nmod location (source search base, Task 5)
    std::istream& m_in;
    std::ostream& m_out;
    IVmDebugView* m_pView = nullptr;  //valid only inside a callback
    RunMode m_mode = RunMode::InitialStop;
    size_t m_stepDepth = 0;     //depth captured when s/n/f was issued
    size_t m_selectedFrame = 0;
    int m_nextBreakpointId = 1;
    std::vector<Breakpoint> m_breakpoints;
};

} // namespace nlang
