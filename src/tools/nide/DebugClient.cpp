// --- DebugClient: drives one ndb --machine child over stdin/stdout ---
// Event lines are split on the wire newline (a partial trailing line
// stays buffered until its \n arrives); commands are written unescaped.
// The child's stdout is the protocol channel, so stderr is only a
// diagnostics buffer that rides along with abnormal-exit reports.

#include "DebugClient.h"

#include <QByteArray>

namespace nlang {

namespace {
//Destructor reap ceiling: kill() is TerminateProcess, so a healthy ndb
//is gone in milliseconds; the wait only covers scheduler noise.
constexpr int kDtorReapWaitMs = 1000;
} // namespace

using DebugProtocolCodec::encodeCommand;
using DebugProtocolCodec::parseEvent;

DebugClient::DebugClient(const QString& ndbPath, QObject* parent)
    : QObject(parent)
{
    m_upProcess = std::make_unique<QProcess>(this);
    m_upProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_upProcess.get(), &QProcess::readyReadStandardOutput,
        this, &DebugClient::onReadyReadStandardOutput);
    connect(m_upProcess.get(), &QProcess::readyReadStandardError,
        this, &DebugClient::onReadyReadStandardError);
    connect(m_upProcess.get(),
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, &DebugClient::onFinished);
    connect(m_upProcess.get(), &QProcess::errorOccurred,
        this, &DebugClient::onErrorOccurred);
    m_upProcess->setProgram(ndbPath);
}

DebugClient::~DebugClient()
{
    if (m_state == State::Idle)
        return;
    if (m_upProcess->state() == QProcess::NotRunning)
        return;
    m_upProcess->kill();
    m_upProcess->waitForFinished(kDtorReapWaitMs);
}

// --- session commands ---

void DebugClient::setWorkingDirectory(const QString& dir)
{
    m_upProcess->setWorkingDirectory(dir);
}

bool DebugClient::launch(const QString& modulePath)
{
    if (m_state != State::Idle)
        return false;
    m_state = State::Launching;
    m_upProcess->start(m_upProcess->program(),
        {QStringLiteral("--machine"), modulePath});
    return true;
}

bool DebugClient::addBreakpoint(const QString& file, int line)
{
    if (m_state != State::Launching && m_state != State::Stopped)
        return false;
    if (m_state == State::Launching)
        ++m_pendingBpReceipts;   //a receipt still owed before run
    writeLine(encodeCommand(QStringLiteral("b"),
        {file, QString::number(line)}));
    return true;
}

bool DebugClient::run()
{
    if (m_state != State::Launching)
        return false;
    m_runRequested = true;
    maybeSendRun();
    return true;
}

bool DebugClient::continueRun()
{
    if (m_state != State::Stopped)
        return false;
    m_state = State::Running;
    writeLine(QStringLiteral("c"));
    return true;
}

bool DebugClient::stepInto()
{
    if (m_state != State::Stopped)
        return false;
    m_state = State::Running;
    writeLine(QStringLiteral("s"));
    return true;
}

bool DebugClient::stepOver()
{
    if (m_state != State::Stopped)
        return false;
    m_state = State::Running;
    writeLine(QStringLiteral("n"));
    return true;
}

bool DebugClient::stepOut()
{
    if (m_state != State::Stopped)
        return false;
    m_state = State::Running;
    writeLine(QStringLiteral("f"));
    return true;
}

bool DebugClient::requestBacktrace()
{
    if (m_state != State::Stopped)
        return false;
    writeLine(QStringLiteral("bt"));
    return true;
}

bool DebugClient::requestLocals(int frameIndex)
{
    if (m_state != State::Stopped)
        return false;
    writeLine(encodeCommand(QStringLiteral("frame"),
        {QString::number(frameIndex)}));
    writeLine(QStringLiteral("locals"));
    return true;
}

bool DebugClient::setBreakOnThrow(bool enabled)
{
    if (m_state != State::Launching && m_state != State::Stopped)
        return false;
    writeLine(encodeCommand(QStringLiteral("breakthrow"),
        {enabled ? QStringLiteral("on") : QStringLiteral("off")}));
    return true;
}

bool DebugClient::deleteBreakpoint(int id)
{
    if (m_state != State::Launching && m_state != State::Stopped)
        return false;
    writeLine(encodeCommand(QStringLiteral("d"),
        {QString::number(id)}));
    return true;
}

void DebugClient::stop()
{
    if (m_state == State::Idle || m_state == State::Ended)
        return;
    m_stopRequested = true;
    m_upProcess->kill();
}

