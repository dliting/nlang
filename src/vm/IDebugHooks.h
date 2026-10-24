#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Checkpoint payload. pc/line anchor the statement; funcIdx indexes
//CompiledModule::functions; depth is the NLang call-stack depth
//(1 = main); contextId is the scheduler context the checkpoint fired
//in (0 = main, spawned contexts count up from 1).
struct DebugStopInfo {
    int contextId = 0;
    uint16_t pc = 0;
    uint16_t line = 0;
    uint16_t funcIdx = 0;
    size_t depth = 0;
};

//One local variable of a frame, rendered for display.
struct DebugLocalValue {
    std::string name;
    std::string kindName;
    std::string display;
};

//One frame of the NLang call stack.
struct DebugFrameInfo {
    std::string funcName;
    std::string sourceFile;
    uint16_t funcIdx = 0;   //CompiledModule::functions index (ndb `x`)
    uint16_t line = 0;
    uint16_t pc = 0;
};

//One context of the run (main or spawned), for context listings.
//stateName is the scheduler state ("runnable", "parked-chan-recv",
//"finished", ...); heldByCarrier is the carrier whose execution window
//currently holds the context (-1 = none: parked, queued or finished);
//frameCount is its live frame count (0 once finished); funcName/line/
//sourceFile locate the innermost frame for display (empty name+file
//when the context is frameless).
struct DebugContextInfo {
    int id = 0;
    std::string stateName;
    int heldByCarrier = -1;
    size_t frameCount = 0;
    std::string funcName;
    uint16_t line = 0;
    std::string sourceFile;
};

//Read-only view over the frozen VM state. Only valid inside an
//IDebugHooks callback (the program is suspended there). Frame queries
//address one context by id: the running context reads the executor's
//call stack, every other context reads its parked frames in the
//scheduler registry — the same query path covers both. Implementations
//must not execute NLang code or allocate on the NLang heap — the heap
//is consistent at freeze time and must stay that way.
//K7: checkpoints run GIL-free on concurrent carriers, so queries may
//arrive from the frozen window's pump while other carriers execute —
//implementations self-guard on the global lock.
class GilHolder;   //the scheduler's global-lock holder interface

class IVmDebugView {
public:
    virtual ~IVmDebugView() = default;
    virtual size_t ContextCount() const = 0;
    virtual DebugContextInfo ContextInfo(int contextId) const = 0;
    virtual size_t FrameCount(int contextId) const = 0;  //depth 0 = innermost
    virtual DebugFrameInfo FrameInfo(int contextId, size_t depth) const = 0;
    virtual std::vector<DebugLocalValue> FrameLocals(int contextId,
        size_t depth) const = 0;
    //K7: the freeze parking bracket, driven by the session controller
    //around the frozen window. Enter: leave the GC stop-the-world
    //denominator (arriving under any in-flight stop) and hand off the
    //global lock this checkpoint may hold — nested-window freezes (a
    //breakpoint inside a toString override) run inside a store guard,
    //top-level checkpoints hold nothing; parkedHolder receives the
    //handed-off holder, nullptr when nothing was held. Exit: re-enter
    //the denominator, re-acquire the handed-off lock, then drop this
    //window's count on the dispatch gate — the last frozen carrier out
    //opens the gate. The parkedHolder out/in pair lives in the
    //controller's Freeze frame so nested parks (a breakpoint inside a
    //pump-driven evaluation) each restore their own holder. Default
    //no-ops keep other view implementations source-compatible.
    virtual void EnterFreezePark(GilHolder*& parkedHolder) {
        parkedHolder = nullptr;
    }
    virtual void ExitFreezePark(GilHolder* parkedHolder) {
        (void)parkedHolder;
    }
};

//Front-end-agnostic debugger callbacks (no CLI/terminal concepts — a
//future DAP adapter or the IDE reuses the same engine layer).
//Contract: the callback runs with the program frozen; not returning
//keeps it frozen. Implementations must not let C++ exceptions escape
//into the VM (they would cross the NLang try/catch boundary).
class IDebugHooks {
public:
    virtual ~IDebugHooks() = default;
    virtual void OnStatement(const DebugStopInfo&, IVmDebugView&) = 0;
    virtual void OnThrow(const DebugStopInfo&, IVmDebugView&) = 0;
    //Desired exclusive-dispatch focus (-1 = none). Read by the executor
    //after every checkpoint: while the focus context is runnable the
    //scheduler dispatches it alone (step windows stay deterministic);
    //the default serves hooks that never step.
    virtual int DebugFocusContext() const { return -1; }
};

} // namespace nlang
