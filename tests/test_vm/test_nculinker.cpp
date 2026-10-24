//nlink unit tests: synthetic per-unit module images drive the load-time
//linker (peer merge by qualified name, placeholder-slot resolution,
//uniform operand remap) with no codegen and no I/O — the design's
//"先立后切" isolation (docs/dev/phase6_loader_design.md §7).
#include "BytecodeEmitter.h"
#include "BytecodeOps.h"
#include "BytecodeReader.h"
#include "NcuLinker.h"
#include "builder/SymbolSlots.hpp"
#include "nlang/vm/CompiledModule.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { \
        std::cerr << "  " << #name << " ... "; \
    } while(0)

#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)

#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

// --- helpers ---

static CompiledModule MakeUnit(const std::string& modulePath) {
    CompiledModule u;
    u.modulePath = modulePath;
    u.name = modulePath;
    return u;
}

static CompiledStruct MakeStruct(const std::string& name) {
    CompiledStruct s;
    s.name = name;
    return s;
}

static CompiledClass MakeClass(const std::string& name) {
    CompiledClass c;
    c.name = name;
    return c;  //empty constructorIndices = no ctor (v2.6)
}

static CompiledFunction MakeFunc(const std::string& name, uint32_t paramCount) {
    CompiledFunction f;
    f.name = name;
    f.paramCount = static_cast<uint16_t>(paramCount);
    return f;
}

static bool LinkThrows(std::vector<CompiledModule> units,
                       const std::string& entryKey, std::string* pMsg) {
    try {
        NcuLinker::Link(std::move(units), entryKey);
    } catch (const std::exception& e) {
        *pMsg = e.what();
        return true;
    }
    return false;
}

static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// --- peer merge across two units, one import of every table kind ---

