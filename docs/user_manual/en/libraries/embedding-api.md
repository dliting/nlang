# Embedding NLang

Besides the subprocess driving described in [Integrating NLang](integrating-nlang.md), NLang offers an in-process host application programming interface (API): a C++ host links the `nlang_embed` library, loads and executes compiled artifacts inside its own process, and exchanges typed values with scripts. This page covers the minimal host, the lifecycle, host functions, value exchange, error handling and output redirection; a complete runnable example ships in `examples\embed_host\` inside the install directory. For a first overview of the subprocess style, see [Embedding NLang in Other Programs](../getting-started/embedding-nlang.md).

## 1. Dependencies and build

- Header: `include\nlang\embed\NLang.h` (add the install directory's `include\` to the compiler's search path).
- Library: `nlang_embed` (static; pulls in `nlang_vm`, `nlang_runtime` and the compiler front end).
- When scripts use native standard-library namespaces, the matching `nlang_*.dll` files can sit next to the host executable or in one of the `addImportDir` directories (search order in section 3).

With CMake:

```cmake
add_executable(my_host main.cpp)
target_link_libraries(my_host PRIVATE nlang_embed)
```

## 2. A minimal host

```cpp
#include "nlang/embed/NLang.h"
#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: my_host <program.ncu|.npkg>\n");
        return 2;
    }
    nlang::initialize();
    nlang::Interpreter itp;
    try {
        itp.load(argv[1]);
        return itp.run();
    } catch (const nlang::Exception& e) {
        std::fprintf(stderr, "script error: %s\n", e.message().c_str());
        return 1;
    } catch (const nlang::LoadError& e) {
        std::fprintf(stderr, "load error: %s\n", e.what());
        return 1;
    }
}
```

Compile the script with `ncc` into a `.ncu` (or pack it into a `.npkg`) first, then load and run it from the host; `run()` returns the script `main`'s exit code.

## 3. Running source directly (ad-hoc scripts)

Besides loading compiled artifacts, the interpreter can run source directly: `runSource` takes a source string, `runFile` a script file path; both compile and execute in-process and return the exit code (the value of a top-level `return`; 0 without one). Scripts skip the `main()` boilerplate — top-level statements are wrapped into a synthesized entry, the same semantics as the `nlang` command-line tool.

```cpp
nlang::Interpreter itp;
itp.addImportDir("path/to/stdlib");   // no implicit stdlib discovery
itp.setOutputHandler(
    [](const char* s) { std::printf("%s", s); }, nullptr);
int rc = itp.runSource("import io; io.print(6*7);");   // rc == 0
```

To `import io`, add the standard-library directory to the import path first: library declarations and packages resolve through the directories you pass to `addImportDir` — plus, for `runFile`, the script's own directory, which joins the search automatically (in-process embedding reads no `NLANG_PATH` and searches neither the host executable's directory nor the current directory). Native modules (`nlang_io.dll`, ...) follow the import directories too, but the executable's own directory always sits first in the native search list — a stale DLL left there silently shadows the copy in your import directory. The typical deployment places the whole stdlib directory (`.n` declarations, `stdlib.npkg`, and native dynamic-link library (DLL) files) under your application data directory and points `addImportDir` at it once at startup.

After execution you can still `call` functions the script declared (keys are `<module>.<function>`): the module name for string sources is fixed as `script`, and `runFile` uses the file-name stem — which must be a valid identifier, so hyphenated names like `my-script.n` are rejected.

```cpp
nlang::Interpreter itp;   // each instance defines its source once
(void)itp.runSource("int getTimeout() { return 30; } return 0;");
nlang::Value t = itp.call("script.getTimeout", {});   // 30
```

Each `Interpreter` defines its program source once (`load`/`runSource`/`runFile` are mutually exclusive); a failed compile does not consume that chance — fix the source and retry. To run another script, create another `Interpreter`; instances share no state. Register host functions and install output redirection before `runSource`/`runFile`.

For one-shot execution there is also a pair of convenience functions:

```cpp
nlang::ScriptOptions opts;
opts.importDirs.push_back("path/to/stdlib");
int rc = nlang::runScript("import io; io.print(\"hi\");", opts);
```

`runScript`/`runScriptFile` build an interpreter, run, and destroy it, keeping no state; use the full `Interpreter` flow when you need host functions or post-run `call`. Keep top-level `return` values within 0-255: the process exit code travels as the full 32-bit value, but POSIX shells observe only the low 8 bits (see the [exit code convention](../language-spec/exit-code-convention.md)).

## 4. Lifecycle

| Call | Count and timing | Notes |
|---|---|---|
| `initialize()` | at least once per process, idempotent | initializes the runtime; `shutdown()` auto-registers via `atexit`, hosts rarely call it explicitly |
| `addImportDir(d)` | any number of times, all before defining the program source | appends link-time `import` search directories (for example the `stdlib` directory) |
| `load(artifact)` | exactly once per instance; one of `load()`/`runSource()`/`runFile()` | loads a `.ncu`/`.npkg` and resolves its closure; failures throw `LoadError` |
| `runSource(src)`/`runFile(path)` | exactly once per instance; one of `load()`/`runSource()`/`runFile()` | compiles and executes source in-process (see section 3); failures throw `LoadError`, a failed compile does not consume the source |
| `run()` | at most once | calls the script `main` and returns its exit code; uncaught script exceptions throw `Exception` |
| `call(name, args)` | any number of times (after the source is defined, before or after `run()`) | calls a module-level function by its `unit-stem.function` key |

Re-entering one `Interpreter` instance is not allowed: calling its `run()`/`call()` again from inside a host-function callback throws `BadValue`.

### Concurrency and suspension

`run()` executes through the scheduler carrier: `thread.sleep` wakes on its deadline, contexts spawned by `thread.spawn` get scheduled, and `thread.join` parks and waits — exactly as under the `nvm` command line. When `main` parks with no runnable context left (an empty-channel `recv` with no sender, for example), `run()` throws a catchable `std::runtime_error` (the message contains `deadlock`) instead of hanging.

`run()` executes under a global interpreter lock (GIL): exactly one virtual thread interprets bytecode at a time. An `Interpreter` drives the scheduler with a single carrier — the library default — so contexts make progress only at suspension points. Hosts that embed at the `VmExecutor` layer can opt into parallel carriers by calling `SetCarrierThreadCount(n)` before `Execute`; a native call registered as blocking under version 5 of the native application binary interface (ABI) then runs without the GIL, and the other carriers execute other virtual threads in that window. Host callbacks may then run concurrently with bytecode on other carriers and with one another, so shared host state needs its own synchronization. An executor still runs at most one `Execute` at a time; concurrent `CallByName` calls from several host threads each acquire the GIL and therefore run one after another. Drive one `Interpreter` instance from one host thread at a time — its re-entry guard is a misuse check, not a concurrency primitive.

Host functions registered through `registerHostFunction` always run through the blocking-native protocol: the VM stages their arguments, releases the GIL for the duration of the callback, and reacquires it before resuming. The staged argument cells and the return slot stay rooted across that gap, so a collection on another carrier cannot reclaim handles in flight; VM callbacks made from inside such a call re-acquire the GIL per callback.

`call()` is a host-initiated nested evaluation window without a carrier turn of its own:

- Intrinsics that would park are rejected with a `std::runtime_error` (message contains `cannot suspend`): `thread.yield`, `thread.sleep`, `thread.join` on an unfinished target, `Future.get` on an unfinished target, a contended `Mutex.lock`, `Channel.recv` on an empty open channel, and `send` on a full bounded channel.
- `thread.spawn` is likewise rejected (message contains `spawn`) — no carrier exists to schedule the new context; host flows that need concurrency should enter through `run()`.
- Non-parking forms work as usual: `Mutex.tryLock`, `Future.isDone`, `send` on a non-full channel, a buffered `recv`, `join` on a finished target, and `Future.get` on a finished target.

## 5. Host functions

Scripts declare external functions as `native`; hosts register the implementation:

```cpp
itp.registerHostFunction("host", "now",
    [](const std::vector<nlang::Value>& args) {
        return nlang::Value(int32_t(42));
    });
