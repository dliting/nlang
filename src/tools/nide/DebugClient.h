/*--- DebugClient.h - session client for a real `ndb --machine` child ---*/
#ifndef NLANG_TOOLS_NIDE_DEBUG_CLIENT_H
#define NLANG_TOOLS_NIDE_DEBUG_CLIENT_H

#include "DebugProtocolCodec.h"
#include <QObject>
#include <QProcess>
#include <QString>
#include <memory>

namespace nlang {

//One debug session against ndb --machine (the wire contract lives on
//MachineFrontEnd.h). Owns the QProcess child: stdout event lines are
//parsed by DebugProtocolCodec and re-emitted as signals, stdin carries
//the commands. One instance drives ONE session -- launch rejects when
//not Idle; a fresh session means a fresh client.
//State machine:
//  Idle --launch--> Launching --run sent (hello + bp receipts done)-->
//  Running --stopped (breakpoint/step/throw)--> Stopped --c/s/n/f-->
//  Running; Running --exited/error--> Ended; stop() kills and finished()
//  converges any state to Ended.
//The protocol's `initial` stop (main's first statement, fired right
//after run) is a program-start artifact, not a user-facing stop: the
//client consumes it with an automatic continue and keeps state Running.
class DebugClient : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Launching, Running, Stopped, Ended };
    Q_ENUM(State)

    //ndbPath: absolute path of the ndb executable (MainWindow passes its
    //own tool directory's copy; tests pass the build tree's). No path
    //lookup happens inside the library.
    explicit DebugClient(const QString& ndbPath,
        QObject* parent = nullptr);
    //A session still live here is terminated and reaped, so no orphaned
    //ndb outlives the client (short bounded wait -- kill() is instant).
    ~DebugClient() override;

    State state() const { return m_state; }

    //Start `ndb --machine <modulePath>`. Only valid from Idle.
    bool launch(const QString& modulePath);
    //Prelude command (also accepted while Stopped): queued until hello,
    //then sent; the bound/unbound receipt arrives as breakpointBound.
    bool addBreakpoint(const QString& file, int line);
    //End the prelude. Deferred until hello AND every outstanding bp
    //receipt arrived, so launch/addBreakpoint/run can be issued back to
    //back without a handshake race.
    bool run();
    //Frozen-window commands: resumes return to Running, inspection
    //commands keep Stopped. requestLocals sends `frame <i>` + `locals`
    //as one self-contained pair (the selection is server-side state
    //that resets on every stop).
    bool continueRun();
    bool stepInto();
    bool stepOver();
    bool stepOut();
    bool requestBacktrace();
    bool requestLocals(int frameIndex);
    //Terminate unconditionally: kill(), no graceful handshake (an
    //infinite loop must stay terminable); finished() is the one
    //convergence point. No-op when not live.
    void stop();

signals:
    void helloReceived(int protocolVersion);
    void breakpointBound(int id, const QString& file, int line,
        bool bound);
    //reason: initial|breakpoint|step|throw. depth is 1-based and equals
    //frameCount; frame indexes (bt/requestLocals) are 0-based, innermost
    //frame = 0.
    void stopped(const QString& reason, int breakpointId,
        const QString& funcName, const QString& file, int line,
        int depth, int frameCount);
    void frameReceived(int frameIndex, const QString& funcName,
        const QString& file, int line);
    void localReceived(const QString& name, const QString& typeName,
        const QString& value);
    void outputReceived(const QString& text);
    //done-terminated multi-line responses: bt = frameCount frame lines
    //then backtraceReady(); locals = the local lines then localsReady().
    void backtraceReady();
    void localsReady();
    void exited(int exitCode);
    //Uncaught NLang exception or a failed module load (may precede
    //hello!): the session's diagnostic end -- the ndb process then
    //exits 1 without an exited event.
    void errorReceived(const QString& report);
    //err events: per-command protocol failures (malformed request,
    //window violation). Transient -- the session survives.
    void commandFailed(const QString& message);
    //The ndb process ended without a session-end event (killed by
    //stop(), crashed, or stdin-EOF quit): one diagnostic, composed with
    //whatever the child wrote to stderr.
    void abnormallyExited(const QString& diagnostic);
    //QProcess could not start (bad ndbPath): the session never begins.
    void failedToLaunch(const QString& error);

private slots:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    void handleEvent(const DebugEvent& event);
    void maybeSendRun();
    void writeLine(const QString& line);
    void convergeEnded(const QString& why);

    std::unique_ptr<QProcess> m_upProcess;
    QString m_stderrLog;          //surfaced with the abnormal-exit note
    QByteArray m_pendingLine;     //partial stdout line between newlines
    State m_state = State::Idle;
    int m_pendingBpReceipts = 0;  //bp receipts still owed by the prelude
    bool m_helloSeen = false;
    bool m_runRequested = false;
    bool m_runSent = false;
    bool m_stopRequested = false;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_DEBUG_CLIENT_H