static void test_link_two_units() {
    TEST(link_two_units);

    //lib: every imported symbol sits behind one extra own entry, so each
    //slot remap below is non-identity (slot N -> merged N+1).
    CompiledModule lib = MakeUnit("lib");
    lib.stringConstants = {"lib-str", "dv"};
    CompiledStruct point;
    point.name = "lib.Point";
    point.fieldCount = 1;
    point.fieldNames = {"x"};
    point.fieldTypeKinds = {RTK_Int32};
    point.fieldStructIndices = {0xFFFF};
    point.fieldClassIndices = {0xFFFF};
    //v2.6 dense invariant: one fieldDefaults entry per field
    //(default-constructed tag = RTK_Void = no default).
    point.fieldDefaults = {DefaultValueDesc{}};
    lib.structs.push_back(MakeStruct("lib.Point0"));  //own 0
    lib.structs.push_back(point);                     //own 1
    CompiledClass rec;
    rec.name = "lib.Rec";
    rec.fieldCount = 1;
    rec.fieldNames = {"v"};
    rec.fieldTypeKinds = {RTK_Int32};
    rec.fieldStructIndices = {0xFFFF};
    rec.fieldClassIndices = {0xFFFF};
    lib.classes.push_back(MakeClass("lib.Rec0"));     //own 0
    lib.classes.push_back(rec);                       //own 1
    lib.enumNames.push_back({"A0"});                  //own 0
    lib.enumKeys.push_back("lib.ColorA");
    lib.enumMethodIndices.push_back({});
    lib.enumMethodAccess.push_back({});
    lib.enumNames.push_back({"Red", "Green"});        //own 1
    lib.enumKeys.push_back("lib.Color");
    lib.enumMethodIndices.push_back({});
    lib.enumMethodAccess.push_back({});
    lib.functions.push_back(MakeFunc("lib.g", 0));    //own 0
    CompiledFunction lf = MakeFunc("lib.f", 1);       //own 1
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_ConstString); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        lf.bytecode = em.TakeBytes();
    }
    lib.functions.push_back(std::move(lf));

    //app: own main + placeholder slots for every lib symbol. Bytecode uses
    //the REAL operand layouts (dst first, table index second — see
    //BytecodeOps.h); both operands of each instruction are asserted below.
    CompiledModule app = MakeUnit("app");
    app.stringConstants = {"app-str", "hello"};
    CompiledFunction mainf = MakeFunc("app.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_ConstString); em.EmitUint16(0);
        em.Emit(OpCode::OP_CallFunc);    em.EmitUint16(1); em.EmitUint16(0);
        em.Emit(OpCode::OP_New);         em.EmitUint16(2); em.EmitUint16(0);
        em.Emit(OpCode::OP_AllocStruct); em.EmitUint16(3);
        em.EmitUint16(0); em.EmitUint16(1);
        em.Emit(OpCode::OP_Enum_to_str); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    app.functions.push_back(std::move(mainf));
    app.functions.push_back(MakeFunc("lib.f", 1));   //slot 1
    app.functionImports.push_back({"lib", "lib.f", 1});
    CompiledClass clsSlot;
    clsSlot.name = "lib.Rec";
    app.classes.push_back(std::move(clsSlot));       //slot 0
    app.classImports.push_back({"lib", "lib.Rec", 0});
    CompiledStruct stSlot;
    stSlot.name = "lib.Point";
    app.structs.push_back(std::move(stSlot));        //slot 0
    app.structImports.push_back({"lib", "lib.Point", 0});
    app.enumNames.push_back({});                     //slot 0: empty names
    app.enumKeys.push_back("lib.Color");
    app.enumMethodIndices.push_back({});
    app.enumMethodAccess.push_back({});
    app.enumImports.push_back({"lib", "lib.Color", 0});

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    CHECK(merged.modulePath == "app", "units[0] modulePath becomes identity");
    //Placeholder records are never copied; lib's real records land once
    //(merged: main + lib.g + lib.f; lib.Point0/lib.Rec0/lib.ColorA first).
    CHECK(merged.functions.size() == 3, "main + lib.g + lib.f only");
    CHECK(merged.functions[1].name == "lib.g"
       && merged.functions[2].name == "lib.f", "function merge order");
    CHECK(merged.classes.size() == 2 && merged.classes[1].name == "lib.Rec",
          "class table");
    CHECK(merged.structs.size() == 2
       && merged.structs[1].name == "lib.Point", "struct table");
    CHECK(merged.enumKeys.size() == 2 && merged.enumKeys[1] == "lib.Color",
          "enum table");
    CHECK(merged.enumNames[1].size() == 2
       && merged.enumNames[1][1] == "Green", "enum names merged");
    CHECK(merged.stringConstants.size() == 4, "string dedup union");
    CHECK(merged.stringConstants[2] == "lib-str", "lib strings appended");

    BytecodeReader rd(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(rd.ReadOp() == OpCode::OP_ConstString && rd.ReadUint16() == 0,
          "own string stays 0");
    CHECK(rd.ReadOp() == OpCode::OP_CallFunc, "CallFunc opcode");
    CHECK(rd.ReadUint16() == 2, "function slot 1 -> merged lib.f at 2");
    rd.ReadUint16();
    CHECK(rd.ReadOp() == OpCode::OP_New, "New opcode");
    CHECK(rd.ReadUint16() == 2, "New dst operand untouched by remap");
    CHECK(rd.ReadUint16() == 1, "class slot 0 -> merged lib.Rec at 1");
    CHECK(rd.ReadOp() == OpCode::OP_AllocStruct, "AllocStruct opcode");
    CHECK(rd.ReadUint16() == 3, "AllocStruct dst operand untouched");
    CHECK(rd.ReadUint16() == 1, "struct slot 0 -> merged lib.Point at 1");
    CHECK(rd.ReadUint16() == 1, "fieldCount operand untouched");
    CHECK(rd.ReadOp() == OpCode::OP_Enum_to_str && rd.ReadUint16() == 1,
          "enum slot 0 -> merged lib.Color at 1");
    CHECK(rd.ReadOp() == OpCode::OP_Return && rd.Eof(), "Return terminator");

    BytecodeReader rf(merged.functions[2].bytecode.data(),
                      merged.functions[2].bytecode.size());
    CHECK(rf.ReadOp() == OpCode::OP_ConstString && rf.ReadUint16() == 2,
          "lib's own string remapped into merged numbering");
    CHECK(merged.entryPoint == 0, "entryKey resolves to app.main");

    PASS();
}

//0.10.14 (v2.9): merging remaps enumMethodIndices through the unit's
//function map — the merged enum must point at the merged function
//record, not the producer-local index.
static void test_enum_method_index_remap_on_merge() {
    TEST(enum_method_index_remap_on_merge);
    CompiledModule lib = MakeUnit("lib");
    lib.enumNames.push_back({"Red", "Green"});
    lib.enumKeys.push_back("lib.Color");
    lib.enumMethodIndices.push_back({0});    //lib's own function 0
    lib.enumMethodAccess.push_back({0});
    lib.functions.push_back(MakeFunc("lib.Color.rank", 1));  //own 0

    CompiledModule app = MakeUnit("app");
    app.functions.push_back(MakeFunc("app.main", 0));
    //The import record rides WITH its placeholder tail entry (the
    //table invariant test_link_two_units mirrors).
    app.functions.push_back(MakeFunc("lib.Color.rank", 1));  //slot 1
    app.functionImports.push_back({"lib", "lib.Color.rank", 1});
    app.enumNames.push_back({});             //placeholder enum slot
    app.enumKeys.push_back("lib.Color");
    app.enumImports.push_back({"lib", "lib.Color", 0});
    app.enumMethodIndices.push_back({});
    app.enumMethodAccess.push_back({});

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    int en = merged.FindEnum("lib.Color");
    CHECK(en >= 0, "merged enum present");
    CHECK(merged.enumMethodIndices[en].size() == 1, "method table merged");
    //Producer-local 0 must land on the merged record. app.main owns
    //merged index 0, so a missed remap reads the wrong name — the
    //assertion discriminates, it cannot pass on a stale index.
    uint16_t remapped = merged.enumMethodIndices[en][0];
    CHECK(remapped < merged.functions.size()
        && merged.functions[remapped].name == "lib.Color.rank",
        "method index remapped to the merged record");
    PASS();
}

// --- builtin class/stub dedup + method/ctor/super index remap ---

static void test_link_builtin_stub_dedup() {
    TEST(link_builtin_stub_dedup);
    //Both units register the Object builtin (identical class + intrinsic
    //method stub): the peer merge must dedup both and remap app.A's super,
    //method and constructor indices into merged numbering.
    CompiledModule app = MakeUnit("app");
    CompiledClass object = MakeClass("Object");
    CompiledFunction ts = MakeFunc("toString", 1);
    ts.intrinsicId = INTR_Object_toString;
    app.functions.push_back(std::move(ts));      //app function 0
    object.methodIndices = {0};
    app.classes.push_back(std::move(object));    //app class 0
    CompiledClass a = MakeClass("app.A");
    a.superClassIdx = 0;                          //app-local Object
    app.functions.push_back(MakeFunc("A", 0));   //app function 1 (ctor)
    a.methodIndices = {1};
    a.constructorIndices = {1};
    app.classes.push_back(std::move(a));         //app class 1
    app.functions.push_back(MakeFunc("app.main", 0));  //app function 2

    CompiledModule lib = MakeUnit("lib");
    CompiledClass libObject = MakeClass("Object");
    CompiledFunction libTs = MakeFunc("toString", 1);
    libTs.intrinsicId = INTR_Object_toString;
    lib.functions.push_back(std::move(libTs));   //lib function 0
    libObject.methodIndices = {0};
    lib.classes.push_back(std::move(libObject)); //lib class 0
    lib.functions.push_back(MakeFunc("lib.f", 0));  //lib function 1

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    CHECK(merged.classes.size() == 2, "lib's Object dedups onto app's");
    CHECK(merged.classes[1].name == "app.A", "class merge order");
    CHECK(merged.classes[1].superClassIdx == 0, "super remapped to merged Object");
    CHECK(merged.classes[0].methodIndices.size() == 1
        && merged.classes[0].methodIndices[0] == 0, "Object methodIndices");
    CHECK(merged.classes[1].methodIndices[0] == 1, "app.A method index");
    CHECK(merged.classes[1].constructorIndices.size() == 1
        && merged.classes[1].constructorIndices[0] == 1, "app.A ctor index");
    int stubCount = 0;
    for (const auto& f : merged.functions)
        if (f.name == "toString")
            ++stubCount;
    CHECK(stubCount == 1, "intrinsic stub deduped across units");
    CHECK(merged.functions.size() == 4,
          "toString + ctor + app.main + lib.f only");
    CHECK(merged.FindFunction("lib.f") == 3, "lib.f after the app block");

    PASS();
}

// --- unresolved imports are all reported in one throw ---

static void test_link_unresolved_all_at_once() {
    TEST(link_unresolved_all_at_once);
    CompiledModule app = MakeUnit("app");
    app.functions.push_back(MakeFunc("app.main", 0));
    //Slot 1: a symbol the claimed module could never provide; slot 2: a
    //module path that is not in the closure at all.
    app.functions.push_back(MakeFunc("nope.g", 0));   //slot 1
    app.functionImports.push_back({"nope", "nope.g", 0});
    app.functions.push_back(MakeFunc("ghost.x", 2));  //slot 2
    app.functionImports.push_back({"ghost", "ghost.x", 2});
    app.classes.push_back(MakeClass("nope.C"));
    app.classImports.push_back({"nope", "nope.C", 0});
    app.structs.push_back(MakeStruct("nope.S"));
    app.structImports.push_back({"nope", "nope.S", 0});
    app.enumNames.push_back({});
    app.enumKeys.push_back("nope.E");
    app.enumMethodIndices.push_back({});
    app.enumMethodAccess.push_back({});
    app.enumImports.push_back({"nope", "nope.E", 0});

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    std::string msg;
    CHECK(LinkThrows(std::move(units), "", &msg),
          "an unresolved closure throws");
    CHECK(Contains(msg, "nope.g"), "diagnostic names the function");
    CHECK(Contains(msg, "nope.C"), "diagnostic names the class");
    CHECK(Contains(msg, "nope.S"), "diagnostic names the struct");
    CHECK(Contains(msg, "nope.E"), "diagnostic names the enum");
    CHECK(Contains(msg, "ghost"), "diagnostic names the missing module");
    CHECK(Contains(msg, "'app'"), "diagnostic names the referencing unit");
    CHECK(Contains(msg, "closure"), "missing-module wording");

    PASS();
}

// --- bare method keys with matching arity must be refused, not guessed ---

static void test_link_ambiguous_function_import() {
    TEST(link_ambiguous_function_import);
    //Two same-named same-arity records in lib (method-style bare keys):
    //(modulePath, name, paramCount) cannot pick one — refuse loudly.
    CompiledModule lib = MakeUnit("lib");
    lib.functions.push_back(MakeFunc("Add", 1));
    lib.functions.push_back(MakeFunc("Add", 1));
    CompiledModule app = MakeUnit("app");
    app.functions.push_back(MakeFunc("app.main", 0));
    app.functions.push_back(MakeFunc("Add", 1));   //slot 1
    app.functionImports.push_back({"lib", "Add", 1});

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    std::string msg;
    CHECK(LinkThrows(std::move(units), "", &msg), "ambiguous import throws");
    CHECK(Contains(msg, "ambiguous"), "diagnostic wording");
    CHECK(Contains(msg, "Add"), "diagnostic names the symbol");

    PASS();
}

// --- per-function metadata: defaultValues, tryBlocks, type descriptors ---

static void test_link_function_metadata_remap() {
    TEST(link_function_metadata_remap);
    CompiledModule lib = MakeUnit("lib");
    lib.stringConstants = {"dv"};
    lib.structs.push_back(MakeStruct("lib.S"));
    lib.structs.push_back(MakeStruct("lib.S2"));
    lib.classes.push_back(MakeClass("lib.Ex"));
    lib.classes.push_back(MakeClass("lib.Ex2"));
    CompiledFunction lf = MakeFunc("lib.f", 1);
    DefaultValueDesc dv;
    dv.tag = RTK_String;
    dv.stringIdx = 0;               //producer-side string index
    lf.defaultValues.push_back(dv);
    TryBlock tb;
    tb.exceptionClassIdx = 0;       //lib-local class index
    lf.tryBlocks.push_back(tb);
    ParamTypeDesc pd;
    pd.type.kind = RTK_Struct;
    pd.type.typeIdx = 1;            //lib.S2
    lf.paramTypeDescs.push_back(pd);
    lf.returnTypeDesc.kind = RTK_Class;
    lf.returnTypeDesc.typeIdx = 1;  //lib.Ex2
    lib.functions.push_back(std::move(lf));

    //app owns one struct + one class, so every lib index shifts by one —
    //the assertions below would pass trivially on identity remaps.
    CompiledModule app = MakeUnit("app");
    app.stringConstants = {"app-str"};
    app.structs.push_back(MakeStruct("app.PS"));
    app.classes.push_back(MakeClass("app.PC"));
    app.functions.push_back(MakeFunc("app.main", 0));
    app.functions.push_back(MakeFunc("lib.f", 1));   //slot 1
    app.functionImports.push_back({"lib", "lib.f", 1});

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "");

    const CompiledFunction& f = merged.functions[1];
    CHECK(f.defaultValues[0].tag == RTK_String
       && f.defaultValues[0].stringIdx == 1,
          "defaultValues stringIdx remapped via the string table");
    CHECK(f.tryBlocks[0].exceptionClassIdx == 1,
          "tryBlock exceptionClassIdx remapped (lib.Ex lands at 1)");
    CHECK(f.paramTypeDescs[0].type.typeIdx == 2,
          "param type descriptor remapped (lib.S2 lands at 2)");
    CHECK(f.returnTypeDesc.typeIdx == 2,
          "return type descriptor remapped (lib.Ex2 lands at 2)");
    CHECK(merged.entryPoint == -1, "empty entryKey => -1");

    PASS();
}

