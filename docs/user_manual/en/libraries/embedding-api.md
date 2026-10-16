# Embedding NLang

Besides the subprocess driving described in [Integrating NLang](integrating-nlang.md), NLang offers an in-process host application programming interface (API): a C++ host links the `nlang_embed` library, loads and executes compiled artifacts inside its own process, and exchanges typed values with scripts. This page covers the minimal host, the lifecycle, host functions, value exchange, error handling and output redirection; a complete runnable example ships in `examples\embed_host\` inside the install directory. For a first overview of the subprocess style, see [Embedding NLang in Other Programs](../getting-started/embedding-nlang.md).

## 1. Dependencies and build

- Header: `include\nlang\embed\NLang.h` (add the install directory's `include\` to the compiler's search path).
- Library: `nlang_embed` (static; pulls in `nlang_vm`, `nlang_runtime` and the compiler front end).
- When scripts use native standard-library namespaces, the matching `nlang_*.dll` files must sit next to the host executable (the loader's search rule matches `nvm`, see [Integrating NLang](integrating-nlang.md)).

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

To `import io`, add the standard-library directory to the import path first: in-process embedding does not read `NLANG_PATH` and does not search the host executable's directory or the current directory. Library declarations, packages, and native modules (`nlang_io.dll`, ...) all resolve from the directories you pass to `addImportDir`. The typical deployment places the whole stdlib directory (`.n` declarations, `stdlib.npkg`, and native dynamic-link library (DLL) files) under your application data directory and points `addImportDir` at it once at startup.

After execution you can still `call` functions the script declared (keys are `<module>.<function>`): the module name for string sources is fixed as `script`, and `runFile` uses the file-name stem — which must be a valid identifier, so hyphenated names like `my-script.n` are rejected.

```cpp
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

`runScript`/`runScriptFile` build an interpreter, run, and destroy it, keeping no state; use the full `Interpreter` flow when you need host functions or post-run `call`. Keep top-level `return` values within 0-255 (Windows truncates process exit codes to 8 bits).

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

## 8. Input and output redirection

`setOutputHandler(out, err)` forwards the script's `io.print` and `io.eprint` to two separate callbacks (text arrives as Unicode Transformation Format (UTF-8) bytes); passing two empty callbacks uninstalls the handlers and returns to the real standard streams. Callbacks fire on the execution thread and must not throw. The input side defaults to no channel: the script's first `io.readLine` raises a catchable `IOException`; host-injected input is future work.

```cpp
itp.setOutputHandler(
    [](const char* text) { std::fprintf(stdout, "[nlang] %s", text); },
    [](const char* text) { std::fprintf(stderr, "[nlang-err] %s", text); });
```
