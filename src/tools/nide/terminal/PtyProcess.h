/*---
PtyProcess.h — the platform seam: one child process attached to a
pseudo terminal. Windows uses ConPTY, dynamically resolved so the IDE
still loads on systems without it; the POSIX branch (openpty/fork) is
written and awaits the Linux port for verification. All non-reader
state is GUI-thread only; the reader thread crosses back via queued
signals (thread discipline, spec §7).
---*/
#pragma once
#include <memory>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

namespace nlang {
namespace terminal {

class PtyProcess : public QObject {
    Q_OBJECT

public:
    enum class StartError { None, Unsupported, SpawnFailed };
    //Unsupported = the ConPTY APIs are not resolvable (pre-1809
    //Windows); SpawnFailed = CreateProcess refused the command line.

    explicit PtyProcess(QObject* parent = nullptr);
    ~PtyProcess() override;
    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;

    bool Start(const QString& program, const QStringList& arguments,
               const QString& workingDirectory, int columns, int rows,
               StartError* error = nullptr);
    bool IsRunning() const;
    void Write(const QByteArray& bytes);   //GUI thread only
    void Resize(int columns, int rows);    //no-op while not running
    void Kill();                           //idempotent

signals:
    void OutputReady(const QByteArray& bytes);  //reader thread -> queued
    void Finished(int exitCode, bool crashed);  //exactly once

private:
    struct Impl;   //per-platform handles + reader thread
    std::unique_ptr<Impl> m_upImpl;
};

} // namespace terminal
} // namespace nlang