// --- stdout event pump ---

void DebugClient::onReadyReadStandardOutput()
{
    m_pendingLine += m_upProcess->readAllStandardOutput();
    for (int nl = m_pendingLine.indexOf('\n'); nl >= 0;
            nl = m_pendingLine.indexOf('\n')) {
        const QByteArray raw = m_pendingLine.left(nl);
        m_pendingLine.remove(0, nl + 1);
        handleEvent(parseEvent(QString::fromUtf8(raw)));
    }
}

void DebugClient::handleEvent(const DebugEvent& ev)
{
    const QStringList& f = ev.fields;
    switch (ev.kind) {
    case DebugEvent::Hello:
        m_helloSeen = true;
        emit helloReceived(f.value(1).toInt());
        maybeSendRun();
        break;
    case DebugEvent::Bp:
        if (m_pendingBpReceipts > 0)
            --m_pendingBpReceipts;
        emit breakpointBound(f.value(1).toInt(), f.value(2),
            f.value(3).toInt(), f.value(4) == QLatin1String("bound"));
        maybeSendRun();
        break;
    case DebugEvent::Stopped:
        if (f.value(1) == QLatin1String("initial")) {
            //Program-start artifact of `run`, not a user-facing stop:
            //consume it with an automatic continue.
            m_state = State::Running;
            writeLine(QStringLiteral("c"));
            break;
        }
        m_state = State::Stopped;
        emit stopped(f.value(1), f.value(2).toInt(), f.value(3),
            f.value(4), f.value(5).toInt(), f.value(6).toInt(),
            f.value(7).toInt());
        break;
    case DebugEvent::Frame:
        emit frameReceived(f.value(1).toInt(), f.value(2), f.value(3),
            f.value(4).toInt());
        break;
    case DebugEvent::Local:
        emit localReceived(f.value(1), f.value(2), f.value(3));
        break;
    case DebugEvent::Done:
        if (f.value(1) == QLatin1String("bt"))
            emit backtraceReady();
        else if (f.value(1) == QLatin1String("locals"))
            emit localsReady();
        break;
    case DebugEvent::Output:
        emit outputReceived(f.value(1));
        break;
    case DebugEvent::Exited:
        m_state = State::Ended;
        emit exited(f.value(1).toInt());
        break;
    case DebugEvent::Error:
        m_state = State::Ended;
        emit errorReceived(f.value(1));
        break;
    case DebugEvent::Err:
        emit commandFailed(f.value(1));
        break;
    case DebugEvent::Unknown:
        break;   //forward compatibility: unknown events stay silent
    }
}

// --- stdin writes ---

void DebugClient::maybeSendRun()
{
    //Both prelude gates: the wire is up (hello) and every requested
    //breakpoint got its receipt -- then the user's run() goes out,
    //exactly once (a bp receipt on a later, stopped-window breakpoint
    //must not re-fire it).
    if (!m_runSent && m_helloSeen && m_runRequested
        && m_pendingBpReceipts <= 0) {
        m_runSent = true;
        m_state = State::Running;
        writeLine(QStringLiteral("run"));
    }
}

void DebugClient::writeLine(const QString& line)
{
    m_upProcess->write((line + QLatin1Char('\n')).toUtf8());
}

// --- stderr / lifetime convergence ---

void DebugClient::onReadyReadStandardError()
{
    m_stderrLog += QString::fromUtf8(m_upProcess->readAllStandardError());
}

void DebugClient::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_state == State::Ended)
        return;   //exited/error already converged the session
    QString why;
    if (m_stopRequested)
        why = QStringLiteral("ndb terminated by stop()");
    else if (exitStatus == QProcess::CrashExit)
        why = QStringLiteral("ndb crashed (exit code %1)").arg(exitCode);
    else
        why = QStringLiteral("ndb exited before the session ended "
              "(exit code %1)").arg(exitCode);
    convergeEnded(why);
}

void DebugClient::onErrorOccurred(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart)
        return;   //crash/IO errors converge through finished()
    m_state = State::Ended;
    emit failedToLaunch(QStringLiteral(
        "could not start ndb: %1").arg(m_upProcess->program()));
}

void DebugClient::convergeEnded(const QString& why)
{
    m_state = State::Ended;
    if (m_stderrLog.trimmed().isEmpty())
        emit abnormallyExited(why);
    else
        emit abnormallyExited(
            QStringLiteral("%1\n%2").arg(why, m_stderrLog.trimmed()));
}

} // namespace nlang
