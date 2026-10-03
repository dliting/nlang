// --- ndb debugger tests: machine-mode seam + line protocol ---
// Host I/O seam (output capture + readLine rejection), the ndb
// --machine line protocol, and loop per-iteration anchors.
#include "test_debugger_common.h"

using namespace nlang;

// --- Machine-mode debugging: host I/O seam ---

//Captures what io.print emits. IsInputAvailable() keeps the interface
//default (false): output-capable only.
class CapturingHostIo : public IHostIo
{
public:
    std::string captured;
    void OnOutput(std::string_view text) override { captured.append(text); }
};

void test_hostio_output_capture()
{
    //Output bytes arrive verbatim, including the '\n' io.print appends.
    TEST(hostio_output_capture);
    BuildOutcome b = buildSource("hostio_print",
        "import io;\n"
        "int main() { io.print(\"hi\"); return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    //io joins at load time: the entry-unit artifact carries only the
    //import slot, so the closure (stdlib.npkg via STDLIB_DIR) must be
    //linked before execution.
    CompiledModule mod = loadLinked("hostio_print");
    VmExecutor exec;
    CapturingHostIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(io.captured == "hi\n", "expected \"hi\\n\", got: " + io.captured);
    PASS();
}

void test_hostio_readline_rejected()
{
    //A host without input must make io.readLine raise IOException
    //(uncaught here -> NLangThrow escapes Execute) instead of silently
    //consuming the embedder's stream.
    TEST(hostio_readline_rejected);
    BuildOutcome b = buildSource("hostio_readline",
        "import io;\n"
        "int main() { string s = io.readLine(); return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    //Same load-time closure as test_hostio_output_capture: io's unit
    //rides in from stdlib.npkg, linked here.
    CompiledModule mod = loadLinked("hostio_readline");
    VmExecutor exec;
    CapturingHostIo io;
    exec.SetHostIo(&io);
    std::string raiseText;
    bool raised = false;
    try {
        exec.Execute(mod);
    } catch (const NLangThrow& ex) {
        raised = true;
        raiseText = ex.what();
    }
    CHECK(raised, "readLine must raise IOException when input is unavailable");
    //Identity pin: the raise must come from the io.readLine site itself,
    //not some other channel that happens to throw.
    CHECK(raiseText.rfind("io.readLine", 0) == 0,
        "raise must originate at io.readLine, got: " + raiseText);
    PASS();
}
// --- Machine-mode line protocol (ndb --machine) ---

void test_protocol_escape_roundtrip()
{
    //Escape round-trip: the framing metacharacters must survive.
    TEST(protocol_escape_roundtrip);
    const std::string tricky = "a\\b\tc\nd\r";
    const std::string encoded = protocol::EncodeField(tricky);
    CHECK(encoded == "a\\\\b\\tc\\nd\\r", "encode");
    CHECK(protocol::DecodeField(encoded) == tricky, "decode");
    //The decoder stays total over arbitrary input: unknown escapes and a
    //trailing lone backslash pass through verbatim.
    CHECK(protocol::DecodeField("\\x") == "\\x", "unknown escape");
    CHECK(protocol::DecodeField("a\\") == "a\\", "trailing backslash");
    PASS();
}

//Drives MachineFrontEnd in-process (commands from a stringstream, event
//lines from an ostringstream); the embedder sequence mirrors the tool's
//RunMachine: wire, PumpUntilRun, Execute, OnExited. The script must run
//the program to completion — EOF while frozen would quit the whole test
//binary (EOF = session quit by contract; dbg_quit_eof pins that in a
//subprocess instead).
void test_machine_session_roundtrip()
{
    TEST(machine_session_roundtrip);
    BuildOutcome b = buildSource("mach_basic",
        "int main() {\n"               //1
        "    int total = 0;\n"         //2
        "    int i = 7;\n"             //3
        "    total = total + i * 6;\n" //4
        "    return total;\n"          //5
        "}\n");                        //6
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("mach_basic");
    const std::string src = (scratchDir() / "mach_basic.n").string();
    const std::string escaped = protocol::EncodeField(src);
    std::ostringstream events;
    std::istringstream in(
        "wat\n"                 //unknown command -> err
        "locals\n"              //window-bound before run -> err
        "b " + src + " 99\n"    //past EOF -> unbound receipt
        "b " + src + " 4\n"     //binds the third statement
        "run\n"                 //ends the prelude; initial stop follows
        "bt\n"                  //read at the initial stop (line 2)
        "c\n"                   //resume; the line-4 breakpoint hits
        "locals\n"              //total/i assigned, line 4 not yet run
        "c\n");                 //resume to completion
    MachineFrontEnd front(mod, in, events);
    DebugSessionController controller(mod, front);
    front.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    front.PumpUntilRun();
    //Embedder contract (RunMachine): session-end events are fired by the
    //embedder around Execute, not by the controller.
    front.OnExited(exec.Execute(mod));

    //Every event is tab-joined with all fields encoded; data tabs are
    //escaped, so a raw tab in the wire is always a field separator.
    const std::string wire = events.str();
    CHECK(wire.find("hello\t1\n") == 0, "hello is the first event");
    CHECK(wire.find("err\tunknown command 'wat'\n") != std::string::npos,
        "unknown commands answer err");
    CHECK(wire.find("err\tView outside the frozen window")
            != std::string::npos,
        "window-bound commands err before run");
    CHECK(wire.find("bp\t0\t" + escaped + "\t99\tunbound\n")
            != std::string::npos,
        "unbound requests report id 0");
    CHECK(wire.find("bp\t1\t" + escaped + "\t4\tbound\n")
            != std::string::npos,
        "bound requests report the receipt");
    CHECK(wire.find("stopped\tinitial\t0\tmach_basic.main\t" + escaped
            + "\t2\t1\t1\n") != std::string::npos,
        "initial stop event (tab-joined fields)");
    CHECK(wire.find("frame\t0\tmach_basic.main\t" + escaped + "\t2\n")
            != std::string::npos,
        "bt frame 0 anchors at the initial stop");
    CHECK(wire.find("done\tbt\n") != std::string::npos,
        "bt terminates with done");
    CHECK(wire.find("stopped\tbreakpoint\t1\tmach_basic.main\t" + escaped
            + "\t4\t1\t1\n") != std::string::npos,
        "breakpoint stop carries the id");
    CHECK(wire.find("local\ttotal\tint\t0\n") != std::string::npos,
        "locals render tab-joined name/type/value");
    CHECK(wire.find("local\ti\tint\t7\n") != std::string::npos,
        "second local");
    CHECK(wire.find("done\tlocals\n") != std::string::npos,
        "locals terminate with done");
    CHECK(wire.find("exited\t42\n") != std::string::npos,
        "exited event carries the program's code");
    PASS();
}

//Machine-face coverage the session roundtrip does not reach: bfunc's
//receipt (the mirror of the controller's binding scan in DoBreakFunction)
//and output events (the IHostIo wiring). io.print fires TWO OnOutput
//calls — text, then the newline — so the wire shows two output lines.
void test_machine_bfunc_and_output()
{
    TEST(machine_bfunc_and_output);
    BuildOutcome b = buildSource("mach_bfunc",
        "import io;\n"                 //1
        "\n"                           //2
        "int helper() {\n"             //3
        "    return 3;\n"              //4
        "}\n"                          //5
        "\n"                           //6
        "int main() {\n"               //7
        "    io.print(\"a\\tb\");\n"   //8
        "    return helper() + 4;\n"   //9
        "}\n");                        //10
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    //io joins the closure at load time (stdlib.npkg via STDLIB_DIR), so
    //the session runs the linked module, not the bare entry unit.
    CompiledModule mod = loadLinked("mach_bfunc");
    const std::string src = (scratchDir() / "mach_bfunc.n").string();
    const std::string escaped = protocol::EncodeField(src);
    std::ostringstream events;
    std::istringstream in(
        "bfunc mach_bfunc.helper\n"   //binds helper's first anchor (line 4)
        "bfunc nosuch\n"        //no such function -> id 0, empty location
        "run\n"                 //initial stop at main line 8
        "c\n"                   //output fires, then the helper bp hits
        "c\n");                 //resume to completion
    MachineFrontEnd front(mod, in, events);
    DebugSessionController controller(mod, front);
    front.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetHostIo(&front);   //the embedder wiring the tool's RunMachine does
    front.PumpUntilRun();
    front.OnExited(exec.Execute(mod));

    const std::string wire = events.str();
    CHECK(wire.find("bp\t1\t" + escaped + "\t4\tbound\n")
            != std::string::npos,
        "bfunc resolves the function's first anchor");
    CHECK(wire.find("bp\t0\t\t0\tunbound\n") != std::string::npos,
        "unknown bfunc reports id 0 with an empty location");
    CHECK(wire.find("stopped\tinitial\t0\tmach_bfunc.main\t" + escaped
            + "\t8\t1\t1\n") != std::string::npos,
        "initial stop anchors main's first statement");
    CHECK(wire.find("output\ta\\tb\n") != std::string::npos,
        "print text arrives as one escaped output event");
    CHECK(wire.find("output\t\\n\n") != std::string::npos,
        "print's newline is a second output event");
    CHECK(wire.find("output\ta\\tb\n") < wire.find("exited\t7\n"),
        "output events stream before the exit event");
    CHECK(wire.find("stopped\tbreakpoint\t1\tmach_bfunc.helper\t" + escaped
            + "\t4\t2\t2\n") != std::string::npos,
        "the bfunc breakpoint hits inside helper (depth is 1-based: "
        "2 frames total)");
    CHECK(wire.find("exited\t7\n") != std::string::npos,
        "session ends with the program's exit code");
    PASS();
}

//Session discipline over a two-frame freeze: frame selection routes
//locals, a deleted breakpoint's id is not reused, stepping reports the
//step reason, and a mis-timed `run` errs without derailing the session.
//Also pins human-written-script tolerance: `b` splits file/line at the
//LAST space and trims the file part, so a double space still binds.
void test_machine_frame_and_discipline()
{
    TEST(machine_frame_and_discipline);
    BuildOutcome b = buildSource("mach_frame",
        "int helper(int v) {\n"    //1
        "    return v + 1;\n"      //2
        "}\n"                      //3
        "\n"                       //4
        "int main() {\n"           //5
        "    int a = 5;\n"         //6
        "    int r = helper(a);\n" //7
        "    return r * 3;\n"      //8
        "}\n");                    //9
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("mach_frame");
    const std::string src = (scratchDir() / "mach_frame.n").string();
    const std::string escaped = protocol::EncodeField(src);
    std::ostringstream events;
    std::istringstream in(
        "bfunc mach_frame.helper\n"   //bp 1 binds helper's first anchor (line 2)
        "run\n"                 //initial stop at main line 6
        "c\n"                   //the helper bp hits -> frozen at 2 frames
        "frame 1\n"             //select main's frame
        "locals\n"              //frame 1's locals, not helper's v
        "d 1\n"                 //delete the bound bp
        "b " + src + "  2\n"    //double space; re-add at the same line
        "s\n"                   //step out of the helper
        "run\n"                 //mis-timed run -> err, session survives
        "c\n");                 //resume to completion
    MachineFrontEnd front(mod, in, events);
    DebugSessionController controller(mod, front);
    front.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetHostIo(&front);
    front.PumpUntilRun();
    front.OnExited(exec.Execute(mod));

    const std::string wire = events.str();
    CHECK(wire.find("stopped\tbreakpoint\t1\tmach_frame.helper\t" + escaped
            + "\t2\t2\t2\n") != std::string::npos,
        "the bfunc bp freezes inside helper (depth == frameCount == 2)");
    CHECK(wire.find("frame\t1\tmach_frame.main\t" + escaped + "\t7\n")
            != std::string::npos,
        "frame 1 is main, still at the call line");
    CHECK(wire.find("local\ta\tint\t5\n") != std::string::npos,
        "locals follow the frame selection (main's a)");
    CHECK(wire.find("local\tv\t") == std::string::npos,
        "helper's v is not rendered for the selected frame");
    CHECK(wire.find("done\td\n") != std::string::npos,
        "deleting a bound bp answers done");
    CHECK(wire.find("bp\t2\t" + escaped + "\t2\tbound\n")
            != std::string::npos,
        "re-adding binds with a fresh id and a trimmed file part");
    CHECK(wire.find("stopped\tstep\t0\tmach_frame.main\t" + escaped + "\t8\t1\t1\n")
            != std::string::npos,
        "stepping out of helper reports the step and drops to 1 frame");
    CHECK(wire.find("err\trun is only valid before the program starts\n")
            != std::string::npos,
        "run errs inside the frozen window");
    CHECK(wire.find("err\trun is only valid before the program starts\n")
            < wire.find("exited\t18\n"),
        "the session stays sane: commands after the err still work");
    CHECK(wire.find("exited\t18\n") != std::string::npos,
        "session ends with the program's exit code");
    PASS();
}
// --- Loop per-iteration anchors (VmBackend) ---

//Loop anchors must fire every iteration (the back edge has to land on
//the condition-entry anchor). The sentinel C++ exception escapes
//Execute cleanly (pinned by hooks_cpp_exception_propagates) — that is
//the in-process "halt" for an otherwise-infinite loop.
struct StopCountingHooks : IDebugHooks
{
    int stops = 0;
    std::vector<uint16_t> lines;
    void OnStatement(const DebugStopInfo& info, IVmDebugView&) override
    {
        ++stops;
        lines.push_back(info.line);
        if (stops >= 3)
            throw std::runtime_error("sentinel");
    }
    void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
};

void test_loop_anchor_per_iteration()
{
    //spin's body is empty, so its while line is the only repeatable
    //anchor. The pinned stop sequence nails the fix exactly: stop 1 is
    //the `spin();` call-statement anchor in main (line 6), then one stop
    //per iteration on the while line (2) — neither more nor fewer. Before
    //the fix the loop anchor fired only at entry and the empty back edge
    //never re-hit it, so no 3rd stop existed and Execute hung instead of
    //raising the sentinel.
    TEST(loop_anchor_per_iteration);
    BuildOutcome b = buildSource("loop_anchor",
        "int spin() {\n"        //1
        "    while (1) {}\n"    //2
        "    return 0;\n"       //3
        "}\n"                   //4
        "int main() {\n"        //5
        "    spin();\n"         //6
        "    return 0;\n"       //7
        "}\n");                 //8
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("loop_anchor");
    VmExecutor exec;
    StopCountingHooks hooks;
    exec.SetDebugHooks(&hooks);
    bool sentinel = false;
    try {
        exec.Execute(mod);
    } catch (const std::runtime_error&) {
        sentinel = true;
    }
    CHECK(sentinel, "3rd stop must raise the sentinel (per-iteration "
          "anchors keep a halt reachable in an empty-body loop)");
    CHECK(hooks.lines.size() == 3, "expected exactly 3 stops, got: "
          + std::to_string(hooks.lines.size()));
    std::string seq;
    for (uint16_t l : hooks.lines) {
        if (!seq.empty()) seq += ", ";
        seq += std::to_string(l);
    }
    CHECK(hooks.lines[0] == 6 && hooks.lines[1] == 2
          && hooks.lines[2] == 2,
          "stop sequence must be {6, 2, 2} (main's spin() call, then the "
          "while line once per iteration), got: {" + seq + "}");
    PASS();
}

void run_debugger_hostio_tests()
{
    test_hostio_output_capture();
    test_hostio_readline_rejected();
}

void run_debugger_machine_tests()
{
    test_protocol_escape_roundtrip();
    test_machine_session_roundtrip();
    test_machine_bfunc_and_output();
    test_machine_frame_and_discipline();
}

void run_debugger_loop_tests()
{
    test_loop_anchor_per_iteration();
}
