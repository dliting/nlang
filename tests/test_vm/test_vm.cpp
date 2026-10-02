#include "BytecodeEmitter.h"
#include "BytecodeReader.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

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
    //Placeholder slot: minimal record, nlink fills it at load.
    CompiledFunction fnPlaceholder;
    fnPlaceholder.name = "lib.f";
    fnPlaceholder.paramCount = 1;
    mod.functions.push_back(std::move(fnPlaceholder));
    mod.functionImports.push_back({"lib", "lib.f", 1});

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
    CHECK(loaded.functionImports.size() == 1, "function import count");
    CHECK(loaded.functionImports[0].modulePath == "lib", "fn import modulePath");
    CHECK(loaded.functionImports[0].name == "lib.f", "fn import name");
    CHECK(loaded.functionImports[0].paramCount == 1, "fn import paramCount");
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
    CHECK(loaded.functions.size() == 2, "function table size");
    CHECK(loaded.functions[1].name == "lib.f", "fn placeholder slot name");
    CHECK(loaded.functions[1].paramCount == 1, "fn placeholder paramCount");
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

    std::filesystem::remove(tmpPath);
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

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
