#include "BytecodeEmitter.h"
#include "BytecodeReader.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/runtime/AutoPointers.h"
#include "nlang/runtime/NodeConsts.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

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

// --- BytecodeEmitter / BytecodeReader round-trip ---

void test_emitter_reader_roundtrip() {
    TEST(emitter_reader_roundtrip);
    BytecodeEmitter em;

    em.Emit(OpCode::OP_ConstInt32);
    em.EmitInt32(42);
    em.Emit(OpCode::OP_VarLocal);
    em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign);
    em.EmitUint16(4);
    em.Emit(OpCode::OP_Return);

    auto bytes = em.TakeBytes();
    BytecodeReader rd(bytes.data(), bytes.size());

    CHECK(rd.ReadOp() == OpCode::OP_ConstInt32, "opcode mismatch");
    CHECK(rd.ReadInt32() == 42, "int32 value mismatch");
    CHECK(rd.ReadOp() == OpCode::OP_VarLocal, "opcode mismatch");
    CHECK(rd.ReadUint16() == 0, "uint16 value mismatch");
    CHECK(rd.ReadOp() == OpCode::OP_Assign, "opcode mismatch");
    CHECK(rd.ReadUint16() == 4, "uint16 value mismatch");
    CHECK(rd.ReadOp() == OpCode::OP_Return, "opcode mismatch");
    CHECK(rd.Eof(), "should be at end");

    PASS();
}

void test_patch_uint16() {
    TEST(patch_uint16);
    BytecodeEmitter em;
    em.Emit(OpCode::OP_Jump);
    size_t patchPos = em.CurrentOffset();
    em.EmitInt16(0); // placeholder
    em.Emit(OpCode::OP_Return);

    em.PatchUint16(patchPos, 5);

    auto bytes = em.TakeBytes();
    BytecodeReader rd(bytes.data(), bytes.size());
    CHECK(rd.ReadOp() == OpCode::OP_Jump, "opcode mismatch");
    CHECK(rd.ReadInt16() == 5, "patched value mismatch");

    PASS();
}

void test_string_constant() {
    TEST(string_constant);
    BytecodeEmitter em;
    uint16_t idx1 = em.AddStringConstant("hello");
    uint16_t idx2 = em.AddStringConstant("world");
    uint16_t idx3 = em.AddStringConstant("hello"); // dedup

    CHECK(idx1 == 0, "first string index");
    CHECK(idx2 == 1, "second string index");
    CHECK(idx3 == 0, "dedup should return same index");

    CHECK(em.StringConstants().size() == 2, "should have 2 unique strings");

    PASS();
}

// --- VmExecutor manual bytecode tests ---
// Retired (Phase 10 audit round-7): hand-built CompiledModules have
// incomplete frame layouts (no callParamBase/evalArea/user locals), so
// execution can only crash or loop. The rationale lives in main(); the
// e2e suite (compiles real .n files, executes them) is the execution
// gate. Only serialization-level tests remain in this file.

// --- Module save/load round-trip ---

void test_module_save_load() {
    TEST(module_save_load);
    // Build a module, save via WriteCompiledModule (single writer shared
    // with the compiler backend), load back, verify. The old test hand-wrote
    // a v1.0 byte layout that drifted from the reader and got rejected by
    // the version floor — the shared writer prevents that forever.
    CompiledModule mod;
    mod.name = "test_mod";
    mod.stringConstants = {"hello", "world"};

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 4;
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(7);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Return);
    func.bytecode = em.TakeBytes();

    LocalDescriptor ld;
    ld.offset = 0; ld.size = 4; ld.isParam = 0; ld.typeKind = 1; ld.name = "ret";
    func.locals.push_back(ld);

    mod.functions.push_back(std::move(func));

    // Save via the shared writer
    std::string tmpPath = std::filesystem::temp_directory_path().string() + "/nlang_test_mod.ncu";
    {
        std::ofstream fs(tmpPath, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod, "main"), "WriteCompiledModule failed");
        fs.close();
    }

    // Load
    auto loaded = ModuleLoader::Load(tmpPath);
    CHECK(loaded.name == "test_mod", "module name mismatch");
    //The entry key must name a function in the table (the loader rejects
    //a dangling key), and resolves back to the function's index.
    CHECK(loaded.entryPoint == 0, "entry key resolves to main's index");
    CHECK(loaded.stringConstants.size() == 2, "string count mismatch");
    CHECK(loaded.stringConstants[0] == "hello", "string[0] mismatch");
    CHECK(loaded.stringConstants[1] == "world", "string[1] mismatch");
    CHECK(loaded.functions.size() == 1, "function count mismatch");
    CHECK(loaded.functions[0].name == "main", "function name mismatch");
    CHECK(loaded.functions[0].localsSize == 4, "localsSize mismatch");
    CHECK(loaded.functions[0].bytecode.size() == mod.functions[0].bytecode.size(), "bytecode size mismatch");

    // Execute loaded module — retired: hand-built CompiledModules have
    //incomplete frame layouts (localsSize=4 but VmExecutor expects
    //callParamBase+evalArea+user locals), causing out-of-bounds frame
    //access. Execution correctness is covered by the e2e suite
    //which compiles real .n files. The save/load test verifies only
    //serialization round-trip fidelity here.
    //  VmExecutor exec;
    //  int result = exec.Execute(loaded);
    //  CHECK(result == 7, "expected 7 from loaded module");

    // Cleanup
    std::filesystem::remove(tmpPath);

    PASS();
}

// --- v2.0 symbol import slots round-trip ---

