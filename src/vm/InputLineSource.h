/*---
InputLineSource.h — LineSource over a FILE* byte stream (stdin by
default), the portable core of io's input side. Reads go straight to
the file descriptor, never through fread: fread blocks until the full
request is satisfied, which on an interactive pipe (an IDE feeding the
program one line, a console user typing) deadlocks the program mid-read
— the nide stdin-row regression. read()/_read answer with whatever
bytes are available, the line-at-a-time contract. Bypassing the FILE*
layer also leaves CRT-internal buffering untouched, so every readable
byte lands in OUR buffer and the probes stay honest. Platform-specific
code is confined to StreamHasMore; everything else is plain read/memchr.
---*/
#pragma once
#include <cstdio>
#include <string>

#include "LineSource.h"

namespace nlang {

class InputLineSource : public LineSource {
public:
    //CRT buffering is not touched: reads bypass the FILE* layer and go
    //to the descriptor directly (see the header rationale). In the
    //executor the first input native constructs this lazily, which is
    //stdin's first touch.
    explicit InputLineSource(std::FILE* stream = stdin);

    InputReadStatus PullLine(std::string& line) override;
    bool HasMore() override;

private:
    //fread one chunk into m_buffer; false = stream exhausted (EOF and
    //error both mean "no more bytes will ever come").
    bool FillBuffer();
    //Buffer-empty probe. Disk: read ahead into our buffer (a blocking
    //read is fine — disks answer immediately). Pipe: non-blocking peek
    //only. Console/char devices answer false (buffered bytes visible).
    bool StreamHasMore();

    std::FILE* m_stream;
    std::string m_buffer;      //read bytes not yet sliced into lines
    size_t m_bufferPos = 0;    //start of unconsumed data in m_buffer
    bool m_streamEof = false;  //FillBuffer already saw end of stream
};

} // namespace nlang
