#include "BytecodeEmitter.h"
#include "BytecodeReader.h"
#include "CompiledModule.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { \
        std::cout << "  " << #name << " ... "; \
    } while(0)

#define PASS() \
    do { ++g_pass; std::cout << "OK\n"; } while(0)

#define FAIL(msg) \
    do { ++g_fail; std::cout << "FAIL: " << msg << "\n"; } while(0)

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

void test_vm_const_int32_return() {
    TEST(vm_const_int32_return);
    // main() { return 42; }
    // Bytecode: ConstInt32 42, VarLocal 0, Return
    // locals layout: [0..3] = return slot
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 4; // return slot
    func.paramCount = 0;
    func.returnTypeKind = 1; // int32

    BytecodeEmitter em;
    em.Emit(OpCode::OP_ConstInt32);
    em.EmitInt32(42);
    em.Emit(OpCode::OP_Assign);
    em.EmitUint16(0); // return slot at offset 0
    em.Emit(OpCode::OP_VarLocal);
    em.EmitUint16(0);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 42, "expected 42");

    PASS();
}

void test_vm_add_i32() {
    TEST(vm_add_i32);
    // Simulate: a = 10, b = 20, c = a + b, return c
    // locals: [0..3]=a, [4..7]=b, [8..11]=c, [12..15]=return slot, [16..19]=temp
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 20;
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 10
    em.Emit(OpCode::OP_ConstInt32);
    em.EmitInt32(10);
    em.Emit(OpCode::OP_Assign);
    em.EmitUint16(0);
    // b = 20
    em.Emit(OpCode::OP_ConstInt32);
    em.EmitInt32(20);
    em.Emit(OpCode::OP_Assign);
    em.EmitUint16(4);
    // c = a + b  (Add_i32: locals[8] += locals[4], but first copy a to c)
    em.Emit(OpCode::OP_VarLocal);
    em.EmitUint16(0); // read a
    em.Emit(OpCode::OP_Assign);
    em.EmitUint16(8); // c = a
    em.Emit(OpCode::OP_Add_i32);
    em.EmitUint16(8); // dst = c
    em.EmitUint16(4); // src = b
    // return c
    em.Emit(OpCode::OP_VarLocal);
    em.EmitUint16(8);
    em.Emit(OpCode::OP_Assign);
    em.EmitUint16(12); // return slot
    em.Emit(OpCode::OP_VarLocal);
    em.EmitUint16(12);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 30, "expected 30");

    PASS();
}

void test_vm_sub_mul_div() {
    TEST(vm_sub_mul_div);
    // Test: a=100, b=5, c=a-b=95, d=c*2=190, e=d/10=19, return e
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 28; // a(0), b(4), c(8), d(12), e(16), ret(20), temp(24)
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 100
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(100);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    // b = 5
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(5);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(4);
    // c = a - b
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(8);
    em.Emit(OpCode::OP_Sub_i32); em.EmitUint16(8); em.EmitUint16(4);
    // d = c * 2
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(8);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(12);
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(2);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(24); // temp
    em.Emit(OpCode::OP_Mul_i32); em.EmitUint16(12); em.EmitUint16(24);
    // e = d / 10
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(12);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(16);
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(10);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(24); // temp
    em.Emit(OpCode::OP_Div_i32); em.EmitUint16(16); em.EmitUint16(24);
    // return e
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(16);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(20); // ret slot
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(20);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 19, "expected 19");

    PASS();
}

void test_vm_comparison() {
    TEST(vm_comparison);
    // Test: a=10, b=20, c=(a<b)?1:0, return c => should be 1
    // OP_Less_i32 writes result to locals[lhs], not pResult.
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 16; // a(0), b(4), c(8), ret(12)
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 10
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(10);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    // b = 20
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(20);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(4);
    // copy a to c, then Less_i32 compares c < b, writes result to c
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(8);
    em.Emit(OpCode::OP_Less_i32); em.EmitUint16(8); em.EmitUint16(4);
    // return c
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(8);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(12);
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(12);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 1, "expected 1 (10 < 20)");

    PASS();
}

void test_vm_function_call() {
    TEST(vm_function_call);
    // helper() { return 99; }
    // main() { return helper(); }
    // OP_CallFunc takes func_index and call_param_base.
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction helperFunc;
    helperFunc.name = "helper";
    helperFunc.localsSize = 8; // retSlot(0), temp(4)
    helperFunc.paramCount = 0;
    helperFunc.returnTypeKind = 1;

    BytecodeEmitter helperEm;
    helperEm.Emit(OpCode::OP_ConstInt32); helperEm.EmitInt32(99);
    helperEm.Emit(OpCode::OP_Assign); helperEm.EmitUint16(0);
    helperEm.Emit(OpCode::OP_VarLocal); helperEm.EmitUint16(0);
    helperEm.Emit(OpCode::OP_Return);
    helperFunc.bytecode = helperEm.TakeBytes();

    CompiledFunction mainFunc;
    mainFunc.name = "main";
    mainFunc.localsSize = 8; // retSlot(0), temp(4)
    mainFunc.paramCount = 0;
    mainFunc.returnTypeKind = 1;

    BytecodeEmitter mainEm;
    mainEm.Emit(OpCode::OP_CallFunc); mainEm.EmitUint16(0); mainEm.EmitUint16(0);
    mainEm.Emit(OpCode::OP_Assign); mainEm.EmitUint16(0);
    mainEm.Emit(OpCode::OP_VarLocal); mainEm.EmitUint16(0);
    mainEm.Emit(OpCode::OP_Return);
    mainFunc.bytecode = mainEm.TakeBytes();

    mod.functions.push_back(std::move(helperFunc));
    mod.functions.push_back(std::move(mainFunc));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 99, "expected 99 from helper()");

    PASS();
}