void test_module_import_slots_roundtrip() {
    TEST(module_import_slots_roundtrip);
    //Per-unit placeholder-slot model: own entries occupy table indices
    //0..n-1, cross-unit slots are appended after them and serialized as
    //four parallel import sections at the end of the unit. Pin the
    //round-trip so the writer (ModuleSaver) and reader (ModuleLoader)
    //cannot drift apart, and pin the slot-position invariant (placeholders
    //keep their index; nlink resolves them at load).
    CompiledModule mod;
    mod.name = "app";
    mod.modulePath = "app";

    CompiledFunction own;
    own.name = "app.main";
    own.paramCount = 0;
    mod.functions.push_back(std::move(own));
    //Placeholder slots: minimal records, nlink fills them at load. The
    //first import is owner-keyed (a method of class lib.Rec, v2.6
    //ownerKind 1); the second is a namespace-level function (qualified
    //key, empty owner class key); the third is a constructor import
    //carrying ctorOrdinal 0 (paramCount includes the hidden this).
    CompiledFunction methodPlaceholder;
    methodPlaceholder.name = "add";
    methodPlaceholder.paramCount = 1;
    mod.functions.push_back(std::move(methodPlaceholder));
    mod.functionImports.push_back({"lib", "add", 1, "lib.Rec", false, 1});
    CompiledFunction fnPlaceholder;
    fnPlaceholder.name = "lib.f";
    fnPlaceholder.paramCount = 1;
    mod.functions.push_back(std::move(fnPlaceholder));
    mod.functionImports.push_back({"lib", "lib.f", 1});
    CompiledFunction ctorPlaceholder;
    ctorPlaceholder.name = "Rec";
    ctorPlaceholder.paramCount = 2;
    mod.functions.push_back(std::move(ctorPlaceholder));
    mod.functionImports.push_back({"lib", "Rec", 2, "lib.Rec", false, 1, 0});

    CompiledClass ownClass;
    ownClass.name = "app.Rec";
    mod.classes.push_back(std::move(ownClass));
    CompiledClass clsPlaceholder;
    clsPlaceholder.name = "gfx.color.Rgb";
    mod.classes.push_back(std::move(clsPlaceholder));
    mod.classImports.push_back({"gfx.color", "gfx.color.Rgb", 0});

    CompiledStruct structPlaceholder;
    structPlaceholder.name = "lib.Point";
    mod.structs.push_back(std::move(structPlaceholder));
    mod.structImports.push_back({"lib", "lib.Point", 0});

    mod.enumNames.push_back({"Red", "Green"});
    mod.enumNames.push_back({});  //placeholder slot for lib.Color
    mod.enumKeys.push_back("app.Color");
    mod.enumKeys.push_back("lib.Color");  //placeholder carries the target key
    mod.enumImports.push_back({"lib", "lib.Color", 0});

    std::string tmpPath = std::filesystem::temp_directory_path().string()
        + "/nlang_test_imports.ncu";
    {
        std::ofstream fs(tmpPath, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod, "app.main"),
              "WriteCompiledModule failed");
        fs.close();
    }

    auto loaded = ModuleLoader::Load(tmpPath);
    //Header identity
    CHECK(loaded.modulePath == "app", "modulePath round-trip");
    //Four import sections round-trip field by field
    CHECK(loaded.functionImports.size() == 3, "function import count");
    CHECK(loaded.functionImports[0].modulePath == "lib", "fn import modulePath");
    CHECK(loaded.functionImports[0].name == "add", "fn import name");
    CHECK(loaded.functionImports[0].paramCount == 1, "fn import paramCount");
    CHECK(loaded.functionImports[0].ownerClassKey == "lib.Rec",
          "fn import owner class key");
    //v2.6: the owner kind + ctor ordinal ride the function import record.
    CHECK(loaded.functionImports[0].ownerKind == 1,
          "fn import owner kind (class member)");
    CHECK(loaded.functionImports[0].ctorOrdinal == 0xFFFF,
          "method import is not a ctor slot");
    CHECK(loaded.functionImports[1].modulePath == "lib", "fn import modulePath");
    CHECK(loaded.functionImports[1].name == "lib.f", "fn import name");
    CHECK(loaded.functionImports[1].ownerClassKey.empty(),
          "namespace-level import has an empty owner key");
    CHECK(loaded.functionImports[1].ownerKind == 0,
          "namespace-level import has owner kind 0");
    CHECK(loaded.functionImports[2].modulePath == "lib", "ctor import modulePath");
    CHECK(loaded.functionImports[2].name == "Rec", "ctor import name");
    CHECK(loaded.functionImports[2].ownerClassKey == "lib.Rec",
          "ctor import owner class key");
    CHECK(loaded.functionImports[2].ownerKind == 1,
          "ctor import owner kind (class member)");
    CHECK(loaded.functionImports[2].ctorOrdinal == 0,
          "ctor import carries the ctor ordinal");
    CHECK(loaded.classImports.size() == 1, "class import count");
    CHECK(loaded.classImports[0].modulePath == "gfx.color", "cls import modulePath");
    CHECK(loaded.classImports[0].name == "gfx.color.Rgb", "cls import name");
    CHECK(loaded.structImports.size() == 1, "struct import count");
    CHECK(loaded.structImports[0].modulePath == "lib", "struct import modulePath");
    CHECK(loaded.structImports[0].name == "lib.Point", "struct import name");
    CHECK(loaded.enumImports.size() == 1, "enum import count");
    CHECK(loaded.enumImports[0].modulePath == "lib", "enum import modulePath");
    CHECK(loaded.enumImports[0].name == "lib.Color", "enum import name");
    CHECK(loaded.enumImports[0].paramCount == 0, "enum import paramCount");
    //Enum qualified keys round-trip and stay parallel to enumNames
    CHECK(loaded.enumKeys.size() == 2, "enum key count");
    CHECK(loaded.enumKeys[0] == "app.Color", "own enum key");
    CHECK(loaded.enumKeys[1] == "lib.Color", "placeholder enum key");
    CHECK(loaded.FindEnum("lib.Color") == 1, "FindEnum addresses the slot");
    CHECK(loaded.FindEnum("app.Color") == 0, "FindEnum addresses own key");
    //Placeholder records keep their slot positions
    CHECK(loaded.functions.size() == 4, "function table size");
    CHECK(loaded.functions[1].name == "add", "method placeholder slot name");
    CHECK(loaded.functions[2].name == "lib.f", "fn placeholder slot name");
    CHECK(loaded.functions[2].paramCount == 1, "fn placeholder paramCount");
    CHECK(loaded.functions[3].name == "Rec", "ctor placeholder slot name");
    CHECK(loaded.functions[3].paramCount == 2, "ctor placeholder paramCount");
    CHECK(loaded.classes.size() == 2, "class table size");
    CHECK(loaded.classes[1].name == "gfx.color.Rgb", "cls placeholder name");
    CHECK(loaded.structs.size() == 1, "struct table size");
    CHECK(loaded.enumNames.size() == 2, "enum table size");
    CHECK(loaded.enumNames[1].empty(), "placeholder enumNames stay empty");
    //entryKey resolves back to the own entry's index (indices are not
    //serialized; the loader re-resolves by name)
    CHECK(loaded.entryPoint == loaded.FindFunction("app.main"),
          "entry key resolves to own main");

    //Empty entry key => no entry
    {
        std::ofstream fs(tmpPath, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod, ""), "write empty entry key");
        fs.close();
        auto noEntry = ModuleLoader::Load(tmpPath);
        CHECK(noEntry.entryPoint == -1, "empty entry key => -1");
    }

    //Corrupt-image guard: import slots are table TAILS (own 0..n-1,
    //imports n..n+m), so an import count above the table size would
    //underflow every ownCount consumer (stub minting, linker slot
    //walks) into out-of-bounds reads. The reader must refuse the image.
    {
        mod.functionImports.push_back({"lib", "lib.g", 1});
        mod.functionImports.push_back({"lib", "lib.h", 1});
        std::ofstream fs(tmpPath, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod, "app.main"),
              "write over-imported module");
        fs.close();
        bool refused = false;
        try {
            (void)ModuleLoader::Load(tmpPath);
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find(
                "import count exceeds") != std::string::npos;
        }
        CHECK(refused, "loader refuses imports above the table size");
    }

    std::filesystem::remove(tmpPath);
    PASS();
}

