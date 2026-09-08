/*---
MachineFrontEnd.h — ndb --machine: line-protocol front end for IDE
embedding. The debuggee runs in this process; the IDE drives it over
stdin/stdout, one line per event (out) and command (in). The same object
implements IHostIo: program output becomes output events, and io.readLine
stays refused (the base-interface default) because stdin is the protocol
channel.
Events (front end -> IDE) are tab-joined lines and every field is
protocol::EncodeField'd, so data tabs/newlines never break the framing —
a raw tab in the wire is always a field separator:
  hello\t1
  bp\t<id>\t<file>\t<line>\t<bound|unbound>   (id 0 = unbound)
  stopped\t<initial|breakpoint|step|throw>\t<bpId>\t<func>\t<file>
          \t<line>\t<depth>\t<frameCount>
  frame\t<n>\t<func>\t<file>\t<line>
  local\t<name>\t<type>\t<value>
  done\t<req>
  output\t<text>
  exited\t<code>
  error\t<report>
  err\t<message>
Frame numbering: <depth> in stopped is 1-based and always equals
<frameCount> (a stop freezes the innermost frame); the frame command
and bt index frames 0-based, innermost frame = 0.
Commands (IDE -> front end) are plain space-separated tokens, unescaped:
b <file> <line> (file/line split at the LAST space), bfunc <name>,
d <id>, breakthrow on|off, bt, frame <n> (also selects for locals),
locals, run (prelude only), c/s/n/f. Non-resume commands answer in place
(bp receipt, done <req> or err); a resume command answers with the next
event (the following stop or exit). `run` ends the prelude started by
PumpUntilRun; before it, window-bound commands err (no frozen window).
---*/
#pragma once
#include "DebugSessionController.h"
#include "IHostIo.h"
#include <iosfwd>
#include <string>
#include <vector>

namespace nlang {

//Protocol primitives, header-only so the unit tests pin the codec
//directly. The IDE side carries its OWN implementation of this codec —
//that duplication is a deliberate module boundary (nide links no nlang
//headers, keeping the IDE client buildable standalone). Drift is pinned
//closed by identical test vectors on both sides: when changing the wire
//format here, mirror the change in the nide codec and its tests.
namespace protocol {

//Version announced by the hello event.
inline constexpr char kProtocolVersion[] = "1";

//Escape the framing metacharacters so one field stays one line and
//tab-joined fields stay unambiguous: \ -> \\, tab -> \t, newline ->
//\n, cr -> \r. Digits and identifiers pass through unchanged.
inline std::string EncodeField(const std::string& field)
{
    std::string out;
    out.reserve(field.size());
    for (char c : field) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c; break;
        }
    }
    return out;
}

//Inverse of EncodeField. Unknown escapes pass through verbatim: the
//decoder stays total over arbitrary input.
inline std::string DecodeField(const std::string& field)
{
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] != '\\' || i + 1 == field.size()) {
            out += field[i];
            continue;
        }
        switch (field[++i]) {
        case '\\': out += '\\'; break;
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        default: out += '\\'; out += field[i]; break;
        }
    }
    return out;
}

//Assemble one event line: keyword + tab-joined encoded fields. Encoding
//is structural — callers cannot forget a field or misplace a separator.
inline std::string MakeEvent(const std::string& keyword,
    const std::vector<std::string>& fields)
{
    std::string line = keyword;
    for (const auto& field : fields) {
        line += '\t';
        line += EncodeField(field);
    }
    return line;
}

} // namespace protocol

class MachineFrontEnd : public IDebugFrontEnd, public IHostIo {
public:
    //in/out injectable so unit tests drive the protocol with string
    //streams (cin/cout in the ndb tool).
    MachineFrontEnd(const CompiledModule& module, std::istream& in,
        std::ostream& out);

    //Wiring: front end and controller reference each other, so main and
    //tests attach the controller after constructing both. Must happen
    //before PumpUntilRun.
    void SetController(DebugSessionController* controller)
        { m_pController = controller; }

    //hello + the pre-run command loop (breakpoint setup only; the
    //window-bound commands err). Returns when `run` is issued — the
    //caller then executes the program.
    void PumpUntilRun();

    //IDebugFrontEnd. OnStopped reports the freeze; WaitUntilResume is
    //the frozen-window command loop (every return follows a resume
    //command; EOF ends the process, never a bare return).
    void OnStopped(const StopInfo& stop) override;
    void OnExited(int code) override;
    void OnRuntimeError(const std::string& report) override;
    void WaitUntilResume() override;

    //IHostIo — program output becomes output events. IsInputAvailable
    //is not overridden: stdin is the protocol channel, so io.readLine
    //is refused.
    void OnOutput(std::string_view text) override;

private:
    //Command dispatch; returns true when the current pump ends (`run`
    //in the prelude, resume in the frozen window). Failures — malformed
    //requests, window violations — become err events, never exceptions
    //(this front end owns the exception boundary into the executor).
    bool Dispatch(const std::string& line);

    void DoBreakpoint(const std::string& arg);
    void DoBreakFunction(const std::string& name);
    void DoDelete(const std::string& arg);
    void DoBreakThrow(const std::string& arg);
    void DoBacktrace();
    void DoFrame(const std::string& arg);
    void DoLocals();
    void EmitFrame(size_t depth);
    //One event line, flushed immediately (Windows pipes are fully
    //buffered; a stalled line deadlocks the IDE side).
    void EmitLine(const std::string& line);
    //EOF is a deliberate session end: hard exit like the CLI's q.
    [[noreturn]] void QuitSession();

    const CompiledModule& m_module;   //bfunc's resolved bp location
    std::istream& m_in;
    std::ostream& m_out;
    DebugSessionController* m_pController = nullptr;  //set via SetController
    bool m_started = false;   //run issued: the frozen window governs now
    size_t m_selectedFrame = 0;   //frame <n> selection for locals
};

} // namespace nlang
