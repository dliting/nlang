/*---
PtyProcess.cpp — the platform seam, one branch per world. Windows
spawns the child on a ConPTY (pseudo console) resolved dynamically so
pre-1809 systems fail Start with Unsupported instead of failing to
load the IDE. The POSIX branch (openpty/fork) is written to spec but
awaits the Linux port for verification.

Windows completion detection is process-handle based, not pipe-EOF
based: conhost keeps the output pipe open while the pseudo console
lives, so ReadFile never signals the child's exit — the reader is a
pure pump and the waiter thread owns the lifecycle (thread model and
handle ownership are documented at WaiterMain).
---*/
#include "PtyProcess.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <pty.h>
#endif

namespace nlang {
namespace terminal {
namespace {

constexpr int kReadChunkBytes = 4096;
constexpr int kTerminatedExitCode = 1;

#ifdef _WIN32

//The three ConPTY entry points, resolved once. Any missing (Windows
//older than 1809) makes Start report Unsupported — a static import
//would keep the whole IDE from loading there.
struct ConPtyApi {
    HRESULT (WINAPI* create)(COORD, HANDLE, HANDLE, DWORD, void**) = nullptr;
    void (WINAPI* resize)(void*, COORD) = nullptr;
    void (WINAPI* close)(void*) = nullptr;
    bool resolved = false;
};

const ConPtyApi& ConPty() {
    static const ConPtyApi api = [] {
        ConPtyApi out;
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) return out;
        out.create = reinterpret_cast<decltype(out.create)>(
            GetProcAddress(kernel32, "CreatePseudoConsole"));
        out.resize = reinterpret_cast<decltype(out.resize)>(
            GetProcAddress(kernel32, "ResizePseudoConsole"));
        out.close = reinterpret_cast<decltype(out.close)>(
            GetProcAddress(kernel32, "ClosePseudoConsole"));
        out.resolved = out.create && out.resize && out.close;
        return out;
    }();
    return api;
}

//Quotes one argument by the MSVCRT/CommandLineToArgvW convention.
//Trailing-backslash and quote-doubling edge cases beyond this are a
//documented MVP limitation (spec §3.1).
std::wstring QuoteArg(const QString& argument) {
    const std::wstring arg = argument.toStdWString();
    if (!arg.empty()
        && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const QString& program,
                              const QStringList& arguments) {
    std::wstring line = QuoteArg(program);
    for (const QString& argument : arguments) {
        line.push_back(L' ');
        line += QuoteArg(argument);
    }
    return line;
}

#endif // _WIN32

} // namespace

struct PtyProcess::Impl {
#ifdef _WIN32
    void* hPty = nullptr;          //HPCON, void* keeps the header clean;
                                   //closed by the waiter (Kill fallback
                                   //only if the waiter died early)
    void* hProcess = nullptr;      //child process, GUI-thread owned
    void* hInputWrite = nullptr;   //our end of the child-stdin pipe
    void* hOutputRead = nullptr;   //reader-owned output end
#else
    int masterFd = -1;
    pid_t childPid = -1;
#endif
    std::thread reader;
    std::atomic<bool> running{false};
    std::atomic<bool> killed{false};
    //Exactly-once gate: the waiter's natural-exit path and Kill's
    //fallback path converge here; the loser stays silent.
    std::atomic<bool> finishedEmitted{false};
    bool started = false;          //GUI thread only
#ifdef _WIN32
    //Completion detector: waits on hProcess, tears the pty down to
    //unblock the reader, joins it, then emits Finished.
    std::thread waiter;
    //Serializes hPty use: the waiter closes it from its own thread
    //while the GUI thread may concurrently be inside Resize(). Held
    //across the API calls, not just the pointer read.
    std::mutex ptyMutex;
#endif