// --- v2.7 full member-surface round-trip (0.9.12 T2) ---

static TypeDesc IntDesc() {
    TypeDesc td;
    td.kind = RTK_Int32;
    return td;
}

static TypeDesc StrDesc() {
    TypeDesc td;
    td.kind = RTK_String;
    return td;
}

static DefaultValueDesc IntDefault(int32_t v) {
    DefaultValueDesc dv;
    dv.tag = RTK_Int32;
    dv.intValue = v;
    return dv;
}

static DefaultValueDesc NoDefault() {
    DefaultValueDesc dv;
    dv.tag = RTK_Void;
    return dv;
}

static DefaultValueDesc StringDefault(uint32_t poolIdx) {
    DefaultValueDesc dv;
    dv.tag = RTK_String;
    dv.stringIdx = poolIdx;
    return dv;
}

//Field-by-field comparison helpers for the v2.7 segments. CHECK's
//early-return leaves the caller's loop running; the failure is already
//counted and printed by FAIL.
static void CheckTypeDescEq(const TypeDesc& got, const TypeDesc& want, const char* what) {
    CHECK(got.kind == want.kind, what);
    CHECK(got.typeIdx == want.typeIdx, what);
    CHECK(got.elems.size() == want.elems.size(), what);
    for (size_t i = 0; i < got.elems.size() && i < want.elems.size(); ++i)
        CheckTypeDescEq(got.elems[i], want.elems[i], what);
}

static void CheckDefaultValueEq(const DefaultValueDesc& got, const DefaultValueDesc& want, const char* what) {
    CHECK(got.tag == want.tag, what);
    CHECK(got.intValue == want.intValue, what);
    CHECK(got.floatValue == want.floatValue, what);
    CHECK(got.stringIdx == want.stringIdx, what);
    CHECK(got.longValue == want.longValue, what);
    CHECK(got.doubleValue == want.doubleValue, what);
    CHECK(got.exprBytes == want.exprBytes, what);
}

static void CheckParamTypeDescEq(const ParamTypeDesc& got, const ParamTypeDesc& want, const char* what) {
    CHECK(got.flags == want.flags, what);
    CheckTypeDescEq(got.type, want.type, what);
}

