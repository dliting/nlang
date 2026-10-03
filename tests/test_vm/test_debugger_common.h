#pragma once
// --- ndb debugger unit tests: shared harness ---
// One include surface for the test_debugger_* TU family (split from
// the original single-TU test_debugger.cpp): the public API pieces
// every scenario drives, the check macros, the shared build/load
// helpers, and each topic TU's runner. In-process compile+run via the
// public ModuleBuilder API (see test_stdlib.cpp for the rationale).
// MUST call Runtime::StaticInit() before any build (IdString tables) —
// the driver's main does it first.
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "IDebugHooks.h"
#include "IHostIo.h"
#include "ModuleLoader.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "Disassembler.h"
#include "DebugSessionController.h"
#include "DebugSession.h"
#include "MachineFrontEnd.h"
#include "SourceCache.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

extern int g_pass, g_fail;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)
//Like CHECK (aborts the current test function) but the message carries
//the condition text — for shape preconditions whose failure would make
//subsequent indexed assertions out-of-bounds.
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

struct BuildOutcome
{
    bool ok = false;
    std::string diagnostics;
};

//Scratch directory for temp sources and modules.
std::filesystem::path scratchDir();

//Write <tag>.n with the raw source (no stdlib imports prepended —
//debugger tests target plain programs), build <tag>.ncu.
BuildOutcome buildSource(const std::string& tag, const std::string& source);

//Build a consumer TU from raw source with the scratch dir on the
//import path (shared by the import-shaped tests; the imported lib must
//already exist as a compiled .ncu — see the FindModuleFile note in
//test_v19_import_roundtrip).
BuildOutcome buildConsumer(const std::string& tag, const std::string& source);

//加载刚构建的 <tag>.ncu。
nlang::CompiledModule loadBuilt(const std::string& tag);

//加载并链接一个带导入的刚构建产物：闭包从 scratch 目录与 stdlib 包
//目录解析（外部 .ncu 与产物同目录；io/math/fs 单元在 stdlib.npkg 里），
//nlink 合并为唯一运行期模块。加载期链接是导入形态的唯一执行路径
//（产物自身不含库代码）。
nlang::CompiledModule loadLinked(const std::string& tag);

//Per-topic runners (one per test_debugger_*.cpp); the driver calls
//them in this order — the original single-TU execution sequence.
void run_debugger_format_tests();
void run_debugger_gc_tests();
void run_debugger_hooks_tests();
void run_debugger_linepc_tests();
void run_debugger_session_tests();
void run_debugger_controller_tests();
void run_debugger_sourcecache_tests();
void run_debugger_hostio_tests();
void run_debugger_machine_tests();
void run_debugger_loop_tests();
