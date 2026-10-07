/*---
InputLineSource.cpp — byte-stream line assembly plus the platform
probe seam (see the header for the read mechanism rationale).
---*/
#include "InputLineSource.h"

#include <cstring>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nlang {
namespace {
constexpr size_t kReadChunkBytes = 4096;
} // namespace

InputLineSource::InputLineSource(std::FILE* stream) : m_stream(stream) {}

bool InputLineSource::FillBuffer() {
    if (m_streamEof) return false;
    const size_t start = m_buffer.size();
    m_buffer.resize(start + kReadChunkBytes);
    //read()/_read answer with the bytes available RIGHT NOW (a pipe
    //delivers one line; a disk fills the whole chunk). fread is
    //unusable here: it blocks until the full request is satisfied, so
    //an interactive writer that keeps the pipe open (an IDE's stdin
    //row, a console user) would deadlock the program mid-read.
#if defined(_WIN32)
    const int got = _read(_fileno(m_stream), &m_buffer[start],
                          static_cast<unsigned int>(kReadChunkBytes));
#else
    //Retry an interrupted read: on POSIX a signal may break read()
    //before any byte arrives; treating that as end of input would
    //silently truncate the stream.
    ssize_t got;
    do {
        got = ::read(fileno(m_stream), &m_buffer[start], kReadChunkBytes);
    } while (got < 0 && errno == EINTR);
#endif
    if (got <= 0) {
        m_buffer.resize(start);
        m_streamEof = true;   //EOF and error both: no more bytes ever
        return false;
    }
    m_buffer.resize(start + static_cast<size_t>(got));
    return true;
}

InputReadStatus InputLineSource::PullLine(std::string& line) {
    for (;;) {
        const char* base = m_buffer.data() + m_bufferPos;
        const size_t avail = m_buffer.size() - m_bufferPos;
        const void* nl = std::memchr(base, '\n', avail);
        if (nl != nullptr) {
            const size_t len = static_cast<size_t>(
                static_cast<const char*>(nl) - base);
            line.assign(base, len);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_bufferPos += len + 1;
            return InputReadStatus::Ok;
        }
        //No newline yet: drop the consumed prefix and read more.
        m_buffer.erase(0, m_bufferPos);
        m_bufferPos = 0;
        //About to read from the stream. Two probe facts decide: it may
        //latch EOF (a disk read-ahead that comes up empty) and it may
        //read data in (a disk read-ahead that succeeds). Cue only when
        //the coming read may BLOCK; when the probe buffered bytes
        //instead, re-scan — the newline may already be here and the
        //blocking FillBuffer below would misread the buffer as a
        //trailing partial line.
        if (m_onInputWait) {
            if (!HasMore() && !m_streamEof)
                m_onInputWait();
            if (!m_buffer.empty()
                && std::memchr(m_buffer.data(), '\n', m_buffer.size())
                    != nullptr)
                continue;   //slice the probed-in line at the top
        }
        if (!FillBuffer()) {
            if (!m_buffer.empty()) {
                //Trailing partial line: "abc<EOF>" yields "abc" once
                //(fgets / Java BufferedReader.readLine convention).
                line = std::move(m_buffer);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                m_buffer.clear();
                return InputReadStatus::Ok;
            }
            return InputReadStatus::Eof;
        }
    }
}

bool InputLineSource::HasMore() {
    if (m_bufferPos < m_buffer.size()) return true;
    if (m_streamEof) return false;
    return StreamHasMore();
}

bool InputLineSource::StreamHasMore() {
#if defined(_WIN32)
    const HANDLE handle = reinterpret_cast<HANDLE>(
        _get_osfhandle(_fileno(m_stream)));
    if (handle == INVALID_HANDLE_VALUE) return false;
    const DWORD fileType = GetFileType(handle);
    if (fileType == FILE_TYPE_DISK)
        //Read-ahead is a legal disk probe (disks answer immediately).
        //Position-vs-size arithmetic is deliberately NOT used: CRLF
        //text-mode translation makes the two disagree, and a
        //position<size that stays true at EOF deadlocks hasInput loops.
        return FillBuffer();
    if (fileType == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available,
                           nullptr)) {
            //Broken pipe = no more bytes ever. Latch it here (not just
            //answer false) so an already-EOF pipe never trips the
            //wait-cue: a read that cannot block must stay silent.
            m_streamEof = true;
            return false;
        }
        return available > 0;
    }
    return false;   //console/char device: only our buffer is visible
#else
    struct stat st;
    if (fstat(fileno(m_stream), &st) != 0) return false;
    if (S_ISREG(st.st_mode)) return FillBuffer();
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
        struct pollfd pfd;
        pfd.fd = fileno(m_stream);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
            return false;
        //POLLIN also fires at pipe EOF (read answers 0): resolve the
        //ambiguity by actually reading — bytes land in our buffer, EOF
        //latches m_streamEof and answers false here.
        return FillBuffer();
    }
    return false;   //tty: only our buffer is visible
#endif
}

} // namespace nlang