void test_vm_function_call_with_params() {
    TEST(vm_function_call_with_params);
    // add(a, b) { return a + b; }
    // main() { return add(3, 4); }
    // Calling convention: caller evaluates params to call_param_base area,
    // OP_CallFunc takes func_index and call_param_base; VM copies params
    // from caller locals[call_param_base..] to callee locals[0..].
    CompiledModule mod;
    mod.name = "test";

    // add function: params a(0), b(4), retSlot(8), temp(12)
    CompiledFunction addFunc;
    addFunc.name = "add";
    addFunc.localsSize = 16;
    addFunc.paramCount = 2;
    addFunc.returnTypeKind = 1;

    BytecodeEmitter addEm;
    // retSlot = a; retSlot += b
    addEm.Emit(OpCode::OP_VarLocal); addEm.EmitUint16(0);
    addEm.Emit(OpCode::OP_Assign); addEm.EmitUint16(8);
    addEm.Emit(OpCode::OP_Add_i32); addEm.EmitUint16(8); addEm.EmitUint16(4);
    addEm.Emit(OpCode::OP_VarLocal); addEm.EmitUint16(8);
    addEm.Emit(OpCode::OP_Return);
    addFunc.bytecode = addEm.TakeBytes();

    // main function: call_param_base at 4 (after retSlot 0)
    // locals: retSlot(0), callParamArea(4..35)
    CompiledFunction mainFunc;
    mainFunc.name = "main";
    mainFunc.localsSize = 36;
    mainFunc.paramCount = 0;
    mainFunc.returnTypeKind = 1;

    const uint16_t CPB = 4; // call param base
    BytecodeEmitter mainEm;
    // Evaluate param 0: const 3 -> CPB+0
    mainEm.Emit(OpCode::OP_ConstInt32); mainEm.EmitInt32(3);
    mainEm.Emit(OpCode::OP_Assign); mainEm.EmitUint16(CPB + 0);
    // Evaluate param 1: const 4 -> CPB+4
    mainEm.Emit(OpCode::OP_ConstInt32); mainEm.EmitInt32(4);
    mainEm.Emit(OpCode::OP_Assign); mainEm.EmitUint16(CPB + 4);
    // Call add (index 0) with call_param_base=CPB
    mainEm.Emit(OpCode::OP_CallFunc); mainEm.EmitUint16(0); mainEm.EmitUint16(CPB);
    // Result -> retSlot (offset 0)
    mainEm.Emit(OpCode::OP_Assign); mainEm.EmitUint16(0);
    mainEm.Emit(OpCode::OP_ParaEnd);
    // Return retSlot
    mainEm.Emit(OpCode::OP_VarLocal); mainEm.EmitUint16(0);
    mainEm.Emit(OpCode::OP_Return);
    mainFunc.bytecode = mainEm.TakeBytes();

    mod.functions.push_back(std::move(addFunc));   // index 0 = add
    mod.functions.push_back(std::move(mainFunc));  // index 1 = main

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 7, "expected 7 from add(3,4)");

    PASS();
}

void test_vm_neg_i32() {
    TEST(vm_neg_i32);
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 8; // a(0), ret(4)
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 42
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(42);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    // a = -a
    em.Emit(OpCode::OP_Neg_i32); em.EmitUint16(0);
    // return a
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(4);
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(4);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == -42, "expected -42");

    PASS();
}

void test_vm_mod_i32() {
    TEST(vm_mod_i32);
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 12; // a(0), b(4), ret(8)
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 17, b = 5
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(17);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(5);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(4);
    // a %= b
    em.Emit(OpCode::OP_Mod_i32); em.EmitUint16(0); em.EmitUint16(4);
    // return a
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(8);
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(8);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 2, "expected 2 (17 % 5)");

    PASS();
}

void test_vm_equal_notequal() {
    TEST(vm_equal_notequal);
    // OP_Equal_i32 writes result to locals[lhs], not pResult.
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 16; // a(0), b(4), c(8), ret(12)
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 10, b = 10
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(10);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    em.Emit(OpCode::OP_ConstInt32); em.EmitInt32(10);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(4);
    // copy a to c, then Equal_i32 compares c == b, writes result to c
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(8);
    em.Emit(OpCode::OP_Equal_i32); em.EmitUint16(8); em.EmitUint16(4);
    // return c
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(8);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(12);
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(12);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 1, "expected 1 (10 == 10)");

    PASS();
}

