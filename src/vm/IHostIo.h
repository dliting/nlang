/*---
IHostIo.h — host I/O seam for embedding debuggers/tools. Output bytes
arrive verbatim (what stdout would have received). Input is opt-in and
tri-state: ReadInputLine answers Line (a whole program-input line),
Eof (input exhausted) or NoChannel (no input facility at all — the
base-interface default, which makes io's input reads raise a catchable
IOException instead of silently consuming the embedder's stream). No
host installed (the default) keeps the executor's stdout/stdin
behavior, so nvm and the CLI front ends are unaffected.
Callbacks fire on the execution thread; the executor is single-threaded
today (contract describes the present fact, not a threading guarantee).
---*/
#pragma once
#include <string>
#include <string_view>

namespace nlang {

//Outcome of a host input read. Eof and NoChannel are terminal — the
//executor latches them (HostIoLineSource), so later probes report
//"no input" without re-asking the host.
enum class HostInputStatus { Line, Eof, NoChannel };

class IHostIo {
public:
    virtual ~IHostIo() = default;
    //Must not throw: it runs on the execution thread, and an escaping
    //exception would surface inside the executor.
    virtual void OnOutput(std::string_view text) = 0;
    //Extension point ④: the program's separate error channel. The
    //default forwards to OnOutput — hosts with one merged output view
    //(nide's terminal) keep their behavior; the embedding adapter
    //overrides to split the streams. Must not throw (same rule).
    virtual void OnError(std::string_view text) { OnOutput(text); }
    //The program's input channel while it is parked in an input read —
    //blocking is allowed. Fill `line` (newline already stripped) and
    //answer Line; Eof = input exhausted; NoChannel = this host supplies
    //no input. Must not throw (same thread rule).
    virtual HostInputStatus ReadInputLine(std::string& line) {
        return HostInputStatus::NoChannel;
    }
    //Non-blocking input probe behind io.hasInput: true = a
    //ReadInputLine would supply a line right now. Hosts without a
    //probe keep the default true ("a channel exists"); the first
    //Eof/NoChannel latches the executor-side answer to false.
    //Must not throw (same thread rule).
    virtual bool HasInputLine() { return true; }
};

} // namespace nlang
