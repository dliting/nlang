// --- ndb debugger unit tests: driver ---
// Runs every topic suite in the historical single-TU order; the topic
// TUs live in test_debugger_*.cpp over the shared harness in
// test_debugger_common.{h,cpp}.
#include "test_debugger_common.h"

using namespace nlang;

int main()
{
    //In-process host init: the ModuleBuilder build path dereferences the
    //IdString static tables — StaticInit must run first or the build
    //segfaults (a crash try/catch cannot intercept).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)

    run_debugger_format_tests();
    run_debugger_gc_tests();
    run_debugger_hooks_tests();
    run_debugger_linepc_tests();
    run_debugger_session_tests();
    run_debugger_controller_tests();
    run_debugger_sourcecache_tests();
    run_debugger_hostio_tests();
    run_debugger_machine_tests();
    run_debugger_loop_tests();

    std::cerr << "\ndebugger_tests: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail > 0 ? 1 : 0;
}