void test_vm_const_zero() {
    TEST(vm_const_zero);
    CompiledModule mod;
    mod.name = "test";

    CompiledFunction func;
    func.name = "main";
    func.localsSize = 8; // a(0), ret(4)
    func.paramCount = 0;
    func.returnTypeKind = 1;

    BytecodeEmitter em;
    // a = 0
    em.Emit(OpCode::OP_ConstZero);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(0);
    // return a
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(0);
    em.Emit(OpCode::OP_Assign); em.EmitUint16(4);
    em.Emit(OpCode::OP_VarLocal); em.EmitUint16(4);
    em.Emit(OpCode::OP_Return);

    func.bytecode = em.TakeBytes();
    mod.functions.push_back(std::move(func));

    VmExecutor exec;
    int result = exec.Execute(mod);
    CHECK(result == 0, "expected 0");

    PASS();
}

// --- Module save/load round-trip ---

void test_module_save_load() {
    TEST(module_save_load);
    // Build a module, save to temp file, load back, verify
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

    // Save
    std::string tmpPath = std::filesystem::temp_directory_path().string() + "/nlang_test_mod.nmod";
    {
        std::ofstream fs(tmpPath, std::ios::binary);
        const char magic[] = "NLANGMOD";
        fs.write(magic, 8);
        uint16_t majorVer = 1, minorVer = 0;
        fs.write(reinterpret_cast<const char*>(&majorVer), sizeof(majorVer));
        fs.write(reinterpret_cast<const char*>(&minorVer), sizeof(minorVer));
        uint32_t nameLen = static_cast<uint32_t>(mod.name.size());
        fs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        fs.write(mod.name.c_str(), nameLen);
        uint32_t strCount = static_cast<uint32_t>(mod.stringConstants.size());
        fs.write(reinterpret_cast<const char*>(&strCount), sizeof(strCount));
        for (auto& s : mod.stringConstants) {
            uint32_t len = static_cast<uint32_t>(s.size());
            fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
            fs.write(s.c_str(), len);
        }
        uint32_t funcCount = static_cast<uint32_t>(mod.functions.size());
        fs.write(reinterpret_cast<const char*>(&funcCount), sizeof(funcCount));
        for (auto& f : mod.functions) {
            uint32_t fnameLen = static_cast<uint32_t>(f.name.size());
            fs.write(reinterpret_cast<const char*>(&fnameLen), sizeof(fnameLen));
            fs.write(f.name.c_str(), fnameLen);
            fs.write(reinterpret_cast<const char*>(&f.localsSize), sizeof(f.localsSize));
            fs.write(reinterpret_cast<const char*>(&f.paramCount), sizeof(f.paramCount));
            fs.write(reinterpret_cast<const char*>(&f.returnTypeKind), sizeof(f.returnTypeKind));
            uint32_t bcSize = static_cast<uint32_t>(f.bytecode.size());
            fs.write(reinterpret_cast<const char*>(&bcSize), sizeof(bcSize));
            if (bcSize > 0)
                fs.write(reinterpret_cast<const char*>(f.bytecode.data()), bcSize);
        }
        fs.close();
    }

    // Load
    auto loaded = ModuleLoader::Load(tmpPath);
    CHECK(loaded.name == "test_mod", "module name mismatch");
    CHECK(loaded.stringConstants.size() == 2, "string count mismatch");
    CHECK(loaded.stringConstants[0] == "hello", "string[0] mismatch");
    CHECK(loaded.stringConstants[1] == "world", "string[1] mismatch");
    CHECK(loaded.functions.size() == 1, "function count mismatch");
    CHECK(loaded.functions[0].name == "main", "function name mismatch");
    CHECK(loaded.functions[0].localsSize == 4, "localsSize mismatch");
    CHECK(loaded.functions[0].bytecode.size() == mod.functions[0].bytecode.size(), "bytecode size mismatch");

    // Execute loaded module
    VmExecutor exec;
    int result = exec.Execute(loaded);
    CHECK(result == 7, "expected 7 from loaded module");

    // Cleanup
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
    std::cout << "=== NLang VM Unit Tests ===\n\n";

    // BytecodeEmitter/Reader
    test_emitter_reader_roundtrip();
    test_patch_uint16();
    test_string_constant();

    // CompiledModule
    test_find_function();

    // VmExecutor
    test_vm_const_int32_return();
    test_vm_const_zero();
    test_vm_add_i32();
    test_vm_sub_mul_div();
    test_vm_neg_i32();
    test_vm_mod_i32();
    test_vm_comparison();
    test_vm_equal_notequal();
    test_vm_function_call();
    test_vm_function_call_with_params();

    // Module save/load
    test_module_save_load();

    std::cout << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