// --- array types: slot-referencing element indices dedup after remap ---

static void test_link_array_types_dedup() {
    TEST(link_array_types_dedup);
    //Both units express "array of lib.Point": lib's elemTypeIdx is an own
    //index, app's is a PLACEHOLDER slot. After slot resolution both must
    //land on one merged array-type entry. lib also owns int[] first, so
    //its Point[] operand (unit-local 1) remaps non-identity to merged 0.
    CompiledModule lib = MakeUnit("lib");
    lib.structs.push_back(MakeStruct("lib.Point"));
    CompiledArrayType intAt;
    intAt.elemKind = RTK_Int32;
    intAt.elemTypeIdx = 0xFFFF;
    lib.arrayTypes.push_back(intAt);                  //own 0
    CompiledArrayType at;
    at.elemKind = RTK_Struct;
    at.elemTypeIdx = 0;
    lib.arrayTypes.push_back(at);                     //own 1
    CompiledFunction lf = MakeFunc("lib.f", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_AllocArrayN);
        em.EmitUint16(2); em.EmitUint16(1); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        lf.bytecode = em.TakeBytes();
    }
    lib.functions.push_back(std::move(lf));

    CompiledModule app = MakeUnit("app");
    app.structs.push_back(MakeStruct("lib.Point"));   //placeholder slot 0
    app.structImports.push_back({"lib", "lib.Point", 0});
    CompiledArrayType appAt;
    appAt.elemKind = RTK_Struct;
    appAt.elemTypeIdx = 0;                             //the slot index
    app.arrayTypes.push_back(appAt);
    CompiledFunction mainf = MakeFunc("app.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_AllocArrayN);
        em.EmitUint16(1); em.EmitUint16(0); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    app.functions.push_back(std::move(mainf));

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    CHECK(merged.arrayTypes.size() == 2, "Point[] dedups; int[] survives");
    CHECK(merged.structs.size() == 1, "struct placeholder not copied");
    BytecodeReader r0(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(r0.ReadOp() == OpCode::OP_AllocArrayN, "AllocArray opcode");
    CHECK(r0.ReadUint16() == 1, "app dst operand untouched");
    CHECK(r0.ReadUint16() == 0, "app array operand -> merged Point[] 0");
    BytecodeReader r1(merged.functions[1].bytecode.data(),
                      merged.functions[1].bytecode.size());
    CHECK(r1.ReadOp() == OpCode::OP_AllocArrayN, "AllocArray opcode");
    CHECK(r1.ReadUint16() == 2, "lib dst operand untouched");
    CHECK(r1.ReadUint16() == 0, "lib Point[] operand 1 -> merged 0");

    PASS();
}

// --- degenerate closure: one unit, no imports, identity remap ---

static void test_link_single_unit_identity() {
    TEST(link_single_unit_identity);
    CompiledModule u = MakeUnit("u");
    u.stringConstants = {"s"};
    CompiledFunction mainf = MakeFunc("u.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_ConstString); em.EmitUint16(0);
        em.Emit(OpCode::OP_MakeFunc);    em.EmitUint16(1);
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    u.functions.push_back(std::move(mainf));
    u.functions.push_back(MakeFunc("u.f", 0));

    std::vector<CompiledModule> units;
    units.push_back(std::move(u));
    CompiledModule merged = NcuLinker::Link(std::move(units), "u.main");

    CHECK(merged.functions.size() == 2, "records unchanged");
    CHECK(merged.entryPoint == 0, "entry resolved");
    BytecodeReader rd(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(rd.ReadOp() == OpCode::OP_ConstString && rd.ReadUint16() == 0,
          "identity string operand");
    CHECK(rd.ReadOp() == OpCode::OP_MakeFunc && rd.ReadUint16() == 1,
          "identity function operand");
    CHECK(rd.ReadOp() == OpCode::OP_Return && rd.Eof(), "end of bytecode");

    PASS();
}

// --- input invariants: closure shape and slot-table consistency ---

static void test_link_input_invariants() {
    TEST(link_input_invariants);
    std::string msg;
    {
        CHECK(LinkThrows({}, "", &msg) && Contains(msg, "empty"),
              "empty closure refused");
    }
    {
        std::vector<CompiledModule> units;
        units.push_back(MakeUnit("dup"));
        units.push_back(MakeUnit("dup"));
        CHECK(LinkThrows(std::move(units), "", &msg) && Contains(msg, "duplicate"),
              "duplicate module path refused");
    }
    {
        std::vector<CompiledModule> units;
        units.push_back(MakeUnit(""));
        CHECK(LinkThrows(std::move(units), "", &msg)
                  && Contains(msg, "module path"),
              "unit without a module path refused");
    }
    {
        CompiledModule app = MakeUnit("app");
        app.functionImports.push_back({"x", "x.f", 0});
        std::vector<CompiledModule> units;
        units.push_back(std::move(app));
        CHECK(LinkThrows(std::move(units), "", &msg) && Contains(msg, "exceed"),
              "import section larger than its table refused");
    }
    {
        CompiledModule u = MakeUnit("u");
        u.functions.push_back(MakeFunc("u.main", 0));
        std::vector<CompiledModule> units;
        units.push_back(std::move(u));
        CHECK(LinkThrows(std::move(units), "u.gone", &msg)
                  && Contains(msg, "u.gone"),
              "dangling entry key refused");
    }

    PASS();
}

// --- cyclic closure: a imports from b while b imports from a ---

static void test_link_cyclic_closure() {
    TEST(link_cyclic_closure);
    //nlink merges only after every image is in hand (design section 4:
    //先全量装载、后统一合并), so a dependency cycle resolves naturally.
    //Each unit keeps an extra own function ahead of the imported target so
    //both call remaps below are non-identity.
    CompiledModule a = MakeUnit("a");
    a.functions.push_back(MakeFunc("a.main", 0));    //own 0
    a.functions.push_back(MakeFunc("a.helper", 0));  //own 1
    a.functions.push_back(MakeFunc("b.f", 0));       //slot 2
    a.functionImports.push_back({"b", "b.f", 0});
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(2); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        a.functions[0].bytecode = em.TakeBytes();
    }

    CompiledModule b = MakeUnit("b");
    b.functions.push_back(MakeFunc("b.pre", 0));     //own 0
    b.functions.push_back(MakeFunc("b.f", 0));       //own 1
    b.functions.push_back(MakeFunc("a.helper", 0));  //slot 2
    b.functionImports.push_back({"a", "a.helper", 0});
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(2); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        b.functions[1].bytecode = em.TakeBytes();
    }

    std::vector<CompiledModule> units;
    units.push_back(std::move(a));
    units.push_back(std::move(b));
    CompiledModule merged = NcuLinker::Link(std::move(units), "a.main");

    //merged: a.main=0, a.helper=1, b.pre=2, b.f=3.
    CHECK(merged.functions.size() == 4, "cycle merges all four functions");
    BytecodeReader r0(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 3,
          "a.main's b.f slot 2 -> merged 3");
    BytecodeReader r3(merged.functions[3].bytecode.data(),
                      merged.functions[3].bytecode.size());
    CHECK(r3.ReadOp() == OpCode::OP_CallFunc && r3.ReadUint16() == 1,
          "b.f's a.helper slot 2 -> merged 1");

    PASS();
}

// --- producer contract: the slot key must include the target module path ---

static void test_function_slot_keyed_by_module() {
    TEST(function_slot_keyed_by_module);
    //FunctionSymbolSlot is the producer side of the placeholder-slot model.
    //Its dedup key must include the target module path: the same key text
    //(here the bare "run", as method-style records legally repeat across
    //packages) from two packages must occupy two slots — collapsing them
    //would bind the second caller to the first package's function.
    {
        CompiledModule app = MakeUnit("app");
        app.functions.push_back(MakeFunc("app.main", 0));
        const uint32_t gfxSlot = FunctionSymbolSlot(app, "gfx", "run", 1);
        const uint32_t ioSlot = FunctionSymbolSlot(app, "io", "run", 1);
        const uint32_t gfxAgain = FunctionSymbolSlot(app, "gfx", "run", 1);
        CHECK(gfxSlot != ioSlot, "same key text from two packages: two slots");
        CHECK(gfxAgain == gfxSlot, "same package + key dedups to one slot");
        CHECK(app.functionImports.size() == 2,
              "one import per distinct package");
        CHECK(app.functionImports[gfxSlot - 1].modulePath == "gfx"
           && app.functionImports[ioSlot - 1].modulePath == "io",
              "slot/import correspondence");
    }

    //The dedup key must also carry the owner-class key: the same bare
    //method name and arity exported by TWO classes of ONE package must
    //occupy two slots — collapsing them would bind the second caller
    //to the first class's method.
    {
        CompiledModule meth = MakeUnit("app");
        meth.functions.push_back(MakeFunc("app.main", 0));
        const uint32_t aSlot = FunctionSymbolSlot(meth, "lib", "add", 1, "lib.A");
        const uint32_t bSlot = FunctionSymbolSlot(meth, "lib", "add", 1, "lib.B");
        const uint32_t aAgain = FunctionSymbolSlot(meth, "lib", "add", 1, "lib.A");
        CHECK(aSlot != bSlot, "one package, two owner classes: two slots");
        CHECK(aAgain == aSlot, "same owner-class key dedups to one slot");
        CHECK(meth.functionImports.size() == 2,
              "one import per distinct owner-class key");
        CHECK(meth.functionImports[aSlot - 1].ownerClassKey == "lib.A"
           && meth.functionImports[bSlot - 1].ownerClassKey == "lib.B",
              "owner-class keys ride on their slot records");
    }

    //Link-level: cross-package functions are referenced by qualified name;
    //each slot must resolve into its own owning unit's region.
    CompiledModule app = MakeUnit("app");
    app.functions.push_back(MakeFunc("app.main", 0));
    const uint32_t gfxSlot = FunctionSymbolSlot(app, "gfx", "gfx.run", 1);
    const uint32_t ioSlot = FunctionSymbolSlot(app, "io", "io.run", 1);
    CompiledModule gfx = MakeUnit("gfx");
    gfx.functions.push_back(MakeFunc("gfx.util", 0));  //own 0
    gfx.functions.push_back(MakeFunc("gfx.run", 1));   //own 1
    CompiledModule io = MakeUnit("io");
    io.functions.push_back(MakeFunc("io.run", 1));
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_CallFunc);
        em.EmitUint16(gfxSlot); em.EmitUint16(0);
        em.Emit(OpCode::OP_CallFunc);
        em.EmitUint16(ioSlot); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        app.functions[0].bytecode = em.TakeBytes();
    }

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(gfx));
    units.push_back(std::move(io));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    //merged: app.main=0, gfx.util=1, gfx.run=2, io.run=3.
    BytecodeReader r0(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 2,
          "gfx slot resolves into the gfx unit's own region");
    r0.ReadUint16();
    CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 3,
          "io slot resolves into the io unit's own region");
    CHECK(merged.functions[2].name == "gfx.run"
       && merged.functions[3].name == "io.run", "owning unit records");

    PASS();
}

