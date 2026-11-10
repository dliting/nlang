/*---
    DisassemblerFormat.cpp — the shared instruction-line formatters
    (extracted from Disassembler.cpp so the operand-family TUs share
    one definition instead of duplicating them).
---*/
#include "DisassemblerFormat.h"
#include <cstdio>

namespace nlang {

//Emit a line with no operands beyond the head.
void EmitPlain(std::ostringstream &out, const std::string &head) {
    out << head << "\n";
}

//Print `n` uint16 operands (n = 1..5), reading the array BEFORE formatting
//(MSVC reverses in-arg read order; snprintf ignores the unused slots).
void DisasmU16xN(int n, BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const char *fmt) {
    uint16_t v[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < n; ++i)
        v[i] = r.ReadUint16();
    char buf[96];
    snprintf(buf, sizeof(buf), fmt, v[0], v[1], v[2], v[3], v[4]);
    out << head << buf << "\n";
}

void EmitU8Tag(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    char buf[48];
    snprintf(buf, sizeof(buf), " typeTag=%d", r.ReadByte());
    out << head << buf << "\n";
}

} // namespace nlang
