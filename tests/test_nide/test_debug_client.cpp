/*--- test_debug_client.cpp - DebugClient session tests ---
Real tools only, no mocks (mirrors test_mainwindow's real-tools
pattern): a real ncc compiles the fixture .n sources, a real
`ndb --machine` child runs each session. The exe paths arrive as
compile definitions (CMake generator expressions, forward slashes). ---*/
#include "DebugClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using nlang::DebugClient;

namespace {

constexpr int kSessionTimeoutMs = 30000;
constexpr int kKillTimeoutMs = 5000;
constexpr int kToolTimeoutMs = 60000;

//prog.n: prints, then computes 42. The line-7 breakpoint freezes the
//session before `total = total + i * 6` executes.
const char kProgSource[] =
    "import io;\n"                 //1
    "\n"                           //2
    "int main() {\n"               //3
    "    int total = 0;\n"         //4
    "    int i = 7;\n"             //5
    "    io.print(\"hi\");\n"      //6
    "    total = total + i * 6;\n" //7
    "    return total;\n"          //8
    "}\n";                         //9

//spin.n: never terminates, so only kill() can end the session.
const char kSpinSource[] =
    "int main() {\n"          //1
    "    int i = 0;\n"        //2
    "    while (1) {\n"       //3
    "        i = i + 1;\n"    //4
    "    }\n"                 //5
    "    return i;\n"         //6
    "}\n";                    //7

//throw.n: an uncaught NLang exception ends the session via the error
//event (ndb exits 1, no exited event).
const char kThrowSource[] =
    "int main() {\n"                       //1
    "    throw new Exception(\"boom\");\n" //2
    "}\n";                                 //3

} // namespace

class TestDebugClient : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void fullSessionRunsToTheExitCode();
    void breakpointAddedWhileStoppedHitsLater();
    void killGuaranteeOnAnInfiniteLoop();
    void loadFailureReportsErrorBeforeHello();
    void uncaughtThrowEndsWithTheErrorEvent();
    void stateGuardsAndLaunchRejection();
    void failedToLaunchWhenNdbIsMissing();

private:
    //Void-on-purpose: QVERIFY/QFAIL expand to `return;`, so helpers
    //using them must not return a value -- the product lands in
    //*modPathOut.
    bool buildModule(const char* tag, const char* source,
        QString* modPathOut) const;

    QTemporaryDir m_dir;
    QString m_progNmod;
    QString m_progSource;
    QString m_spinNmod;
    QString m_throwNmod;
};

void TestDebugClient::initTestCase() {
    QVERIFY(m_dir.isValid());
    m_progSource = m_dir.filePath("prog.n");
    QVERIFY(buildModule("prog", kProgSource, &m_progNmod));
    QVERIFY(buildModule("spin", kSpinSource, &m_spinNmod));
    QVERIFY(buildModule("throw", kThrowSource, &m_throwNmod));
}

//Write the source, compile with the real ncc, store the .nmod path.
//-o is explicit: without it ncc drops the module into the process CWD.
//Uses QTest::qFail (not QVERIFY/QFAIL, which expand to a void return)
//so the bool result carries the failure back to the caller's QVERIFY.
bool TestDebugClient::buildModule(
    const char* tag, const char* source, QString* modPathOut) const {
    const QString srcPath = m_dir.filePath(QLatin1String(tag) + ".n");
    const QString modPath = m_dir.filePath(QLatin1String(tag) + ".nmod");
    {
        QFile src(srcPath);
        if (!src.open(QIODevice::WriteOnly | QIODevice::Text)
            || src.write(source) <= 0) {
            QTest::qFail(qPrintable(QStringLiteral(
                "cannot write %1").arg(srcPath)), __FILE__, __LINE__);
            return false;
        }
    }

    QProcess ncc;
    ncc.setProcessChannelMode(QProcess::MergedChannels);
    ncc.start(QString::fromUtf8(NCC_EXE),
        {QStringLiteral("build"), srcPath, QStringLiteral("-o"), modPath});
    if (!ncc.waitForFinished(kToolTimeoutMs)) {
        QTest::qFail(qPrintable(QStringLiteral(
            "ncc timed out for %1").arg(tag)), __FILE__, __LINE__);
        return false;
    }
    if (ncc.exitCode() != 0) {
        QTest::qFail(qPrintable(QStringLiteral("ncc failed for %1: %2")
            .arg(tag, QString::fromUtf8(ncc.readAll()))),
            __FILE__, __LINE__);
        return false;
    }
    if (!QFileInfo::exists(modPath)) {
        QTest::qFail(qPrintable(QStringLiteral(
            "ncc produced no %1").arg(modPath)), __FILE__, __LINE__);
        return false;
    }
    *modPathOut = modPath;
    return true;
}