// --- owner-keyed method/ctor imports resolve inside the owning class ---

static void test_link_owner_keyed_methods() {
    TEST(link_owner_keyed_methods);
    //Two classes in ONE unit each own add(int) — the same bare key and
    //arity the namespace-level scan must refuse. Owner-class keys
    //disambiguate; the ctor import resolves through its v2.6 ownerOrdinal
    //record into the owner's constructorIndices.
    CompiledModule lib = MakeUnit("lib");
    lib.functions.push_back(MakeFunc("add", 1));   //own 0 (lib.A.add)
    lib.functions.push_back(MakeFunc("A", 0));     //own 1 (lib.A ctor)
    lib.functions.push_back(MakeFunc("add", 1));   //own 2 (lib.B.add)
    lib.functions.push_back(MakeFunc("B", 0));     //own 3 (lib.B ctor)
    lib.functions.push_back(MakeFunc("lib.f", 0)); //own 4
    CompiledClass a = MakeClass("lib.A");
    a.methodIndices = {0};
    a.constructorIndices = {1};
    lib.classes.push_back(a);
    CompiledClass b = MakeClass("lib.B");
    b.methodIndices = {2};
    b.constructorIndices = {3};
    lib.classes.push_back(b);

    CompiledModule app = MakeUnit("app");
    CompiledFunction mainf = MakeFunc("app.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(1); em.EmitUint16(0);
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(2); em.EmitUint16(0);
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(3); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    app.functions.push_back(std::move(mainf));     //own 0
    app.functions.push_back(MakeFunc("add", 1));   //slot 1
    app.functionImports.push_back({"lib", "add", 1, "lib.A", false, 1});
    app.functions.push_back(MakeFunc("add", 1));   //slot 2
    app.functionImports.push_back({"lib", "add", 1, "lib.B", false, 1});
    app.functions.push_back(MakeFunc("B", 0));     //slot 3 (ctor)
    //v2.6 ctor record: ownerKind 1 = class, ownerOrdinal 0 = lib.B's
    //constructorIndices[0] (function 3).
    app.functionImports.push_back({"lib", "B", 0, "lib.B", false, 1, 0});

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    //merged functions: app.main=0, addA=1, ctorA=2, addB=3, ctorB=4.
    BytecodeReader r0(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 1,
          "lib.A.add slot 1 -> merged 1");
    r0.ReadUint16();
    CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 3,
          "lib.B.add slot 2 -> merged 3");
    r0.ReadUint16();
    CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 4,
          "lib.B ctor slot 3 -> merged 4 (ownerOrdinal path)");
    CHECK(merged.classes.size() == 2, "both classes merged");
    CHECK(merged.classes[0].methodIndices.size() == 1
       && merged.classes[0].methodIndices[0] == 1, "lib.A method remap");
    CHECK(merged.classes[0].constructorIndices.size() == 1
       && merged.classes[0].constructorIndices[0] == 2, "lib.A ctor remap");
    CHECK(merged.classes[1].methodIndices[0] == 3, "lib.B method remap");
    CHECK(merged.classes[1].constructorIndices.size() == 1
       && merged.classes[1].constructorIndices[0] == 4, "lib.B ctor remap");

    //An owner class missing from the owning unit is reported with the
    //owner key named.
    {
        CompiledModule libg = MakeUnit("lib");
        libg.functions.push_back(MakeFunc("lib.g", 0));
        CompiledModule app2 = MakeUnit("app2");
        app2.functions.push_back(MakeFunc("app2.main", 0));
        app2.functions.push_back(MakeFunc("add", 1));
        app2.functionImports.push_back({"lib", "add", 1, "lib.Ghost",
                                        false, 1});
        std::vector<CompiledModule> u2;
        u2.push_back(std::move(app2));
        u2.push_back(std::move(libg));
        std::string msg;
        CHECK(LinkThrows(std::move(u2), "", &msg), "owner-class miss throws");
        CHECK(Contains(msg, "lib.Ghost"),
              "diagnostic names the owner class");
    }

    //The class exists but no member matches the imported signature —
    //the diagnosis names the class and the arity mismatch (the
    //owner-class-found arm, distinct from the Ghost class-miss arm).
    {
        CompiledModule libs = MakeUnit("lib");
        libs.functions.push_back(MakeFunc("add", 1));   //lib.A.add(int)
        libs.functions.push_back(MakeFunc("A", 0));     //lib.A ctor
        CompiledClass a = MakeClass("lib.A");
        a.methodIndices = {0};
        a.constructorIndices = {1};
        libs.classes.push_back(a);
        CompiledModule app3 = MakeUnit("app3");
        app3.functions.push_back(MakeFunc("app3.main", 0));
        app3.functions.push_back(MakeFunc("add", 2));
        app3.functionImports.push_back({"lib", "add", 2, "lib.A",
                                        false, 1});
        std::vector<CompiledModule> u3;
        u3.push_back(std::move(app3));
        u3.push_back(std::move(libs));
        std::string msg;
        CHECK(LinkThrows(std::move(u3), "", &msg),
              "owner-class signature miss throws");
        CHECK(Contains(msg, "lib.A")
              && Contains(msg, "provides no member"),
              "diagnosis names the class and the signature miss");
    }

    PASS();
}

