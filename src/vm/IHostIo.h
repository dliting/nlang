/*---
IHostIo.h — host I/O seam for embedding debuggers/tools. Output bytes
arrive verbatim (what stdout would have received). Input is opt-in:
ReadInputLine supplies whole program-input lines through the seam (the
machine front end feeds it from stdin data commands); the base-interface
default answers no-input, which makes io.readLine raise a catchable
IOException instead of silently consuming the embedder's stream. No host
installed (the default) keeps the executor's stdout/stdin behavior, so
nvm and the CLI front ends are unaffected.
Callbacks fire on the execution thread; the executor is single-threaded
today (contract describes the present fact, not a threading guarantee).
---*/
#pragma once
#include <string>
#include <string_view>

namespace nlang {

class IHostIo {
public:
    virtual ~IHostIo() = default;
    //Must not throw: it runs on the execution thread, and an escaping
    //exception would surface inside the executor.
    virtual void OnOutput(std::string_view text) = 0;
    //The program's input channel while it is parked in io.readLine —
    //blocking is allowed. Return true and fill `line` (newline already
    //stripped) when a line is supplied; false = no input channel, and
    //the executor raises IOException. Must not throw (same thread rule).
    virtual bool ReadInputLine(std::string& line) { return false; }
};

} // namespace nlang
