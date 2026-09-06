#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Checkpoint payload. pc/line anchor the statement; funcIdx indexes
//CompiledModule::functions; depth is the NLang call-stack depth
//(1 = main).
struct DebugStopInfo {
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

//Read-only view over the frozen VM state. Only valid inside an
//IDebugHooks callback (the program is suspended there). Implementations
//must not execute NLang code or allocate on the NLang heap — the heap
//is consistent at freeze time and must stay that way.
class IVmDebugView {
public:
    virtual ~IVmDebugView() = default;
    virtual size_t FrameCount() const = 0;  //depth 0 = innermost
    virtual DebugFrameInfo FrameInfo(size_t depth) const = 0;
    virtual std::vector<DebugLocalValue> FrameLocals(size_t depth) const = 0;
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
};

} // namespace nlang
