/*---
test_primitivetypes.cpp - registry-derived facts about the 12 scalar
primitives (0.7.5 basic types). Pins the registry as the single source
of truth: row count and index order, slot widths, RTK code allocation
(0/1 legacy scalars, 10..19 new, descriptor-only 8/9 absent), kind/rtk
round-trips, Rn singleton wiring, and the derived category predicates.
Console-style suite (same shape as test_array_token). In-process host:
Runtime::StaticInit() must run before anything else.
---*/
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/runtime/Runtime.h>
#include <nlang/runtime/RnTypes.h>
#include <cstdio>
#include <iostream>
#include <string>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::printf("  %s ... ", #name); } while(0)
#define PASS() \
    do { ++g_pass; std::printf("OK\n"); } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cout << "FAIL: " << (msg) << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

static const ScalarPrimInfo *FindRow(const char *kw)
{
    for (size_t i = 0; i < kScalarPrimCount; ++i)
        if (std::string(kScalarPrims[i].name) == kw)
            return &kScalarPrims[i];
    return nullptr;
}

static void test_registry_shape()
{
    TEST(registry_shape);
    CHECK(kScalarPrimCount == 12, "12 scalar primitives expected");

    //Index order fixes promotion rank derivations later tasks rely on
    //(the two pre-existing types int/float lead the table).
    static const char *kExpectedOrder[] = {
        "int", "float", "byte", "ubyte", "short", "ushort",
        "uint", "long", "ulong", "double", "bool", "char"
    };
    for (size_t i = 0; i < kScalarPrimCount; ++i)
        CHECK(std::string(kScalarPrims[i].name) == kExpectedOrder[i],
            std::string("row ") + std::to_string(i) + " should be " +
            kExpectedOrder[i] + ", got " + kScalarPrims[i].name);

    //Slot widths (frame layout, .nmod LocalDescriptor sizes derive here).
    struct WidthExpect { const char *kw; uint8_t width; };
    static const WidthExpect kWidths[] = {
        {"byte", 1}, {"ubyte", 1}, {"short", 2}, {"ushort", 2},
        {"int", 4}, {"uint", 4}, {"float", 4}, {"bool", 4}, {"char", 4},
        {"long", 8}, {"ulong", 8}, {"double", 8}
    };
    for (const auto &e : kWidths)
    {
        const ScalarPrimInfo *row = FindRow(e.kw);
        CHECK(row != nullptr, std::string(e.kw) + " missing from registry");
        CHECK(row->slotWidth == e.width,
            std::string(e.kw) + " slot width should be " +
            std::to_string(e.width));
    }
    PASS();
}

static void test_rtk_allocation()
{
    TEST(rtk_allocation);
    CHECK(RTK_Int32 == 0, "RTK_Int32 must keep its legacy wire value 0");
    CHECK(RTK_Float == 1, "RTK_Float must keep its legacy wire value 1");

    //Unique codes, all inside the scalar segment 0..19, none colliding
    //with the non-scalar legacy block (2..7) or the TypeDesc
    //descriptor-only 8/9.
    for (size_t i = 0; i < kScalarPrimCount; ++i)
    {
        const uint8_t rtk = kScalarPrims[i].rtk;
        CHECK(rtk <= 19, "scalar rtk must stay within 0..19");
        CHECK(rtk < 2 || rtk >= 10,
            "scalar rtk must not land in the legacy non-scalar block");
        for (size_t j = i + 1; j < kScalarPrimCount; ++j)
            CHECK(kScalarPrims[i].rtk != kScalarPrims[j].rtk,
                "duplicate rtk across registry rows");
    }

    //Non-scalar codes resolve to -1 (membership goes through the table,
    //never a range compare).
    CHECK(ScalarPrimIndexOfRtk(2) == -1, "RTK_String must not be scalar");
    CHECK(ScalarPrimIndexOfRtk(8) == -1, "RTK_List descriptor must not be scalar");
    CHECK(ScalarPrimIndexOfRtk(9) == -1, "RTK_Dict descriptor must not be scalar");
    CHECK(ScalarPrimIndexOfRtk(0xFF) == -1, "0xFF sentinel must not be scalar");

    //Kind/rtk round-trips for every row.
    for (size_t i = 0; i < kScalarPrimCount; ++i)
    {
        const NodeKind k = kScalarPrims[i].kind;
        CHECK(ScalarPrimIndexOf(k) == static_cast<int>(i),
            "ScalarPrimIndexOf must return the registry index");
        CHECK(ScalarPrimIndexOfRtk(RtkOfKind(k)) == static_cast<int>(i),
            "RtkOfKind/ScalarPrimIndexOfRtk must round-trip");
    }
    CHECK(ScalarPrimIndexOf(NK_String) == -1, "string is not a scalar prim");
    CHECK(ScalarPrimIndexOf(NK_Type) == -1, "type is not a scalar prim");
    CHECK(RtkOfKind(NK_String) == 0xFF, "non-scalar kind maps to 0xFF");
    PASS();
}