void TestDebugClient::fullSessionRunsToTheExitCode() {
    DebugClient client(QString::fromUtf8(NDB_EXE));
    QSignalSpy hello(&client, &DebugClient::helloReceived);
    QSignalSpy bound(&client, &DebugClient::breakpointBound);
    QSignalSpy stoppedSpy(&client, &DebugClient::stopped);
    QSignalSpy output(&client, &DebugClient::outputReceived);
    QSignalSpy exited(&client, &DebugClient::exited);
    QSignalSpy failed(&client, &DebugClient::commandFailed);
    QSignalSpy abnormal(&client, &DebugClient::abnormallyExited);

    QVERIFY(client.launch(m_progNmod));
    QCOMPARE(client.state(), DebugClient::State::Launching);
    QVERIFY(client.addBreakpoint(m_progSource, 7));
    QVERIFY(client.run());   //deferred until hello + the bp receipt

    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Stopped, kSessionTimeoutMs);
    //Exactly one user-facing stop: the protocol's initial stop was
    //consumed with an automatic continue; this is the line-7 hit.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(bound.count(), 1);
    QCOMPARE(bound.first().at(0).toInt(), 1);   //bound -> id 1
    QCOMPARE(bound.first().at(3).toBool(), true);
    const QList<QVariant> stop = stoppedSpy.first();
    QCOMPARE(stop.at(0).toString(), QStringLiteral("breakpoint"));
    QCOMPARE(stop.at(1).toInt(), 1);
    QCOMPARE(stop.at(2).toString(), QStringLiteral("main"));
    QCOMPARE(stop.at(4).toInt(), 7);
    QCOMPARE(stop.at(5).toInt(), 1);   //depth: 1-based
    QCOMPARE(stop.at(6).toInt(), 1);   //frameCount
    //Program output streamed while running to the breakpoint.
    QString joined;
    for (const QList<QVariant>& o : output)
        joined += o.at(0).toString();
    QVERIFY(joined.contains(QLatin1String("hi")));

    QSignalSpy btReady(&client, &DebugClient::backtraceReady);
    QSignalSpy frames(&client, &DebugClient::frameReceived);
    QVERIFY(client.requestBacktrace());
    QTRY_COMPARE_WITH_TIMEOUT(btReady.count(), 1, kSessionTimeoutMs);
    QCOMPARE(frames.count(), 1);   //main-only program: exactly frame 0
    QCOMPARE(frames.first().at(0).toInt(), 0);
    QCOMPARE(frames.first().at(1).toString(), QStringLiteral("main"));

    QSignalSpy localsReady(&client, &DebugClient::localsReady);
    QSignalSpy locals(&client, &DebugClient::localReceived);
    QVERIFY(client.requestLocals(0));
    QTRY_COMPARE_WITH_TIMEOUT(localsReady.count(), 1, kSessionTimeoutMs);
    //Line 7 has not executed yet: total=0, i=7.
    bool sawTotal = false, sawI = false;
    for (const QList<QVariant>& l : locals) {
        if (l.at(0).toString() == QLatin1String("total")
            && l.at(1).toString() == QLatin1String("int")
            && l.at(2).toString() == QLatin1String("0"))
            sawTotal = true;
        if (l.at(0).toString() == QLatin1String("i")
            && l.at(2).toString() == QLatin1String("7"))
            sawI = true;
    }
    QVERIFY(sawTotal);
    QVERIFY(sawI);
    QCOMPARE(client.state(), DebugClient::State::Stopped);

    QVERIFY(client.continueRun());
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kSessionTimeoutMs);
    QCOMPARE(exited.count(), 1);
    QCOMPARE(exited.first().at(0).toInt(), 42);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(abnormal.count(), 0);
}

void TestDebugClient::breakpointAddedWhileStoppedHitsLater() {
    DebugClient client(QString::fromUtf8(NDB_EXE));
    QSignalSpy bound(&client, &DebugClient::breakpointBound);
    QSignalSpy stoppedSpy(&client, &DebugClient::stopped);
    QSignalSpy exited(&client, &DebugClient::exited);
    //Pins the one-shot run gate: a stopped-window bp receipt must NOT
    //re-fire `run` (the server would answer err and this spy would see
    //it, even though the session survives).
    QSignalSpy failed(&client, &DebugClient::commandFailed);

    QVERIFY(client.launch(m_progNmod));
    QVERIFY(client.addBreakpoint(m_progSource, 7));
    QVERIFY(client.run());
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Stopped, kSessionTimeoutMs);
    QCOMPARE(stoppedSpy.count(), 1);

    //Gutter click while frozen: the receipt still arrives, the line-8
    //breakpoint hits on the next resume.
    QVERIFY(client.addBreakpoint(m_progSource, 8));
    QTRY_COMPARE_WITH_TIMEOUT(bound.count(), 2, kSessionTimeoutMs);
    QVERIFY(client.continueRun());
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Stopped, kSessionTimeoutMs);
    QCOMPARE(stoppedSpy.count(), 2);
    QCOMPARE(stoppedSpy.last().at(4).toInt(), 8);

    QVERIFY(client.continueRun());
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kSessionTimeoutMs);
    QCOMPARE(exited.first().at(0).toInt(), 42);
    QCOMPARE(failed.count(), 0);
}