// --- v2.6 struct ctor metadata: slot resolve, default remap, corrupt rejects ---

static void test_link_struct_ctor_metadata() {
    TEST(link_struct_ctor_metadata);

    //Happy path: a struct ctor import (ownerKind 2, ownerOrdinal 0) resolves
    //through the owner's constructorIndices, and the struct's fieldDefaults
    //ride the merge with their producer-pool string indices remapped.
    {
        CompiledModule lib = MakeUnit("lib");
        lib.stringConstants = {"ctor-str", "default-tag"};
        lib.functions.push_back(MakeFunc("Vec", 1));  //own 0 (lib.Vec ctor)
        CompiledStruct vec;
        vec.name = "lib.Vec";
        vec.fieldCount = 1;
        vec.fieldNames = {"tag"};
        vec.fieldTypeKinds = {RTK_String};
        vec.fieldStructIndices = {0xFFFF};
        vec.fieldClassIndices = {0xFFFF};
        DefaultValueDesc dv;                    //RTK_String "default-tag"
        dv.tag = RTK_String;
        dv.stringIdx = 1;                       //lib-local pool index
        vec.fieldDefaults = {dv};
        vec.constructorIndices = {0};
        lib.structs.push_back(vec);

        CompiledModule app = MakeUnit("app");
        app.stringConstants = {"app-str"};
        CompiledFunction mainf = MakeFunc("app.main", 0);
        {
            BytecodeEmitter em;
            em.Emit(OpCode::OP_CallFunc); em.EmitUint16(1); em.EmitUint16(0);
            em.Emit(OpCode::OP_Return);
            mainf.bytecode = em.TakeBytes();
        }
        app.functions.push_back(std::move(mainf));  //own 0
        app.functions.push_back(MakeFunc("Vec", 1));  //slot 1 (ctor)
        //ownerKind 2 = struct, ownerOrdinal 0 = lib.Vec's ctor #0.
        app.functionImports.push_back({"lib", "Vec", 1, "lib.Vec",
                                       false, 2, 0});
        CompiledStruct stSlot;
        stSlot.name = "lib.Vec";
        app.structs.push_back(std::move(stSlot));   //slot 0 (placeholder)
        app.structImports.push_back({"lib", "lib.Vec", 0});

        std::vector<CompiledModule> units;
        units.push_back(std::move(app));
        units.push_back(std::move(lib));
        CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

        //merged functions: app.main=0, lib.Vec ctor=1.
        BytecodeReader r0(merged.functions[0].bytecode.data(),
                          merged.functions[0].bytecode.size());
        CHECK(r0.ReadOp() == OpCode::OP_CallFunc && r0.ReadUint16() == 1,
              "struct ctor slot resolves through ownerOrdinal");
        CHECK(merged.structs.size() == 1
           && merged.structs[0].constructorIndices.size() == 1
           && merged.structs[0].constructorIndices[0] == 1,
              "struct ctor index remapped into the merged table");
        //merged strings: app-str=0, ctor-str=1, default-tag=2.
        CHECK(merged.structs[0].fieldDefaults.size() == 1
           && merged.structs[0].fieldDefaults[0].tag == RTK_String
           && merged.structs[0].fieldDefaults[0].stringIdx == 2
           && merged.stringConstants.size() == 3
           && merged.stringConstants[2] == "default-tag",
              "field default string index remapped to the merged pool");
    }

    //Corrupt: a struct ctor index naming the import tail or past the own
    //function table cannot name a constructor.
    {
        CompiledModule lib = MakeUnit("lib");
        lib.functions.push_back(MakeFunc("Vec", 1));  //own 0 only
        CompiledStruct vec = MakeStruct("lib.Vec");
        vec.constructorIndices = {5};
        lib.structs.push_back(vec);
        CompiledModule app = MakeUnit("app");
        app.functions.push_back(MakeFunc("app.main", 0));
        std::vector<CompiledModule> units;
        units.push_back(std::move(app));
        units.push_back(std::move(lib));
        std::string msg;
        CHECK(LinkThrows(std::move(units), "app.main", &msg),
              "struct ctor index outside the own table throws");
        CHECK(Contains(msg, "constructor index 5")
           && Contains(msg, "corrupt image"),
              "diagnostic names the index and the corrupt-image cause");
    }

    //Corrupt: the fieldDefaults block is dense by contract — a short one
    //means the record was truncated or hand-mangled.
    {
        CompiledModule lib = MakeUnit("lib");
        lib.functions.push_back(MakeFunc("lib.g", 0));
        CompiledStruct vec = MakeStruct("lib.Vec");
        vec.fieldCount = 2;
        vec.fieldDefaults = {DefaultValueDesc{}};  //size 1 != fieldCount 2
        lib.structs.push_back(vec);
        CompiledModule app = MakeUnit("app");
        app.functions.push_back(MakeFunc("app.main", 0));
        std::vector<CompiledModule> units;
        units.push_back(std::move(app));
        units.push_back(std::move(lib));
        std::string msg;
        CHECK(LinkThrows(std::move(units), "app.main", &msg),
              "short fieldDefaults block throws");
        CHECK(Contains(msg, "fieldDefaults size 1 != fieldCount 2")
           && Contains(msg, "corrupt image"),
              "diagnostic names both counts and the corrupt-image cause");
    }

    //Malicious import record: ownerOrdinal beyond the owner's ctor list is
    //a signature miss on the found struct (not a missing unit) — the
    //diagnostic uses the struct wording.
    {
        CompiledModule lib = MakeUnit("lib");
        lib.functions.push_back(MakeFunc("Vec", 1));  //own 0 (the only ctor)
        CompiledStruct vec = MakeStruct("lib.Vec");
        vec.constructorIndices = {0};
        lib.structs.push_back(vec);
        CompiledModule app = MakeUnit("app");
        app.functions.push_back(MakeFunc("app.main", 0));
        app.functions.push_back(MakeFunc("Vec", 1));  //slot 1
        app.functionImports.push_back({"lib", "Vec", 1, "lib.Vec",
                                       false, 2, 1});  //ownerOrdinal 1: none
        std::vector<CompiledModule> units;
        units.push_back(std::move(app));
        units.push_back(std::move(lib));
        std::string msg;
        CHECK(LinkThrows(std::move(units), "app.main", &msg),
              "ownerOrdinal past the owner's ctor list throws");
        CHECK(Contains(msg, "struct 'lib.Vec'")
           && Contains(msg, "provides no member with that signature"),
              "diagnostic names the struct and the signature miss");
    }

    PASS();
}

