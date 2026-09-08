/*---
IHostIo.h — host I/O seam for embedding debuggers/tools. Output bytes
arrive verbatim (what stdout would have received). Input is opt-in: an
installed host that declares no input makes readLine fail loudly
instead of silently consuming the embedder's stream (machine mode keeps
its stdin as the protocol channel). No host installed (the default)
keeps the executor's stdout/stdin behavior, so nvm and the CLI front
ends are unaffected.
Callbacks fire on the execution thread; the executor is single-threaded
today (contract describes the present fact, not a threading guarantee).
---*/
#pragma once
#include <string_view>

namespace nlang
{

class IHostIo
{
public:
    virtual ~IHostIo() = default;
    virtual void OnOutput(std::string_view text) = 0;
    virtual bool IsInputAvailable() const { return false; }
};

} //namespace nlang