void TestDebugClient::killGuaranteeOnAnInfiniteLoop() {
    //THE hard requirement: a run with no breakpoints must stay
    //terminable -- stop() kills, finished() converges, unconditionally.
    DebugClient client(QString::fromUtf8(NDB_EXE));
    QSignalSpy stoppedSpy(&client, &DebugClient::stopped);
    QSignalSpy exited(&client, &DebugClient::exited);

    QVERIFY(client.launch(m_spinNmod));
    QVERIFY(client.run());   //no breakpoints: run goes out after hello
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Running, kSessionTimeoutMs);
    client.stop();
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kKillTimeoutMs);
    //The spin never stopped nor exited on its own.
    QCOMPARE(stoppedSpy.count(), 0);
    QCOMPARE(exited.count(), 0);
}

void TestDebugClient::loadFailureReportsErrorBeforeHello() {
    //Module load failure: the error event PRECEDES hello (there is no
    //hello), and the session converges Ended on the error alone.
    DebugClient client(QString::fromUtf8(NDB_EXE));
    QSignalSpy hello(&client, &DebugClient::helloReceived);
    QSignalSpy errorSpy(&client, &DebugClient::errorReceived);
    QSignalSpy exited(&client, &DebugClient::exited);
    QSignalSpy abnormal(&client, &DebugClient::abnormallyExited);

    QVERIFY(client.launch(m_dir.filePath("missing.nmod")));
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kSessionTimeoutMs);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(!errorSpy.first().at(0).toString().isEmpty());
    QCOMPARE(hello.count(), 0);
    QCOMPARE(exited.count(), 0);   //error is the session-end channel
    QCOMPARE(abnormal.count(), 0);
}

void TestDebugClient::uncaughtThrowEndsWithTheErrorEvent() {
    DebugClient client(QString::fromUtf8(NDB_EXE));
    QSignalSpy hello(&client, &DebugClient::helloReceived);
    QSignalSpy errorSpy(&client, &DebugClient::errorReceived);
    QSignalSpy exited(&client, &DebugClient::exited);

    QVERIFY(client.launch(m_throwNmod));
    QVERIFY(client.run());
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kSessionTimeoutMs);
    QCOMPARE(hello.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    //The report survives the wire with its embedded newlines decoded:
    //"user throw\n  at main (throw.n:2)".
    const QString report = errorSpy.first().at(0).toString();
    QVERIFY(report.contains(QLatin1String("user throw")));
    QVERIFY(report.contains(QLatin1Char('\n')));
    QCOMPARE(exited.count(), 0);
}

void TestDebugClient::stateGuardsAndLaunchRejection() {
    DebugClient client(QString::fromUtf8(NDB_EXE));
    //Nothing is live yet: every session command rejects from Idle.
    QVERIFY(!client.addBreakpoint(QStringLiteral("x.n"), 1));
    QVERIFY(!client.run());
    QVERIFY(!client.continueRun());
    QVERIFY(!client.stepInto());
    QVERIFY(!client.requestBacktrace());
    QVERIFY(!client.requestLocals(0));
    client.stop();   //no-op, must not crash
    QCOMPARE(client.state(), DebugClient::State::Idle);

    QVERIFY(client.launch(m_progNmod));
    QVERIFY(!client.launch(m_progNmod));   //one session per client
    QCOMPARE(client.state(), DebugClient::State::Launching);
    client.stop();   //tidy: kill the idle-at-hello child
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kKillTimeoutMs);
}

void TestDebugClient::failedToLaunchWhenNdbIsMissing() {
    DebugClient client(m_dir.filePath("no-such-ndb.exe"));
    QSignalSpy failedToLaunch(&client, &DebugClient::failedToLaunch);
    QVERIFY(client.launch(m_progNmod));
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), DebugClient::State::Ended, kToolTimeoutMs);
    QCOMPARE(failedToLaunch.count(), 1);
}

QTEST_GUILESS_MAIN(TestDebugClient)
#include "test_debug_client.moc"
