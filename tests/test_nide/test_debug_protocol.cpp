/*--- test_debug_protocol.cpp - Qt-side ndb line-protocol codec tests ---
The escape/roundtrip vectors are IDENTICAL to the engine side's
test_protocol_escape_roundtrip (tests/test_vm/test_debugger.cpp): the
nide codec is a deliberate second implementation of the same wire
format, and these shared vectors pin it closed against drift. ---*/
#include "DebugProtocolCodec.h"

#include <QtTest>

using nlang::DebugEvent;
using nlang::DebugProtocolCodec::decodeField;
using nlang::DebugProtocolCodec::encodeCommand;
using nlang::DebugProtocolCodec::encodeField;
using nlang::DebugProtocolCodec::parseEvent;

class TestDebugProtocol : public QObject {
    Q_OBJECT

private slots:
    void escapeRoundTripPinsTheEngineVectors();
    void unknownEscapePassesThrough();
    void trailingLoneBackslashPassesThrough();
    void encodeCommandJoinsWithSingleSpaces();
    void parseStoppedCarriesAllFields();
    void parseContextCarriesAllFields();
    void parseHelloExitedAndDone();
    void parseBpKeepsAnEmptyFileField();
    void parseLocalAndOutputDecodeFields();
    void parseTrimsTheTrailingCr();
    void parseUnknownKeywordAndEmptyLine();
};

void TestDebugProtocol::escapeRoundTripPinsTheEngineVectors() {
    //Same literal vector as test_protocol_escape_roundtrip on the
    //engine side: all four framing metacharacters escape and round-trip.
    const QString tricky = QStringLiteral("a\\b\tc\nd\r");
    const QString encoded = encodeField(tricky);
    QCOMPARE(encoded, QStringLiteral("a\\\\b\\tc\\nd\\r"));
    QCOMPARE(decodeField(encoded), tricky);
}

void TestDebugProtocol::unknownEscapePassesThrough() {
    //The decoder stays total over arbitrary input.
    QCOMPARE(decodeField(QStringLiteral("\\x")), QStringLiteral("\\x"));
}

void TestDebugProtocol::trailingLoneBackslashPassesThrough() {
    QCOMPARE(decodeField(QStringLiteral("a\\")), QStringLiteral("a\\"));
}

void TestDebugProtocol::encodeCommandJoinsWithSingleSpaces() {
    //Commands are unescaped space-separated tokens; `b` splits file/line
    //at the LAST space, so a path with spaces stays one argument.
    QCOMPARE(encodeCommand(QStringLiteral("b"),
                {QStringLiteral("C:\\my dir\\m.n"), QStringLiteral("9")}),
        QStringLiteral("b C:\\my dir\\m.n 9"));
    QCOMPARE(encodeCommand(QStringLiteral("c"), {}), QStringLiteral("c"));
}

void TestDebugProtocol::parseStoppedCarriesAllFields() {
    const DebugEvent ev = parseEvent(QStringLiteral(
        "stopped\tbreakpoint\t3\tmain\tC:\\x\\m.n\t9\t1\t2"));
    QCOMPARE(ev.kind, DebugEvent::Stopped);
    QCOMPARE(ev.fields.size(), 8);
    QCOMPARE(ev.fields.at(1), QStringLiteral("breakpoint"));
    QCOMPARE(ev.fields.at(2), QStringLiteral("3"));
    QCOMPARE(ev.fields.at(4), QStringLiteral("C:\\x\\m.n"));
    QCOMPARE(ev.fields.at(5), QStringLiteral("9"));
    QCOMPARE(ev.fields.at(6), QStringLiteral("1"));
    QCOMPARE(ev.fields.at(7), QStringLiteral("2"));
}

