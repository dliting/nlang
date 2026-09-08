/*--- DebugProtocolCodec.h - ndb --machine line-protocol codec ---*/
#ifndef NLANG_TOOLS_NIDE_DEBUG_PROTOCOL_CODEC_H
#define NLANG_TOOLS_NIDE_DEBUG_PROTOCOL_CODEC_H

#include <QString>
#include <QStringList>

namespace nlang {

//One parsed event line from ndb. fields[0] is the event keyword
//("hello", "stopped", ...); the remaining entries are the decoded
//fields in wire order (data tabs/newlines arrive escaped, so a raw tab
//in the line is always a field separator).
struct DebugEvent
{
    enum Kind
    {
        Hello, Bp, Stopped, Frame, Local, Done, Output, Exited, Error,
        Err, Unknown
    };
    Kind kind = Unknown;
    QStringList fields;
};

//Qt-side codec of the ndb --machine wire format (the contract is
//documented on MachineFrontEnd.h). This is a DELIBERATE second
//implementation of the engine-side codec: nide links no nlang headers
//(crash isolation + standalone IDE builds), so drift is pinned closed
//by identical test vectors on both sides -- changing the wire format
//here requires mirroring the engine codec (protocol::EncodeField /
//DecodeField) and its tests.
namespace DebugProtocolCodec {

//Escape the framing metacharacters so one field stays one line and
//tab-joined fields stay unambiguous: \ -> \\, tab -> \t, newline ->
//\n, cr -> \r.
QString encodeField(const QString& field);

//Inverse of encodeField; total over arbitrary input -- unknown escapes
//(\x) and a trailing lone backslash pass through verbatim.
QString decodeField(const QString& field);

//Parse one raw event line as read from ndb's stdout. A trailing \r is
//framing (Windows text mode), not data, and is trimmed before the tab
//split. Unknown keywords yield Kind::Unknown with the line kept as
//fields, so forward compatibility stays total.
DebugEvent parseEvent(const QString& line);

//Join one command line for ndb's stdin: space-separated tokens,
//unescaped (the engine splits `b <file> <line>` at the LAST space, so
//paths with spaces stay one argument -- send single spaces only).
QString encodeCommand(const QString& keyword, const QStringList& args);

} // namespace DebugProtocolCodec

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_DEBUG_PROTOCOL_CODEC_H