    void ReaderMain(PtyProcess* owner);
#ifdef _WIN32
    void WaiterMain(PtyProcess* owner);
    bool SpawnWindows(const QString& program, const QStringList& arguments,
                      const QString& workingDirectory, int columns, int rows,
                      StartError* error);
    bool CreatePtyPipes(int columns, int rows);
    bool LaunchChildWindows(const QString& program,
                            const QStringList& arguments,
                            const QString& workingDirectory);
#else
    bool SpawnPosix(const QString& program, const QStringList& arguments,
                    int columns, int rows, StartError* error);
#endif
};

PtyProcess::PtyProcess(QObject* parent)
    : QObject(parent), m_upImpl(std::make_unique<Impl>()) {}

PtyProcess::~PtyProcess() {
    Kill();
}

bool PtyProcess::Start(const QString& program, const QStringList& arguments,
                       const QString& workingDirectory, int columns, int rows,
                       StartError* error) {
    Kill();   //tear down any earlier session (no-op when never started)
    Impl& impl = *m_upImpl;
    //Fresh session, fresh verdict and gate: without this reset a
    //restart after a natural exit would never emit Finished again
    //(the gate stayed latched from the previous run).
    impl.killed.store(false);
    impl.finishedEmitted.store(false);
#ifdef _WIN32
    if (!impl.SpawnWindows(program, arguments, workingDirectory,
                           columns, rows, error))
        return false;
#else
    if (!impl.SpawnPosix(program, arguments, columns, rows, error))
        return false;
#endif
    impl.started = true;
    impl.running.store(true);
    //Reader first: thread construction is the happens-before edge, so
    //the waiter always observes the assigned reader member to join it.
    impl.reader = std::thread(&Impl::ReaderMain, &impl, this);
#ifdef _WIN32
    impl.waiter = std::thread(&Impl::WaiterMain, &impl, this);
#endif
    return true;
}

#ifdef _WIN32

bool PtyProcess::Impl::SpawnWindows(const QString& program,
                                    const QStringList& arguments,
                                    const QString& workingDirectory,
                                    int columns, int rows,
                                    StartError* error) {
    if (!ConPty().resolved) {
        if (error) *error = StartError::Unsupported;
        return false;
    }
    if (!CreatePtyPipes(columns, rows)
        || !LaunchChildWindows(program, arguments, workingDirectory)) {
        CloseHandle(hInputWrite); hInputWrite = nullptr;
        CloseHandle(hOutputRead); hOutputRead = nullptr;
        std::lock_guard<std::mutex> lock(ptyMutex);
        if (hPty) ConPty().close(hPty);
        hPty = nullptr;
        if (error) *error = StartError::SpawnFailed;
        return false;
    }
    if (error) *error = StartError::None;
    return true;
}

//Two plain pipes: we hold the far ends, the pseudo console holds
//the near ends. Nothing is inherited classically — the attribute
//list in LaunchChildWindows carries the console reference into the
//child, which is why bInheritHandles is FALSE and no handle is
//inheritable.
bool PtyProcess::Impl::CreatePtyPipes(int columns, int rows) {
    HANDLE inputRead = nullptr;    //pseudo console reads child stdin
    HANDLE outputWrite = nullptr;  //pseudo console writes child stdout
    HANDLE inputWrite = nullptr;   //ours: Write() destination
    HANDLE outputRead = nullptr;   //ours: the reader thread's source
    if (!CreatePipe(&inputRead, &inputWrite, nullptr, 0)
        || !CreatePipe(&outputRead, &outputWrite, nullptr, 0)) {
        if (inputRead) CloseHandle(inputRead);
        if (inputWrite) CloseHandle(inputWrite);
        if (outputRead) CloseHandle(outputRead);
        if (outputWrite) CloseHandle(outputWrite);
        return false;
    }
    const COORD size{static_cast<SHORT>(columns), static_cast<SHORT>(rows)};
    HRESULT hr = ConPty().create(size, inputRead, outputWrite, 0, &hPty);
    CloseHandle(inputRead);     //the pseudo console owns these now
    CloseHandle(outputWrite);
    if (FAILED(hr)) {
        CloseHandle(inputWrite);
        CloseHandle(outputRead);
        return false;
    }
    hInputWrite = inputWrite;
    hOutputRead = outputRead;
    return true;
}

bool PtyProcess::Impl::LaunchChildWindows(const QString& program,
                                          const QStringList& arguments,
                                          const QString& workingDirectory) {
    STARTUPINFOEXW info{};
    info.StartupInfo.cb = sizeof(info);
    //The child must have no console of its own: if it inherits the
    //caller's (or gets a freshly allocated one) the pseudo console is
    //silently ignored and the pipes stay empty. INVALID here is what
    //wires the child's std handles onto the pty instead.
    info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    info.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    info.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    info.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
    std::size_t bytesRequired = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytesRequired);
    //Pointer-sized elements keep the buffer aligned for the list.
    std::vector<void*> attributeBuffer(
        (bytesRequired + sizeof(void*) - 1) / sizeof(void*));
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0,
                                           &bytesRequired)
        || !UpdateProcThreadAttribute(
               attributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
               hPty, sizeof(hPty), nullptr, nullptr))
        return false;
    //Without this assignment the child starts with no console at all:
    //it inherits ours and the pty pipes see neither input nor EOF.
    info.lpAttributeList = attributeList;
    std::wstring commandLine = BuildCommandLine(program, arguments);
    const std::wstring directory = workingDirectory.toStdWString();
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
                        FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        directory.empty() ? nullptr : directory.c_str(),
                        &info.StartupInfo, &child)) {
        DeleteProcThreadAttributeList(attributeList);
        return false;
    }
    CloseHandle(child.hThread);
    DeleteProcThreadAttributeList(attributeList);   //safe once spawned
    hProcess = child.hProcess;
    return true;
}