//context events (contexts responses only) carry the scheduler state,
//the holding carrier (-1 = held by none), and the top-frame location;
//a frameless context keeps the field COUNT with empty func/file and
//line 0 -- empty fields must survive the tab split exactly like bp's.
void TestDebugProtocol::parseContextCarriesAllFields() {
    const DebugEvent ev = parseEvent(QStringLiteral(
        "context\t3\tRunnable\t-1\tmain\tC:\\x\\m.n\t12"));
    QCOMPARE(ev.kind, DebugEvent::Context);
    QCOMPARE(ev.fields.size(), 7);
    QCOMPARE(ev.fields.at(1), QStringLiteral("3"));
    QCOMPARE(ev.fields.at(2), QStringLiteral("Runnable"));
    QCOMPARE(ev.fields.at(3), QStringLiteral("-1"));
    QCOMPARE(ev.fields.at(4), QStringLiteral("main"));
    QCOMPARE(ev.fields.at(5), QStringLiteral("C:\\x\\m.n"));
    QCOMPARE(ev.fields.at(6), QStringLiteral("12"));

    const DebugEvent held = parseEvent(QStringLiteral(
        "context\t1\tRunnable\t0\tworker\tC:\\w\\w.n\t7"));
    QCOMPARE(held.kind, DebugEvent::Context);
    QCOMPARE(held.fields.at(3), QStringLiteral("0"));

    const DebugEvent frameless = parseEvent(QStringLiteral(
        "context\t5\tFinished\t-1\t\t\t0"));
    QCOMPARE(frameless.kind, DebugEvent::Context);
    QCOMPARE(frameless.fields.size(), 7);
    QCOMPARE(frameless.fields.at(4), QString());
    QCOMPARE(frameless.fields.at(5), QString());
    QCOMPARE(frameless.fields.at(6), QStringLiteral("0"));
}

void TestDebugProtocol::parseHelloExitedAndDone() {
    const DebugEvent hello = parseEvent(QStringLiteral("hello\t1"));
    QCOMPARE(hello.kind, DebugEvent::Hello);
    QCOMPARE(hello.fields.at(1), QStringLiteral("1"));
    const DebugEvent exited = parseEvent(QStringLiteral("exited\t42"));
    QCOMPARE(exited.kind, DebugEvent::Exited);
    QCOMPARE(exited.fields.at(1), QStringLiteral("42"));
    const DebugEvent done = parseEvent(QStringLiteral("done\tbt"));
    QCOMPARE(done.kind, DebugEvent::Done);
    QCOMPARE(done.fields.at(1), QStringLiteral("bt"));
}

void TestDebugProtocol::parseBpKeepsAnEmptyFileField() {
    //An unbound bfunc receipt reports an empty location: empty fields
    //must survive the tab split.
    const DebugEvent ev =
        parseEvent(QStringLiteral("bp\t0\t\t0\tunbound"));
    QCOMPARE(ev.kind, DebugEvent::Bp);
    QCOMPARE(ev.fields.size(), 5);
    QCOMPARE(ev.fields.at(2), QString());
    QCOMPARE(ev.fields.at(4), QStringLiteral("unbound"));
}

void TestDebugProtocol::parseLocalAndOutputDecodeFields() {
    const DebugEvent local =
        parseEvent(QStringLiteral("local\ttotal\tint\t0"));
    QCOMPARE(local.kind, DebugEvent::Local);
    QCOMPARE(local.fields.at(1), QStringLiteral("total"));
    QCOMPARE(local.fields.at(3), QStringLiteral("0"));
    //Data tabs/newlines arrive escaped: a raw tab split first, the
    //field content decoded after.
    const DebugEvent output =
        parseEvent(QStringLiteral("output\ta\\tb"));
    QCOMPARE(output.kind, DebugEvent::Output);
    QCOMPARE(output.fields.at(1), QStringLiteral("a\tb"));
    const DebugEvent error =
        parseEvent(QStringLiteral("error\tline1\\nline2"));
    QCOMPARE(error.kind, DebugEvent::Error);
    QCOMPARE(error.fields.at(1), QStringLiteral("line1\nline2"));
}

void TestDebugProtocol::parseTrimsTheTrailingCr() {
    //Windows text mode ends event lines with \r\n: the client splits
    //at the \n, so parseEvent receives the line WITH the \r still on
    //it and trims that framing (a real CR inside a field arrives
    //escaped).
    const DebugEvent ev = parseEvent(QStringLiteral("exited\t42\r"));
    QCOMPARE(ev.kind, DebugEvent::Exited);
    QCOMPARE(ev.fields.at(1), QStringLiteral("42"));
}

void TestDebugProtocol::parseUnknownKeywordAndEmptyLine() {
    const DebugEvent unknown = parseEvent(QStringLiteral("wat\tstuff"));
    QCOMPARE(unknown.kind, DebugEvent::Unknown);
    QCOMPARE(unknown.fields.size(), 2);
    QCOMPARE(unknown.fields.at(1), QStringLiteral("stuff"));
    const DebugEvent empty = parseEvent(QString());
    QCOMPARE(empty.kind, DebugEvent::Unknown);
    QCOMPARE(empty.fields.size(), 1);
}

QTEST_GUILESS_MAIN(TestDebugProtocol)
#include "test_debug_protocol.moc"
