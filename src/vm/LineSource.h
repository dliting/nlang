/*---
LineSource.h — abstract blocking line source plus the host-IO adapter.
TokenView (the readToken/readChar state machine) sits on this interface
so the console/file path (InputLineSource) and the embedded-host path
(HostIoLineSource) share one slicing implementation.
---*/
#pragma once
#include <string>

#include "IHostIo.h"

namespace nlang {

//Outcome of a line pull. NoChannel is distinct from Eof: a demanding
//read raises for both, but readLine treats Eof as the normal ""-sentinel
//end while NoChannel stays an error.
enum class InputReadStatus { Ok, Eof, NoChannel };

//One sticky rule: after PullLine reports Eof/NoChannel the source must
//never produce a line again. A false HasMore() is a "nothing buffered
//now" answer — on a console, a later blocking pull may still deliver
//newly typed input.
class LineSource {
public:
    virtual ~LineSource() = default;
    //Blocking pull of the next line, newline and a trailing '\r'
    //already stripped. A trailing partial line ("abc<EOF>") yields
    //"abc" once, then Eof.
    virtual InputReadStatus PullLine(std::string& line) = 0;
    //Non-blocking probe: true when a line is available right now
    //(already-buffered content counts). Exactness is source-dependent —
    //see InputLineSource and HostIoLineSource.
    virtual bool HasMore() = 0;
};

//Adapter over the embedding host's input seam. Latches terminal
//states: IHostIo::HasInputLine's default (true) must not keep promising
//input after the host itself reported Eof/NoChannel, or a hasInput-
//driven loop would never exit.
class HostIoLineSource : public LineSource {
public:
    explicit HostIoLineSource(IHostIo& host) : m_host(host) {}

    InputReadStatus PullLine(std::string& line) override {
        if (m_terminal != InputReadStatus::Ok) return m_terminal;
        switch (m_host.ReadInputLine(line)) {
        case HostInputStatus::Line:
            //Hosts may hand "\r\n"-shaped lines; normalize here so both
            //line sources answer the byte-identical contract.
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return InputReadStatus::Ok;
        case HostInputStatus::Eof:
            return m_terminal = InputReadStatus::Eof;
        case HostInputStatus::NoChannel:
            return m_terminal = InputReadStatus::NoChannel;
        }
        return InputReadStatus::NoChannel;   //unreachable
    }

    bool HasMore() override {
        return m_terminal == InputReadStatus::Ok && m_host.HasInputLine();
    }

private:
    IHostIo& m_host;
    InputReadStatus m_terminal = InputReadStatus::Ok;
};

} // namespace nlang
