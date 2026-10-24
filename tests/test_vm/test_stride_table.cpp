//Stride-table integrity pin: every opcode value in [0, OP_Count) must
//resolve to an extent in [1, 9] — an unlisted opcode throws at runtime
//("unknown opcode in disassembler") far from the authoring mistake.
//Guards the group-table rewrite of InstructionStride, where inline-
//braced rows of a constexpr aggregate holding initializer_list members
//silently failed to match under MSVC (OP_ConstInt32 walked to the miss
//on every module).
#include "BytecodeOps.h"
#include <iostream>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void test_all_opcodes_resolve()
{
    TEST(all_opcodes_resolve);
    for (int value = 0; value < static_cast<int>(OpCode::OP_Count);
            ++value) {
        const OpCode op = static_cast<OpCode>(value);
        size_t stride = 0;
        try {
            stride = InstructionStride(op);
        } catch (const std::exception&) {
            FAIL("opcode with no stride group");
            std::cerr << "    missing opcode value: " << value << "\n";
            return;
        }
        if (stride < 1 || stride > 9) {
            FAIL("stride out of the [1, 9] extent range");
            std::cerr << "    opcode " << value << " stride " << stride
                      << "\n";
            return;
        }
    }
    PASS();
}

static void test_known_extents()
{
    TEST(known_extents);
    CHECK(InstructionStride(OpCode::OP_Return) == 1,
        "no-operand opcode is 1 byte");
    CHECK(InstructionStride(OpCode::OP_DebugInfo) == 3,
        "opcode + uint16 line is 3 bytes");
    CHECK(InstructionStride(OpCode::OP_PrimCast) == 3,
        "opcode + two kind bytes is 3 bytes");
    CHECK(InstructionStride(OpCode::OP_ConstInt32) == 5,
        "opcode + int32 immediate is 5 bytes");
    CHECK(InstructionStride(OpCode::OP_ConstDouble) == 9,
        "opcode + 8-byte immediate is 9 bytes");
    CHECK(InstructionStride(OpCode::OP_CallDelegateOut) == 9,
        "opcode + 2x uint16 + uint32 outMask is 9 bytes");
    PASS();
}

int main()
{
    std::cerr << "=== stride table tests ===" << std::endl;
    test_all_opcodes_resolve();
    test_known_extents();
    std::cerr << "=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===" << std::endl;
    return g_fail;
}