void test_module_v27_surface_roundtrip() {
    TEST(module_v27_surface_roundtrip);
    //NCU v2.7 carries the complete member surface: own fields with
    //access bytes, every member method (static included) with access
    //and virtuality bytes, constructors with access, struct field
    //defaults and constructor access, and interface instance-method
    //signatures (formals, return kind, defaults). Build one module
    //exercising every v2.7 segment and pin the save/load symmetry —
    //the single writer and the reader cannot drift apart unnoticed.
    CompiledModule mod;
    mod.name = "surface";
    mod.modulePath = "surface";
    mod.stringConstants = { "hello" };

    //0: instance method — private + virtual, one int formal with a
    //default, the hidden this local, a try block, a source path.
    CompiledFunction m;
    m.name = "surface.C.m";
    m.paramCount = 2;   //hidden this + one written formal
    m.localsSize = 1;
    m.returnTypeKind = RTK_Int32;
    m.returnTypeDesc = IntDesc();
    m.paramTypeDescs.push_back({ IntDesc(), 0 });
    m.defaultValues.push_back(IntDefault(7));   //methods: one entry per formal
    LocalDescriptor ld;
    ld.offset = 0; ld.size = 4; ld.isParam = 1; ld.typeKind = RTK_Int32;
    ld.declPc = 0; ld.name = "this";
    m.locals.push_back(ld);
    TryBlock tb;
    tb.startPc = 0; tb.endPc = 3; tb.handlerPc = 5;
    tb.exceptionClassIdx = 0xFFFF; tb.catchLocalOff = 2;
    m.tryBlocks.push_back(tb);
    m.sourceFile = "surface.n";
    {
        BytecodeEmitter em;
        em.Emit(OpCode::OP_Return);
        m.bytecode = em.TakeBytes();
    }
    mod.functions.push_back(std::move(m));

    //1: static method — public, non-virtual, no formals.
    CompiledFunction s;
    s.name = "surface.C.s";
    s.paramCount = 0;
    s.returnTypeKind = RTK_Int32;
    s.returnTypeDesc = IntDesc();
    s.isStatic = true;
    mod.functions.push_back(std::move(s));

    //2/3: constructors — void return (the RTK_Void kind byte gates the
    //return-descriptor block on both sides of the wire).
    CompiledFunction ctor;
    ctor.name = "surface.C.C";
    ctor.paramCount = 1;   //hidden this only
    ctor.returnTypeKind = RTK_Void;
    mod.functions.push_back(std::move(ctor));
    CompiledFunction pctor;
    pctor.name = "surface.P.P";
    pctor.paramCount = 1;
    pctor.returnTypeKind = RTK_Void;
    mod.functions.push_back(std::move(pctor));

    //4: free function — out formal + a no-default slot (free functions
    //carry one default entry per formal, unlike methods).
    CompiledFunction helper;
    helper.name = "surface.helper";
    helper.paramCount = 1;
    helper.returnTypeKind = RTK_Int32;
    helper.returnTypeDesc = IntDesc();
    helper.paramTypeDescs.push_back({ IntDesc(), PTDF_Out });
    helper.defaultValues.push_back(NoDefault());
    mod.functions.push_back(std::move(helper));

    //5: interface static method.
    CompiledFunction make;
    make.name = "surface.I.make";
    make.paramCount = 0;
    make.returnTypeKind = RTK_Int32;
    make.returnTypeDesc = IntDesc();
    make.isStatic = true;
    mod.functions.push_back(std::move(make));

    //Class C: two own fields with distinct access, one virtual private
    //method + one static public method, one protected constructor.
    CompiledClass c;
    c.name = "surface.C";
    c.superClassIdx = -1;
    c.fieldCount = 2;
    c.fieldNames = { "a", "s" };
    c.fieldTypeKinds = { RTK_Int32, RTK_String };
    c.fieldStructIndices = { 0xFFFF, 0xFFFF };
    c.fieldClassIndices = { 0xFFFF, 0xFFFF };
    c.fieldTypeDescs = { IntDesc(), StrDesc() };
    c.fieldAccess = { FA_Private, FA_Public };
    c.methodIndices = { 0, 1 };
    c.methodAccess = { FA_Private, FA_Public };
    c.methodVirtual = { 1, 0 };
    c.constructorIndices = { 2 };
    c.constructorAccess = { FA_Protected };
    mod.classes.push_back(std::move(c));

    //Class D: inherits C, adds one protected own field — v2.7 records
    //carry OWN fields only, the chain link is the superClassIdx.
    CompiledClass d;
    d.name = "surface.D";
    d.superClassIdx = 0;
    d.fieldCount = 1;
    d.fieldNames = { "d" };
    d.fieldTypeKinds = { RTK_Int32 };
    d.fieldStructIndices = { 0xFFFF };
    d.fieldClassIndices = { 0xFFFF };
    d.fieldTypeDescs = { IntDesc() };
    d.fieldAccess = { FA_Protected };
    mod.classes.push_back(std::move(d));

    //Struct P: one field with a constant default + one public ctor.
    CompiledStruct p;
    p.name = "surface.P";
    p.fieldCount = 1;
    p.fieldNames = { "x" };
    p.fieldTypeKinds = { RTK_Int32 };
    p.fieldStructIndices = { 0xFFFF };
    p.fieldClassIndices = { 0xFFFF };
    p.fieldTypeDescs = { IntDesc() };
    p.fieldDefaults = { IntDefault(5) };
    p.constructorIndices = { 3 };
    p.constructorAccess = { FA_Public };
    mod.structs.push_back(std::move(p));

    //Interface I: one static, one constant, two instance signatures —
    //an int default and a string default resolved against the pool.
    CompiledInterface iface;
    iface.name = "surface.I";
    iface.staticMethodIndices = { 5 };
    CompiledInterfaceConstant limit;
    limit.name = "LIMIT";
    limit.type = IntDesc();
    limit.value = IntDefault(42);
    iface.constants.push_back(limit);
    CompiledInterfaceMethod run;
    run.name = "run";
    run.paramCount = 1;
    run.paramTypeDescs.push_back({ IntDesc(), 0 });
    run.returnType = IntDesc();
    run.defaultValues.push_back(IntDefault(7));
    iface.instanceMethods.push_back(run);
    CompiledInterfaceMethod go;
    go.name = "go";
    go.paramCount = 1;
    go.paramTypeDescs.push_back({ StrDesc(), PTDF_Out });
    go.returnType.kind = RTK_Void;   //the kind byte — a descriptor is never zero bytes
    go.defaultValues.push_back(StringDefault(0));
    iface.instanceMethods.push_back(go);
    mod.interfaces.push_back(std::move(iface));

    std::string tmpPath = std::filesystem::temp_directory_path().string()
        + "/nlang_test_surface.ncu";
    {
        std::ofstream fs(tmpPath, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod, ""), "write surface module");
        fs.close();
    }
    auto loaded = ModuleLoader::Load(tmpPath);
    std::filesystem::remove(tmpPath);

    //Header + string pool
    CHECK(loaded.modulePath == "surface", "module path");
    CHECK(loaded.name == "surface", "module name");
    CHECK(loaded.entryPoint == -1, "empty entry key => -1");
    CHECK(loaded.stringConstants.size() == 1, "string pool size");
    if (!loaded.stringConstants.empty())
        CHECK(loaded.stringConstants[0] == "hello", "string pool entry");

    //Functions: every record field, including the v2.7 descriptor
    //segments (param flags, return descriptor gated on RTK_Void).
    CHECK(loaded.functions.size() == 6, "function count");
    for (size_t i = 0; i < loaded.functions.size() && i < mod.functions.size(); ++i) {
        const auto& g = loaded.functions[i];
        const auto& w = mod.functions[i];
        CHECK(g.name == w.name, "function name");
        CHECK(g.localsSize == w.localsSize, "function locals size");
        CHECK(g.paramCount == w.paramCount, "function param count");
        CHECK(g.returnTypeKind == w.returnTypeKind, "function return kind");
        CHECK(g.intrinsicId == w.intrinsicId, "function intrinsic id");
        CHECK(g.isNative == w.isNative, "function native flag");
        CHECK(g.isStatic == w.isStatic, "function static flag");
        CHECK(g.defaultValues.size() == w.defaultValues.size(), "function default count");
        for (size_t j = 0; j < g.defaultValues.size() && j < w.defaultValues.size(); ++j)
            CheckDefaultValueEq(g.defaultValues[j], w.defaultValues[j], "function default value");
        CHECK(g.bytecode == w.bytecode, "function bytecode");
        CHECK(g.tryBlocks.size() == w.tryBlocks.size(), "function try count");
        for (size_t j = 0; j < g.tryBlocks.size() && j < w.tryBlocks.size(); ++j) {
            CHECK(g.tryBlocks[j].startPc == w.tryBlocks[j].startPc, "try block start pc");
            CHECK(g.tryBlocks[j].endPc == w.tryBlocks[j].endPc, "try block end pc");
            CHECK(g.tryBlocks[j].handlerPc == w.tryBlocks[j].handlerPc, "try block handler pc");
            CHECK(g.tryBlocks[j].exceptionClassIdx == w.tryBlocks[j].exceptionClassIdx, "try block exception class");
            CHECK(g.tryBlocks[j].catchLocalOff == w.tryBlocks[j].catchLocalOff, "try block catch slot");
        }
        CHECK(g.locals.size() == w.locals.size(), "function local count");
        for (size_t j = 0; j < g.locals.size() && j < w.locals.size(); ++j) {
            CHECK(g.locals[j].offset == w.locals[j].offset, "local offset");
            CHECK(g.locals[j].size == w.locals[j].size, "local size");
            CHECK(g.locals[j].isParam == w.locals[j].isParam, "local isParam");
            CHECK(g.locals[j].typeKind == w.locals[j].typeKind, "local type kind");
            CHECK(g.locals[j].declPc == w.locals[j].declPc, "local declPc");
            CHECK(g.locals[j].name == w.locals[j].name, "local name");
        }
        CHECK(g.sourceFile == w.sourceFile, "function source file");
        CHECK(g.paramTypeDescs.size() == w.paramTypeDescs.size(), "function param desc count");
        for (size_t j = 0; j < g.paramTypeDescs.size() && j < w.paramTypeDescs.size(); ++j)
            CheckParamTypeDescEq(g.paramTypeDescs[j], w.paramTypeDescs[j], "function param desc");
        if (w.returnTypeKind != RTK_Void)
            CheckTypeDescEq(g.returnTypeDesc, w.returnTypeDesc, "function return desc");
    }

    //Classes: own fields + access, methods + access + virtuality,
    //ctors + access, the chain link.
    CHECK(loaded.classes.size() == 2, "class count");
    for (size_t i = 0; i < loaded.classes.size() && i < mod.classes.size(); ++i) {
        const auto& g = loaded.classes[i];
        const auto& w = mod.classes[i];
        CHECK(g.name == w.name, "class name");
        CHECK(g.fieldCount == w.fieldCount, "class field count");
        CHECK(g.superClassIdx == w.superClassIdx, "class super index");
        CHECK(g.fieldNames == w.fieldNames, "class field names");
        CHECK(g.fieldTypeKinds == w.fieldTypeKinds, "class field kinds");
        CHECK(g.fieldStructIndices == w.fieldStructIndices, "class field struct indices");
        CHECK(g.fieldClassIndices == w.fieldClassIndices, "class field class indices");
        CHECK(g.fieldAccess == w.fieldAccess, "class field access");
        CHECK(g.fieldTypeDescs.size() == w.fieldTypeDescs.size(), "class field desc count");
        for (size_t j = 0; j < g.fieldTypeDescs.size() && j < w.fieldTypeDescs.size(); ++j)
            CheckTypeDescEq(g.fieldTypeDescs[j], w.fieldTypeDescs[j], "class field desc");
        CHECK(g.methodIndices == w.methodIndices, "class method indices");
        CHECK(g.methodAccess == w.methodAccess, "class method access");
        CHECK(g.methodVirtual == w.methodVirtual, "class method virtual flags");
        CHECK(g.constructorIndices == w.constructorIndices, "class ctor indices");
        CHECK(g.constructorAccess == w.constructorAccess, "class ctor access");
    }

    //Structs: fields + descs + constant defaults + ctor access.
    CHECK(loaded.structs.size() == 1, "struct count");
    if (!loaded.structs.empty()) {
        const auto& g = loaded.structs[0];
        const auto& w = mod.structs[0];
        CHECK(g.name == w.name, "struct name");
        CHECK(g.fieldCount == w.fieldCount, "struct field count");
        CHECK(g.fieldNames == w.fieldNames, "struct field names");
        CHECK(g.fieldTypeKinds == w.fieldTypeKinds, "struct field kinds");
        CHECK(g.fieldStructIndices == w.fieldStructIndices, "struct field struct indices");
        CHECK(g.fieldClassIndices == w.fieldClassIndices, "struct field class indices");
        CHECK(g.fieldTypeDescs.size() == w.fieldTypeDescs.size(), "struct field desc count");
        for (size_t j = 0; j < g.fieldTypeDescs.size() && j < w.fieldTypeDescs.size(); ++j)
            CheckTypeDescEq(g.fieldTypeDescs[j], w.fieldTypeDescs[j], "struct field desc");
        CHECK(g.fieldDefaults.size() == w.fieldDefaults.size(), "struct default count");
        for (size_t j = 0; j < g.fieldDefaults.size() && j < w.fieldDefaults.size(); ++j)
            CheckDefaultValueEq(g.fieldDefaults[j], w.fieldDefaults[j], "struct field default");
        CHECK(g.constructorIndices == w.constructorIndices, "struct ctor indices");
        CHECK(g.constructorAccess == w.constructorAccess, "struct ctor access");
    }

    //Interfaces: statics, constants, instance signatures with formals,
    //return kinds and defaults (the v2.7 instance-method segment).
    CHECK(loaded.interfaces.size() == 1, "interface count");
    if (!loaded.interfaces.empty()) {
        const auto& g = loaded.interfaces[0];
        const auto& w = mod.interfaces[0];
        CHECK(g.name == w.name, "interface name");
        CHECK(g.staticMethodIndices == w.staticMethodIndices, "interface static indices");
        CHECK(g.constants.size() == w.constants.size(), "interface constant count");
        for (size_t j = 0; j < g.constants.size() && j < w.constants.size(); ++j) {
            CHECK(g.constants[j].name == w.constants[j].name, "interface constant name");
            CheckTypeDescEq(g.constants[j].type, w.constants[j].type, "interface constant type");
            CheckDefaultValueEq(g.constants[j].value, w.constants[j].value, "interface constant value");
        }
        CHECK(g.instanceMethods.size() == w.instanceMethods.size(), "interface signature count");
        for (size_t j = 0; j < g.instanceMethods.size() && j < w.instanceMethods.size(); ++j) {
            const auto& gs = g.instanceMethods[j];
            const auto& ws = w.instanceMethods[j];
            CHECK(gs.name == ws.name, "interface method name");
            CHECK(gs.paramCount == ws.paramCount, "interface method param count");
            CHECK(gs.paramTypeDescs.size() == ws.paramTypeDescs.size(), "interface method param desc count");
            for (size_t k = 0; k < gs.paramTypeDescs.size() && k < ws.paramTypeDescs.size(); ++k)
                CheckParamTypeDescEq(gs.paramTypeDescs[k], ws.paramTypeDescs[k], "interface method param desc");
            CheckTypeDescEq(gs.returnType, ws.returnType, "interface method return desc");
            CHECK(gs.defaultValues.size() == ws.defaultValues.size(), "interface method default count");
            for (size_t k = 0; k < gs.defaultValues.size() && k < ws.defaultValues.size(); ++k)
                CheckDefaultValueEq(gs.defaultValues[k], ws.defaultValues[k], "interface method default");
        }
    }

    PASS();
}