// --- v2.7 interface instance signatures: cross-unit descriptor + default remap ---

static void test_link_interface_signature_remap() {
    TEST(link_interface_signature_remap);

    //lib: one class + one struct (the descriptor targets) and one
    //interface whose instance signature references both by UNIT-LOCAL
    //indices, with a pooled string default.
    CompiledModule lib = MakeUnit("lib");
    lib.stringConstants = {"greet-tag"};             //lib-local pool index 0
    lib.classes.push_back(MakeClass("lib.Widget"));  //own 0
    lib.structs.push_back(MakeStruct("lib.Point"));  //own 0
    CompiledInterface iface;
    iface.name = "lib.Greeter";
    CompiledInterfaceMethod sig;
    sig.name = "greet";
    sig.paramCount = 2;
    ParamTypeDesc clsParam;
    clsParam.type.kind = RTK_Class;
    clsParam.type.typeIdx = 0;                       //lib-local class 0
    ParamTypeDesc structParam;
    structParam.type.kind = RTK_Struct;
    structParam.type.typeIdx = 0;                    //lib-local struct 0
    structParam.flags = PTDF_Out;
    sig.paramTypeDescs = {clsParam, structParam};
    sig.returnType.kind = RTK_Class;
    sig.returnType.typeIdx = 0;                      //lib-local class 0
    DefaultValueDesc dv;                             //RTK_String "greet-tag"
    dv.tag = RTK_String;
    dv.stringIdx = 0;                                //lib-local pool index
    sig.defaultValues = {dv, DefaultValueDesc{}};    //2nd slot: no default
    iface.instanceMethods.push_back(std::move(sig));
    lib.interfaces.push_back(std::move(iface));

    //app: its own class + string shift every lib index by one on merge,
    //so the class-channel remap is non-identity and genuinely pinned.
    CompiledModule app = MakeUnit("app");
    app.stringConstants = {"app-str"};
    app.classes.push_back(MakeClass("app.Host"));    //own 0 -> merged 0
    CompiledFunction mainf = MakeFunc("app.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    app.functions.push_back(std::move(mainf));       //own 0

    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");

    //merged classes: app.Host=0, lib.Widget=1; merged strings:
    //app-str=0, greet-tag=1. The signature's unit-local indices must
    //have been remapped through the lib unit's maps.
    CHECK(merged.interfaces.size() == 1
       && merged.interfaces[0].name == "lib.Greeter"
       && merged.interfaces[0].instanceMethods.size() == 1,
          "interface signature reaches the merged table");
    const CompiledInterfaceMethod& m =
        merged.interfaces[0].instanceMethods[0];
    CHECK(m.paramTypeDescs.size() == 2
       && m.paramTypeDescs[0].type.kind == RTK_Class
       && m.paramTypeDescs[0].type.typeIdx == 1,
          "class-typed formal remapped to the merged class table");
    CHECK(m.paramTypeDescs[1].type.kind == RTK_Struct
       && m.paramTypeDescs[1].type.typeIdx == 0
       && m.paramTypeDescs[1].flags == PTDF_Out,
          "struct-typed formal remapped and its out flag preserved");
    CHECK(m.returnType.kind == RTK_Class && m.returnType.typeIdx == 1,
          "class-typed return remapped to the merged class table");
    CHECK(m.defaultValues.size() == 2
       && m.defaultValues[0].tag == RTK_String
       && m.defaultValues[0].stringIdx == 1
       && merged.stringConstants.size() == 2
       && merged.stringConstants[1] == "greet-tag",
          "default string index remapped to the merged pool");
    CHECK(m.defaultValues[1].tag == RTK_Void,
          "the no-default slot stays RTK_Void through the remap");

    PASS();
}

//0.10.14: a namespace-branch import record with an ownerOrdinal
//single-selects the target record by index — two same-name same-arity
//candidates no longer collide (the old ambiguity error is gone). The
//remapped call operand pins WHICH record was chosen, not just "no
//error": a wrong single-select would land on the other merged index.
static void test_namespace_ordinal_single_select() {
    TEST(namespace_ordinal_single_select);
    CompiledModule lib = MakeUnit("lib");
    lib.functions.push_back(MakeFunc("lib.f", 1));  //own 0: int shape
    lib.functions.push_back(MakeFunc("lib.f", 1));  //own 1: float shape
    CompiledModule app = MakeUnit("app");
    CompiledFunction mainf = MakeFunc("app.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(1); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    app.functions.push_back(std::move(mainf));       //own 0
    app.functions.push_back(MakeFunc("lib.f", 1));   //slot 1 (placeholder)
    //The record names the SECOND candidate (producer index 1).
    app.functionImports.push_back({"lib", "lib.f", 1, "", false, 0, 1});
    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");
    CHECK(merged.functions.size() == 3, "main + both lib.f records");
    //Merged layout: app.main=0, lib own 0->1, lib own 1->2. The slot
    //operand must have been remapped to 2 (the ordinal-1 record).
    BytecodeReader rd(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(rd.ReadOp() == OpCode::OP_CallFunc, "CallFunc opcode");
    CHECK(rd.ReadUint16() == 2, "ordinal 1 -> merged record of producer 1");
    PASS();
}

//0.10.14 (Review Focus #1): a stale consumer record whose ordinal
//points at a drifted signature reports a named error, never a silent
//mislink.
static void test_namespace_ordinal_recheck_mismatch() {
    TEST(namespace_ordinal_recheck_mismatch);
    CompiledModule lib = MakeUnit("lib");
    lib.functions.push_back(MakeFunc("lib.g", 2));  //ordinal 0 exists,
                                                    //arity differs
    CompiledModule app = MakeUnit("app");
    app.functions.push_back(MakeFunc("app.main", 0));
    app.functions.push_back(MakeFunc("lib.g", 1));  //slot 1
    app.functionImports.push_back({"lib", "lib.g", 1, "", false, 0, 0});
    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    std::string msg;
    CHECK(LinkThrows(std::move(units), "", &msg),
          "a drifted ordinal throws");
    CHECK(Contains(msg, "does not match its record"),
          "named stale-artifact diagnostic");
    CHECK(Contains(msg, "lib.g"), "diagnostic names the import");
    PASS();
}

//0.10.14: the 0xFFFF fallback keeps today's scan semantics — a single
//match still links through the scan.
static void test_namespace_fallback_scan_unchanged() {
    TEST(namespace_fallback_scan_unchanged);
    CompiledModule lib = MakeUnit("lib");
    lib.functions.push_back(MakeFunc("lib.h", 1));
    CompiledModule app = MakeUnit("app");
    app.functions.push_back(MakeFunc("app.main", 0));
    app.functions.push_back(MakeFunc("lib.h", 1));  //slot 1
    //0xFFFF ordinal: the legacy scan path stays live.
    app.functionImports.push_back(
        {"lib", "lib.h", 1, "", false, 0, 0xFFFF});
    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");
    CHECK(merged.functions.size() == 2, "main + lib.h");
    PASS();
}

//0.10.14 (Review Focus #5): two same-arity overload records with
//distinct ordinals coexist and each resolves to its own callee (the
//dedup key already carries ownerOrdinal — no collapse).
static void test_two_namespace_records_same_arity() {
    TEST(two_namespace_records_same_arity);
    CompiledModule lib = MakeUnit("lib");
    lib.functions.push_back(MakeFunc("lib.k", 1));  //own 0
    lib.functions.push_back(MakeFunc("lib.k", 1));  //own 1
    CompiledModule app = MakeUnit("app");
    CompiledFunction mainf = MakeFunc("app.main", 0);
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(1); em.EmitUint16(0);
        em.Emit(OpCode::OP_CallFunc); em.EmitUint16(2); em.EmitUint16(0);
        em.Emit(OpCode::OP_Return);
        mainf.bytecode = em.TakeBytes();
    }
    app.functions.push_back(std::move(mainf));        //own 0
    app.functions.push_back(MakeFunc("lib.k", 1));    //slot 1
    app.functionImports.push_back({"lib", "lib.k", 1, "", false, 0, 0});
    app.functions.push_back(MakeFunc("lib.k", 1));    //slot 2
    app.functionImports.push_back({"lib", "lib.k", 1, "", false, 0, 1});
    std::vector<CompiledModule> units;
    units.push_back(std::move(app));
    units.push_back(std::move(lib));
    CompiledModule merged = NcuLinker::Link(std::move(units), "app.main");
    CHECK(merged.functions.size() == 3, "main + both records");
    BytecodeReader rd(merged.functions[0].bytecode.data(),
                      merged.functions[0].bytecode.size());
    CHECK(rd.ReadOp() == OpCode::OP_CallFunc, "first call");
    CHECK(rd.ReadUint16() == 1, "ordinal 0 -> merged record 1");
    rd.ReadUint16();
    CHECK(rd.ReadOp() == OpCode::OP_CallFunc, "second call");
    CHECK(rd.ReadUint16() == 2, "ordinal 1 -> merged record 2");
    PASS();
}

int main() {
#ifdef _WIN32
    //Prevent CRT abort/error dialogs from blocking the test runner.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    std::cerr << "=== NLang nlink Unit Tests ===\n\n";

    try { test_link_two_units(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_enum_method_index_remap_on_merge(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_builtin_stub_dedup(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_unresolved_all_at_once(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_ambiguous_function_import(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_function_metadata_remap(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_array_types_dedup(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_single_unit_identity(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_input_invariants(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_cyclic_closure(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_function_slot_keyed_by_module(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_owner_keyed_methods(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_struct_ctor_metadata(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_link_interface_signature_remap(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_namespace_ordinal_single_select(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_namespace_ordinal_recheck_mismatch(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_namespace_fallback_scan_unchanged(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_two_namespace_records_same_arity(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