static void test_rn_instances()
{
    TEST(rn_instances);
    //One macro-generated block over all 12 rows (RnInt32/RnFloat are the
    //pre-existing hand-written singletons). Name() is the C++-side trait
    //description (the #T form, "Int32" for int today) — the language
    //keyword lives in the registry row, the two are deliberately distinct.
#define CHECK_INSTANCE(T, Kw, Width, Carrier, Cat, Rank)                        \
    do {                                                                       \
        Rn##T *p = Rn##T::Instance();                                          \
        CHECK(p != nullptr, #T " instance should exist");                      \
        CHECK(p->Kind() == NK_##T, #T " kind mismatch");                       \
        CHECK(std::string(p->Name().ToString()) == #T,                         \
            #T " trait name mismatch");                                        \
    } while (0);
    SCALAR_PRIMITIVE_DECL(CHECK_INSTANCE)
#undef CHECK_INSTANCE
    PASS();
}

static void test_categories()
{
    TEST(categories);
    //Category + numeric/traceable predicates (bool/char are non-numeric;
    //no scalar kind ever holds a heap reference).
    CHECK(FindRow("bool")->category == PC_Bool, "bool category");
    CHECK(FindRow("char")->category == PC_Char, "char category");
    CHECK(FindRow("double")->category == PC_Float, "double category");
    CHECK(FindRow("ulong")->category == PC_UInt, "ulong category");
    CHECK(FindRow("byte")->category == PC_SInt, "byte category");
    for (size_t i = 0; i < kScalarPrimCount; ++i)
    {
        const NodeKind k = kScalarPrims[i].kind;
        CHECK(!PrimKindIsTraceable(k), "scalars are never GC-traceable");
        const bool numeric = kScalarPrims[i].category == PC_SInt ||
                             kScalarPrims[i].category == PC_UInt ||
                             kScalarPrims[i].category == PC_Float;
        CHECK(PrimKindIsNumeric(k) == numeric,
            std::string(kScalarPrims[i].name) + " numeric predicate mismatch");
    }
    PASS();
}

static void test_nodekind_layout()
{
    TEST(nodekind_layout);
    //Post-expansion NK values: the 12 scalars occupy 0..11, string stays
    //the LAST primitive row (12) so the NK_EXTEND_DT_BEFORE alias and the
    //macro-generated traits table keep their invariants.
    CHECK(NK_DT_COUNT == 15, "13 primitives + Type + Void expected");
    CHECK(NK_String == 12, "string must stay the last primitive kind");
    CHECK(NK_Char == 11, "char closes the scalar block");
    CHECK(NK_Byte == 2, "byte opens the new scalar block");
    CHECK(IsPrimitiveType(NK_Char), "char must be a primitive type");
    CHECK(IsPrimitiveType(NK_String), "string stays a primitive type");
    CHECK(!IsPrimitiveType(NK_Type), "type is an extend type");
    PASS();
}

int main()
{
    Runtime::StaticInit();

    test_registry_shape();
    test_rtk_allocation();
    test_rn_instances();
    test_categories();
    test_nodekind_layout();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