```

```nlang
//the file must be named host.n — the declaring unit's stem is the key's first half
native int now();

int main() {
    return 100 - now();   //58
}
```

Key rule: the namespace argument of `registerHostFunction` must equal the declaring file's stem (`host` above). Arguments reach the callback marshalled per the script-side declaration (a `string` formal arrives as `Kind::String`); a C++ exception thrown by the callback becomes a catchable `Exception` inside the script, and a script `Exception` rethrown from a callback keeps its original instance.

## 6. Value exchange

`nlang::Value` is classified by `kind()`: `Null`, `Int`, `Long`, `Float`, `Double`, `Bool`, `Char`, `String`, and the reference kinds `Array`, `List`, `Dict`, `Object`, `Struct`, `Func`.

- Scalars: `asInt()`, `asString()` and friends; a kind mismatch throws `BadValue`. Narrow integer types (byte, short, ...) fold to `Int`/`Long`; enums fold to `Int`.
- Strings: value semantics — each side holds its own copy; changes do not cross.
- Reference kinds (containers and objects): `asList()`, `asDict()`, `asArray()`, `asObject()` return read/write proxies; host edits are immediately visible to the script and vice versa. Container elements and object fields are validated against the script-side declared types; mismatches throw `BadValue`.
- Building containers on the host: `newList()`/`newDict()` return builder values staging elements host-side; every crossing into a script call materializes a fresh container instance (no caching), with elements validated against the formal's declared element types.
- Memory safety: reference values held by the host are registered as garbage collection (GC) roots, so collections the script triggers never reclaim objects the host still uses. Proxies and reference values are valid only while their `Interpreter` lives.

```cpp
nlang::Value v = itp.call("mymod.makeList", {});
nlang::ListProxy list = v.asList();
list.set(0, nlang::Value(int32_t(9)));       //immediately visible to the script
nlang::Value sum = itp.call("mymod.sumList", {v});
```

## 7. Error handling

| Exception class | Meaning | Suggested handling |
|---|---|---|
| `nlang::Exception` | an uncaught script exception (script-side failure) | catch at the host boundary; read `message()`, `backtrace()`, `exceptionClass()` |
| `nlang::BadValue` | host misuse: kind mismatches, index out of bounds, re-entry, lifetime errors | a host bug — fix the host code rather than catching around it |
| `nlang::LoadError` | load and environment failures: bad path, bad artifact, missing native module | catch and report the environment problem to the user |
| `std::runtime_error` | VM-internal errors and concurrency boundaries: deadlock reports, suspension or `thread.spawn` rejections inside a nested window | catch at the host boundary and report |

## 8. Input and output redirection

`setOutputHandler(out, err)` forwards the script's `io.print` and `io.eprint` to two separate callbacks (text arrives as Unicode Transformation Format (UTF-8) bytes); passing two empty callbacks uninstalls the handlers and returns to the real standard streams. Callbacks fire on the execution thread, run while holding the GIL (keep them short), and must not throw. The input side defaults to no channel: the script's first `io.readLine` raises a catchable `IOException`; host-injected input is future work.

```cpp
itp.setOutputHandler(
    [](const char* text) { std::fprintf(stdout, "[nlang] %s", text); },
    [](const char* text) { std::fprintf(stderr, "[nlang-err] %s", text); });
```