// --- v2.7 loader validation suite (0.9.12 T2.7 pins) ---

//Serialize through the single writer into an in-memory image.
static std::string ImageOf(const CompiledModule& mod, const std::string& entryKey) {
    std::ostringstream ss(std::ios::binary | std::ios::out);
    if (!WriteCompiledModule(ss, mod, entryKey))
        throw std::runtime_error("WriteCompiledModule failed");
    return ss.str();
}

static std::string MarkerU32(uint32_t len, const char* name) {
    std::string m;
    m.push_back(char(len & 0xFF));
    m.push_back(char((len >> 8) & 0xFF));
    m.push_back(char((len >> 16) & 0xFF));
    m.push_back(char((len >> 24) & 0xFF));
    m += name;
    return m;
}

static std::string MarkerU16(uint16_t len, const char* name) {
    std::string m;
    m.push_back(char(len & 0xFF));
    m.push_back(char((len >> 8) & 0xFF));
    m += name;
    return m;
}

//Locate a byte pattern in the image. These hand-built images are tiny
//and every marker below is unique — assert it instead of assuming it.
static size_t FindUnique(const std::string& img, const std::string& marker) {
    size_t pos = img.find(marker);
    if (pos == std::string::npos || img.find(marker, pos + 1) != std::string::npos)
        throw std::runtime_error("marker not unique in image");
    return pos;
}

