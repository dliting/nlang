/*---
LineSource.h — abstract blocking line source for io's input side.
TokenView (the readToken/readChar state machine) sits on this interface
so the console/file path (InputLineSource) and the embedded-host path
(HostIoLineSource, appended once IHostIo grows its tri-state input
seam) share one slicing implementation.
---*/
#pragma once
#include <string>

namespace nlang {

//Outcome of a line pull. NoChannel is distinct from Eof: a demanding
//read raises for both, but readLine treats Eof as the normal ""-sentinel
//end while NoChannel stays an error.
enum class InputReadStatus { Ok, Eof, NoChannel };

//One sticky rule: after HasMore() answers false (or PullLine reports
//Eof/NoChannel) the source must never produce a line again.
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

} // namespace nlang
