/*---
InputLineSource.cpp — byte-stream line assembly plus the platform
probe seam (see the header for the buffering rationale).
---*/
#include "InputLineSource.h"

#include <cstring>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nlang {
namespace {
constexpr size_t kReadChunkBytes = 4096;
} // namespace

InputLineSource::InputLineSource(std::FILE* stream) : m_stream(stream) {
    std::setvbuf(m_stream, nullptr, _IONBF, 0);
}

bool InputLineSource::FillBuffer() {
    if (m_streamEof) return false;
    const size_t start = m_buffer.size();
    m_buffer.resize(start + kReadChunkBytes);
    const size_t got = std::fread(&m_buffer[start], 1, kReadChunkBytes,
                                  m_stream);
    m_buffer.resize(start + got);
    if (got == 0) {
        m_streamEof = true;
        return false;
    }
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
                           nullptr))
            return false;   //broken pipe = no more bytes ever
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