static void PatchU8(std::string& img, size_t off, unsigned v) {
    img[off] = char(v & 0xFF);
}

static void PatchU16(std::string& img, size_t off, unsigned v) {
    img[off] = char(v & 0xFF);
    img[off + 1] = char((v >> 8) & 0xFF);
}

static void PatchU32(std::string& img, size_t off, uint32_t v) {
    img[off] = char(v & 0xFF);
    img[off + 1] = char((v >> 8) & 0xFF);
    img[off + 2] = char((v >> 16) & 0xFF);
    img[off + 3] = char((v >> 24) & 0xFF);
}

//Load must refuse the patched image with exactly this diagnostic.
static bool RefusesWith(const std::string& img, const char* diag) {
    try {
        (void)ModuleLoader::LoadFromBytes("corrupt.ncu", img);
        return false;
    } catch (const std::exception& e) {
        return std::string(e.what()).find(diag) != std::string::npos;
    }
}

static bool LoadsCleanly(const std::string& img) {
    try {
        (void)ModuleLoader::LoadFromBytes("clean.ncu", img);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void test_module_v27_corrupt_rejection() {
    TEST(module_v27_corrupt_rejection);
    //Every v2.7 class/interface validation rule gets one byte-patched
    //image: the loader must refuse with its exact diagnostic. Offsets
    //are computed by walking the record's wire layout from the name
    //start (mirrors ReadClassRecords / ReadInterfaceSignatures exactly
    //for these record shapes).

    //Class-side module: one function, one class with one own field, one
    //method and one constructor — every v2.7 class segment present.
    CompiledModule cm;
    cm.name = "corrupt";
    cm.modulePath = "corrupt";
    cm.stringConstants = { "hello" };
    CompiledFunction f;
    f.name = "corrupt.f";
    f.paramCount = 0;
    f.returnTypeKind = RTK_Int32;
    f.returnTypeDesc = IntDesc();
    cm.functions.push_back(std::move(f));
    CompiledClass cc;
    cc.name = "corrupt.C";
    cc.superClassIdx = -1;
    cc.fieldCount = 1;
    cc.fieldNames = { "a" };
    cc.fieldTypeKinds = { RTK_Int32 };
    cc.fieldStructIndices = { 0xFFFF };
    cc.fieldClassIndices = { 0xFFFF };
    cc.fieldTypeDescs = { IntDesc() };
    cc.fieldAccess = { FA_Private };
    cc.methodIndices = { 0 };
    cc.methodAccess = { FA_Public };
    cc.methodVirtual = { 0 };
    cc.constructorIndices = { 0 };
    cc.constructorAccess = { FA_Public };
    cm.classes.push_back(std::move(cc));

    std::string base = ImageOf(cm, "");
    CHECK(LoadsCleanly(base), "clean class image loads");

    const char* kCls = "corrupt.C";
    size_t o = FindUnique(base, MarkerU32(9, kCls)) + 4 + std::strlen(kCls);
    const size_t superOff = o + 2;    o += 4;   //fieldCount u16 + super i16
    o += 4 + 1;                                   //field name len + "a"
    o += 2 + 2 + 2;                               //kind, structIdx, classIdx
    const size_t tdLenOff = o;        o += 2 + 1; //field TypeDesc len + kind byte
    const size_t fieldAccessOff = o;  o += 1;
    const size_t methodCountOff = o;  o += 2;
    const size_t methodIdxOff = o;    o += 2;
    const size_t methodAccessOff = o; o += 1;
    const size_t methodVirtualOff = o; o += 1;
    o += 2;                                       //ctorCount
    const size_t ctorIdxOff = o;                  //ctor index

    { auto img = base; PatchU8(img, fieldAccessOff, FA_Public + 1);
      CHECK(RefusesWith(img, "Invalid module: bad class field access"),
            "field access above FA_Public refused"); }
    { auto img = base; PatchU8(img, methodAccessOff, FA_Public + 1);
      CHECK(RefusesWith(img, "Invalid module: bad class method access"),
            "method access above FA_Public refused"); }
    { auto img = base; PatchU16(img, methodCountOff, 0xFFFF);
      CHECK(RefusesWith(img, "Invalid module: bad class method count"),
            "method count above the ceiling refused"); }
    { auto img = base; PatchU16(img, methodIdxOff, 0xFFFF);
      CHECK(RefusesWith(img, "Invalid module: class method index out of range"),
            "method index above the function table refused"); }
    { auto img = base; PatchU16(img, ctorIdxOff, 0xFFFF);
      CHECK(RefusesWith(img, "Invalid module: class constructor index out of range"),
            "ctor index above the function table refused"); }
    { auto img = base; PatchU8(img, methodVirtualOff, 2);
      CHECK(RefusesWith(img, "Invalid module: bad class method virtual flag"),
            "virtual flag outside 0/1 refused"); }
    { auto img = base; PatchU16(img, tdLenOff, 0xFFFF);
      CHECK(RefusesWith(img, "Invalid module: bad type descriptor length"),
            "field type descriptor length above the cap refused"); }
    { auto img = base; PatchU16(img, superOff, 0);   //self-loop
      CHECK(RefusesWith(img, "Invalid module: corrupt class superclass chain"),
            "self-loop superclass refused"); }

    //Interface-side module: one signature with one formal, an int
    //return and a string default resolved against the pool.
    CompiledModule im;
    im.name = "corrupt";
    im.modulePath = "corrupt";
    im.stringConstants = { "hello" };
    CompiledInterface iface;
    iface.name = "corrupt.I";
    CompiledInterfaceMethod sig;
    sig.name = "run";
    sig.paramCount = 1;
    sig.paramTypeDescs.push_back({ IntDesc(), 0 });
    sig.returnType = IntDesc();
    sig.defaultValues.push_back(StringDefault(0));
    iface.instanceMethods.push_back(std::move(sig));
    im.interfaces.push_back(std::move(iface));

    std::string ibase = ImageOf(im, "");
    CHECK(LoadsCleanly(ibase), "clean interface image loads");

    const char* kIface = "corrupt.I";
    size_t io = FindUnique(ibase, MarkerU16(9, kIface)) + 2 + std::strlen(kIface);
    io += 2;                        //staticCount (0)
    io += 2;                        //constantCount (0)
    io += 2;                        //sigCount (1)
    io += 2 + 3;                    //signature name len + "run"
    io += 2;                        //paramCount (1)
    io += 1;                        //formal flags
    io += 2 + 1;                    //formal TypeDesc len + kind byte
    io += 2 + 1;                    //return TypeDesc len + kind byte
    const size_t defaultCountOff = io;  io += 2;
    const size_t stringIdxOff = io + 9; //DefaultValueDesc: tag 1 + int 4 + float 4

    { auto img = ibase; PatchU16(img, defaultCountOff, 0);
      CHECK(RefusesWith(img, "Invalid module: bad interface method default count"),
            "default count != formal count refused"); }
    { auto img = ibase; PatchU32(img, stringIdxOff, 0x0FFFFFFF);
      CHECK(RefusesWith(img, "Invalid module: bad interface default string index"),
            "string default above the pool refused"); }

    PASS();
}

// --- CompiledModule::FindFunction ---

void test_find_function() {
    TEST(find_function);
    CompiledModule mod;
    CompiledFunction f1, f2;
    f1.name = "foo";
    f2.name = "bar";
    mod.functions.push_back(std::move(f1));
    mod.functions.push_back(std::move(f2));

    CHECK(mod.FindFunction("foo") == 0, "foo should be index 0");
    CHECK(mod.FindFunction("bar") == 1, "bar should be index 1");
    CHECK(mod.FindFunction("baz") == -1, "baz should not be found");

    PASS();
}

// --- UniquePtrCollection move semantics ---
//Contract: move construction transfers BOTH the items and their deleter
//(the destination must be able to destroy what it took over), leaving the
//source empty; move assignment destroys the destination's current items,
//then takes over the source's items and deleter. A counting deleter turns
//a dropped deleter (leak) or a double destruction into a count mismatch.

void test_autopointers_move_semantics() {
    TEST(autopointers_move_semantics);

    struct MoveSemanticsNode
    {
        explicit MoveSemanticsNode(int v) : value(v) {}
        int value;
    };

    int destroyedCount = 0;
    std::function<void(MoveSemanticsNode*)> countingDeleter =
        [&destroyedCount](MoveSemanticsNode* pNode)
        {
            ++destroyedCount;
            delete pNode;
        };

    //UniquePtrList: move construction.
    {
        UniquePtrList<MoveSemanticsNode> source(countingDeleter);
        source.Add(new MoveSemanticsNode(1));
        source.Add(new MoveSemanticsNode(2));

        UniquePtrList<MoveSemanticsNode> moved(std::move(source));
        CHECK(moved.size() == 2, "destination owns the moved items");
        int itemSum = 0;
        for (auto* pNode : moved)
            itemSum += pNode->value;
        CHECK(itemSum == 3, "moved items are intact");
        CHECK(source.empty(), "source is left empty");
    }
    CHECK(destroyedCount == 2,
        "transferred deleter destroys every moved item exactly once");

    //AutoPtrVector: move construction (same contract, random access).
    destroyedCount = 0;
    {
        AutoPtrVector<MoveSemanticsNode> source(countingDeleter);
        source.Add(new MoveSemanticsNode(1));
        source.Add(new MoveSemanticsNode(2));

        AutoPtrVector<MoveSemanticsNode> moved(std::move(source));
        CHECK(moved.size() == 2, "destination owns the moved items");
        CHECK((*moved[0]).value == 1 && (*moved[1]).value == 2,
            "moved items are intact");
        CHECK(source.empty(), "source is left empty");
    }
    CHECK(destroyedCount == 2,
        "transferred deleter destroys every moved item exactly once");

    //UniquePtrList: move assignment replaces prior contents.
    destroyedCount = 0;
    {
        UniquePtrList<MoveSemanticsNode> target(countingDeleter);
        target.Add(new MoveSemanticsNode(10));

        UniquePtrList<MoveSemanticsNode> source(countingDeleter);
        source.Add(new MoveSemanticsNode(20));

        target = std::move(source);
        CHECK(target.size() == 1, "assignment takes over the source items");
        CHECK(source.empty(), "source is left empty");
        CHECK(destroyedCount == 1, "replaced item is destroyed on takeover");
    }
    CHECK(destroyedCount == 2, "moved item destroyed exactly once");

    PASS();
}

int main() {
#ifdef _WIN32
    //Prevent CRT abort/error dialogs from blocking the test runner.
    //VmExecutor::Execute throws on runtime errors; an uncaught exception
    //triggers CRT abort which pops a dialog and hangs automated runs.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    std::cerr << "=== NLang VM Unit Tests ===\n\n";

    // BytecodeEmitter/Reader
    test_emitter_reader_roundtrip();
    test_patch_uint16();
    test_string_constant();

    // CompiledModule
    test_find_function();

    // AutoPointers move semantics (header-only)
    try { test_autopointers_move_semantics(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    // VmExecutor execution tests are retired — see the rationale at the
    // former section above. The e2e suite (real .n files, real execution)
    // is the authoritative gate.

    // Module save/load (uses WriteCompiledModule — frame layout is correct)
    try { test_module_save_load(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    // v2.0 symbol import slots (serialization round-trip)
    try { test_module_import_slots_roundtrip(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    // v2.7 full member surface (round-trip symmetry + loader checks)
    try { test_module_v27_surface_roundtrip(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_module_v27_corrupt_rejection(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
