//Stride-table integrity pin: every opcode value in [0, OP_Count) must
//resolve to an extent in [1, 11] — an unlisted opcode throws at runtime
//("unknown opcode in disassembler") far from the authoring mistake.
//The ceiling is the widest deliberate stride: 9 (immediate opcodes)
//until v0.12.7 Task 11's 5-operand OP_MakeArrayView at 11.
//Guards the group-table rewrite of InstructionStride, where inline-
//braced rows of a constexpr aggregate holding initializer_list members
//silently failed to match under MSVC (OP_ConstInt32 walked to the miss
//on every module).
#include "BytecodeOps.h"
#include <iostream>
#include <iterator>

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
        if (stride < 1 || stride > 11) {
            FAIL("stride out of the [1, 11] extent range");
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
    CHECK(InstructionStride(OpCode::OP_MakeArrayView) == 11,
        "opcode + 5x uint16 operands is 11 bytes (widest stride)");
    //v0.13.1 D5: the element-descriptor reshapes - the allocators grew
    //to five u16 operands, the checks to four.
    CHECK(InstructionStride(OpCode::OP_AllocArrayN) == 11,
        "allocator with the element descriptor word is 11 bytes");
    CHECK(InstructionStride(OpCode::OP_AssembleArrayN) == 11,
        "assembler with the element descriptor word is 11 bytes");
    CHECK(InstructionStride(OpCode::OP_CheckArrayType) == 9,
        "array type check with the descriptor word is 9 bytes");
    CHECK(InstructionStride(OpCode::OP_AllocArrayDyn) == 9,
        "dynamic allocator with the descriptor word is 9 bytes");
    PASS();
}


//v0.13.1 D5: an opcode listed in TWO extent groups resolves to the
//first group's stride while the emitters write the new operand shape -
//silent image corruption (a live duplicate shipped briefly during the
//element-descriptor reshape). Membership must be disjoint.
static void test_groups_disjoint()
{
    TEST(groups_disjoint);
    struct GroupRef {
        const OpCode* ops;
        size_t count;
        const char* name;
    };
    const GroupRef groups[] = {
        {kStrideNoOperands, std::size(kStrideNoOperands), "NoOperands"},
        {kStrideByteTag, std::size(kStrideByteTag), "ByteTag"},
        {kStrideOneWord, std::size(kStrideOneWord), "OneWord"},
        {kStrideKindWord, std::size(kStrideKindWord), "KindWord"},
        {kStrideKindByte, std::size(kStrideKindByte), "KindByte"},
        {kStrideTwoWords, std::size(kStrideTwoWords), "TwoWords"},
        {kStrideKindTwoWords, std::size(kStrideKindTwoWords),
         "KindTwoWords"},
        {kStrideThreeWords, std::size(kStrideThreeWords), "ThreeWords"},
        {kStrideFourWords, std::size(kStrideFourWords), "FourWords"},
        {kStrideFiveWords, std::size(kStrideFiveWords), "FiveWords"},
        {kStrideOutMaskWords, std::size(kStrideOutMaskWords),
         "OutMaskWords"},
        {kStrideCmpHead, std::size(kStrideCmpHead), "CmpHead"},
        {kStrideConst32, std::size(kStrideConst32), "Const32"},
        {kStrideConst64, std::size(kStrideConst64), "Const64"},
    };
    for (size_t a = 0; a < std::size(groups); ++a) {
        for (size_t b = a + 1; b < std::size(groups); ++b) {
            for (size_t i = 0; i < groups[a].count; ++i) {
                for (size_t j = 0; j < groups[b].count; ++j) {
                    if (groups[a].ops[i] == groups[b].ops[j]) {
                        FAIL("opcode in two stride groups");
                        std::cerr << "    opcode "
                            << OpCodeName(groups[a].ops[i]) << " in "
                            << groups[a].name << " and "
                            << groups[b].name << "\n";
                        return;
                    }
                }
            }
        }
    }
    PASS();
}

int main()
{
    std::cerr << "=== stride table tests ===" << std::endl;
    test_all_opcodes_resolve();
    test_known_extents();
    test_groups_disjoint();
    std::cerr << "=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===" << std::endl;
    return g_fail;
}
