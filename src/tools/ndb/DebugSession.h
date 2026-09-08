#pragma once
#include "DebugSessionController.h"
#include "SourceCache.h"
#include "nlang/vm/CompiledModule.h"
#include <iosfwd>
#include <string>

namespace nlang {

//CLI front end over DebugSessionController: command parsing, text
//formatting and the source cache. All session state (breakpoints, step
//mode, frozen-window view) lives in the controller; this adapter only
//translates between terminal lines and controller calls, byte-identical
//to the pre-extraction ndb (the dbg_* e2e set pins the formats).
//
//Lifecycle: main() constructs the session and the controller, wires the
//controller into VmExecutor via SetDebugHooks, then calls Execute. The
//controller freezes the program at the first statement (gdb `start`
//behavior), calls OnStopped and blocks in WaitUntilResume — which runs
//the interactive command loop; returning from a resume command resumes
//in place.
class DebugSession : public IDebugFrontEnd {
public:
    //in/out injectable so unit tests drive the loop with string
    //streams (cin/cout in the ndb tool).
    DebugSession(const CompiledModule& module, std::string modulePath,
        std::istream& in, std::ostream& out);
    ~DebugSession() override;

    //Wiring: session and controller reference each other, so main/tests
    //attach the controller after constructing both. Must be set before
    //the first stop.
    void SetController(DebugSessionController* controller)
        { m_pController = controller; }

    //IDebugFrontEnd. OnExited/OnRuntimeError are no-ops: main() prints
    //the exit code / runtime error itself (same bytes as before the
    //controller extraction).
    void OnStopped(const StopInfo& stop) override;
    void OnExited(int code) override;
    void OnRuntimeError(const std::string& backtrace) override;
    void WaitUntilResume() override;

private:
    //Command dispatch; returns true for resume commands (c/s/n/f).
    bool RunCommand(const std::string& cmd);
    void ReportStop(const std::string& prefix);
    void ReportBreakpoint(int id);
    void DoBreak(const std::string& arg);
    void DoInfoBreakpoints();
    void DoDelete(const std::string& arg);
    void DoBacktrace();
    void DoFrame(const std::string& arg);
    void DoInfoLocals();
    void DoPrint(const std::string& arg);
    void DoList(const std::string& arg);
    void DoDisassemble();
    void DoCatch(const std::string& arg);
    void DoHelp();
    [[noreturn]] void Quit();

    const CompiledModule& m_module;   //`x` disassembly + name lookup
    std::string m_modulePath;   //.nmod location (source search base)
    std::istream& m_in;
    std::ostream& m_out;
    DebugSessionController* m_pController = nullptr;  //set via SetController
    size_t m_selectedFrame = 0;   //`frame <n>` selection (display concern)
    SourceCache m_sourceCache;   //`l` source resolution + caching
};

} // namespace nlang