#else

//POSIX branch — written to spec, not compiled or run on Windows.
bool PtyProcess::Impl::SpawnPosix(const QString& program,
                                  const QStringList& arguments,
                                  int columns, int rows,
                                  StartError* error) {
    const auto fail = [error](StartError value) {
        if (error) *error = value;
        return false;
    };
    int fdMaster = -1;
    int fdSlave = -1;
    if (openpty(&fdMaster, &fdSlave, nullptr, nullptr, nullptr) != 0)
        return fail(StartError::SpawnFailed);
    winsize window{static_cast<unsigned short>(columns),
                   static_cast<unsigned short>(rows), 0, 0};
    ioctl(fdMaster, TIOCSWINSZ, &window);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fdMaster);
        close(fdSlave);
        return fail(StartError::SpawnFailed);
    }
    if (pid == 0) {
        setsid();
        ioctl(fdSlave, TIOCSCTTY, 0);
        dup2(fdSlave, STDIN_FILENO);
        dup2(fdSlave, STDOUT_FILENO);
        dup2(fdSlave, STDERR_FILENO);
        if (fdSlave > STDERR_FILENO) close(fdSlave);
        close(fdMaster);
        const std::string utf8Program = program.toStdString();
        std::vector<std::string> utf8Arguments;
        for (const QString& argument : arguments)
            utf8Arguments.push_back(argument.toStdString());
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(utf8Program.c_str()));
        for (std::string& argument : utf8Arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);   //execvp only returns on failure
    }
    close(fdSlave);
    masterFd = fdMaster;
    childPid = pid;
    if (error) *error = StartError::None;
    return true;
}

#endif

bool PtyProcess::IsRunning() const {
    return m_upImpl->running.load();
}

void PtyProcess::Write(const QByteArray& bytes) {
#ifdef _WIN32
    Impl& impl = *m_upImpl;
    if (!impl.hInputWrite || bytes.isEmpty()) return;
    DWORD written = 0;
    WriteFile(impl.hInputWrite, bytes.constData(),
              DWORD(bytes.size()), &written, nullptr);   //best effort
#else
    if (m_upImpl->masterFd < 0 || bytes.isEmpty()) return;
    write(m_upImpl->masterFd, bytes.constData(), bytes.size());
#endif
}

void PtyProcess::Resize(int columns, int rows) {
#ifdef _WIN32
    const COORD size{static_cast<SHORT>(columns),
                     static_cast<SHORT>(rows)};
    std::lock_guard<std::mutex> lock(m_upImpl->ptyMutex);
    if (!m_upImpl->hPty) return;
    ConPty().resize(m_upImpl->hPty, size);
#else
    if (m_upImpl->masterFd < 0) return;
    winsize window{static_cast<unsigned short>(columns),
                   static_cast<unsigned short>(rows), 0, 0};
    ioctl(m_upImpl->masterFd, TIOCSWINSZ, &window);
#endif
}

