// --- ndb --machine line-protocol codec (Qt side) ---
// Mirrors protocol::EncodeField/DecodeField in MachineFrontEnd.h; the
// cross-pinned test vectors in tests/test_nide/test_debug_protocol.cpp
// and tests/test_vm/test_debugger.cpp keep the two sides identical.

#include "DebugProtocolCodec.h"

namespace nlang {

namespace DebugProtocolCodec {

QString encodeField(const QString& field)
{
    QString out;
    out.reserve(field.size());
    const int count = field.size();
    for (int i = 0; i < count; ++i) {
        switch (field.at(i).unicode()) {
        case '\\': out += QLatin1String("\\\\"); break;
        case '\t': out += QLatin1String("\\t"); break;
        case '\n': out += QLatin1String("\\n"); break;
        case '\r': out += QLatin1String("\\r"); break;
        default: out += field.at(i); break;
        }
    }
    return out;
}

QString decodeField(const QString& field)
{
    QString out;
    out.reserve(field.size());
    const int count = field.size();
    for (int i = 0; i < count; ++i) {
        const QChar c = field.at(i);
        if (c != QLatin1Char('\\') || i + 1 == count) {
            out += c;
            continue;
        }
        const QChar next = field.at(++i);
        switch (next.unicode()) {
        case '\\': out += QLatin1Char('\\'); break;
        case 't': out += QLatin1Char('\t'); break;
        case 'n': out += QLatin1Char('\n'); break;
        case 'r': out += QLatin1Char('\r'); break;
        default:
            out += QLatin1Char('\\');
            out += next;
            break;
        }
    }
    return out;
}

DebugEvent parseEvent(const QString& line)
{
    QString trimmed = line;
    while (trimmed.endsWith(QLatin1Char('\r')))
        trimmed.chop(1);
    const QStringList rawFields =
        trimmed.split(QLatin1Char('\t'), QString::KeepEmptyParts);
    DebugEvent ev;
    ev.fields.reserve(rawFields.size());
    for (const QString& raw : rawFields)
        ev.fields << decodeField(raw);
    const QString keyword = ev.fields.value(0);
    if (keyword == QLatin1String("hello")) ev.kind = DebugEvent::Hello;
    else if (keyword == QLatin1String("bp")) ev.kind = DebugEvent::Bp;
    else if (keyword == QLatin1String("stopped")) ev.kind = DebugEvent::Stopped;
    else if (keyword == QLatin1String("frame")) ev.kind = DebugEvent::Frame;
    else if (keyword == QLatin1String("local")) ev.kind = DebugEvent::Local;
    else if (keyword == QLatin1String("done")) ev.kind = DebugEvent::Done;
    else if (keyword == QLatin1String("output")) ev.kind = DebugEvent::Output;
    else if (keyword == QLatin1String("exited")) ev.kind = DebugEvent::Exited;
    else if (keyword == QLatin1String("error")) ev.kind = DebugEvent::Error;
    else if (keyword == QLatin1String("err")) ev.kind = DebugEvent::Err;
    else ev.kind = DebugEvent::Unknown;
    return ev;
}

QString encodeCommand(const QString& keyword, const QStringList& args)
{
    QStringList tokens;
    tokens.reserve(args.size() + 1);
    tokens << keyword << args;
    return tokens.join(QLatin1Char(' '));
}

} // namespace DebugProtocolCodec

} // namespace nlang
