/*---
InputLineSource.h — LineSource over a FILE* byte stream (stdin by
default), the portable core of io's input side. The stream is switched
to unbuffered mode on construction: every readable byte must land in
OUR buffer or no probe could see it (CRT-internal buffering answers no
portably-askable question). Platform-specific code is confined to
StreamHasMore's pipe probe; everything else is plain fread/memchr.
---*/
#pragma once
#include <cstdio>
#include <string>

#include "LineSource.h"

namespace nlang {

class InputLineSource : public LineSource {
public:
    //Takes the stream unbuffered — call before any I/O on it. In the
    //executor the first input native constructs this lazily, which is
    //stdin's first touch. A setvbuf failure (already-buffered stream)
    //is tolerated: probing degrades, reading still works.
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