void PtyProcess::Kill() {
    Impl& impl = *m_upImpl;
    if (!impl.started) return;
    impl.started = false;
    impl.killed.store(true);   //before the terminate: the waiter reads
                               //it for the killed verdict
#ifdef _WIN32
    TerminateProcess(impl.hProcess, DWORD(kTerminatedExitCode));
    //The waiter performs the whole teardown once the process dies
    //(close pty → join reader → emit); joining it first serializes
    //both the thread shutdown and the signal emission. Terminate on
    //an already-dead child just fails — harmless.
    if (impl.waiter.joinable()) impl.waiter.join();
    if (impl.hInputWrite) {
        CloseHandle(impl.hInputWrite);
        impl.hInputWrite = nullptr;
    }
    //Normally the waiter already closed and nulled the pty; a value
    //still here means the waiter died before its cleanup, and closing
    //it ourselves is also what unblocks a possibly-stuck reader.
    {
        std::lock_guard<std::mutex> lock(impl.ptyMutex);
        if (impl.hPty) {
            ConPty().close(impl.hPty);
            impl.hPty = nullptr;
        }
    }
    if (impl.reader.joinable()) impl.reader.join();
    if (!impl.finishedEmitted.exchange(true)) {
        DWORD code = 0;
        GetExitCodeProcess(impl.hProcess, &code);
        impl.running.store(false);
        emit Finished(int(code), true);
    }
    if (impl.hProcess) {
        CloseHandle(impl.hProcess);
        impl.hProcess = nullptr;
    }
#else
    kill(impl.childPid, SIGKILL);
    close(impl.masterFd);   //reader sees EOF and reaps the child
    impl.masterFd = -1;
    if (impl.reader.joinable()) impl.reader.join();
    if (!impl.finishedEmitted.exchange(true)) {
        impl.running.store(false);
        emit Finished(kTerminatedExitCode, true);
    }
#endif
}

void PtyProcess::Impl::ReaderMain(PtyProcess* owner) {
    char buffer[kReadChunkBytes];
#ifdef _WIN32
    //Pure output pump. No Finished here: conhost holds the pipe open
    //after the child exits, so a ReadFile failure only means the pty
    //was torn down (by the waiter or Kill), never that the child is
    //gone — completion is the waiter's call.
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(hOutputRead, buffer, sizeof(buffer), &got,
                      nullptr))
            break;   //broken pipe: the pseudo console is gone
        if (got == 0) break;
        emit owner->OutputReady(QByteArray(buffer, int(got)));
    }
    CloseHandle(hOutputRead);   //the reader owns this end
    hOutputRead = nullptr;
#else
    for (;;) {
        const ssize_t got = read(masterFd, buffer, sizeof(buffer));
        if (got <= 0) break;
        emit owner->OutputReady(QByteArray(buffer, int(got)));
    }
    int status = 0;
    waitpid(childPid, &status, 0);
    close(masterFd);
    masterFd = -1;
    if (!finishedEmitted.exchange(true)) {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                           : 128 + WTERMSIG(status);
        running.store(false);
        emit owner->Finished(code, killed.load() || !WIFEXITED(status));
    }
#endif
}

#ifdef _WIN32
void PtyProcess::Impl::WaiterMain(PtyProcess* owner) {
    //Completion detection by process handle, not pipe EOF (see the
    //file header). The sequence is the whole lifecycle contract:
    //wait for the child, read its exit code, tear the pseudo console
    //down (the reader's ReadFile fails and it exits), join the reader
    //so every OutputReady is queued before Finished (queued-connection
    //FIFO then guarantees the GUI sees all output first), emit once.
    WaitForSingleObject(hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(hProcess, &code);
    {
        std::lock_guard<std::mutex> lock(ptyMutex);
        if (hPty) {
            ConPty().close(hPty);   //unblocks the reader
            hPty = nullptr;
        }
    }
    if (reader.joinable()) reader.join();
    if (!finishedEmitted.exchange(true)) {
        running.store(false);
        emit owner->Finished(int(code),
                             killed.load() || code >= 0x80000000u);
    }
}
#endif

} // namespace terminal
} // namespace nlang
