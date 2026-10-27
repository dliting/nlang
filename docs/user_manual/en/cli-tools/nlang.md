# nlang — Run Scripts Directly

nlang runs NLang statements without the `main()` boilerplate, the way a
python interpreter does: hand it a script of top-level statements. You
can run a script file, or put a few statements right on the command line
with `-c` — handy for one-off tasks, teaching examples, and quick
experiments. nlang compiles and executes in process and leaves no
artifact on disk.

## Invocation Forms

| Form | Command | Behavior |
|---|---|---|
| Run a script file | `nlang <script.n> [-I <dir>...]` | Compile and execute in process |
| Run inline statements | `nlang -c "<statements>" [-I <dir>...]` | Statements go to a temp `.n`, run as a script |
| Version query | `nlang --version` | Prints `nlang (NLang) <version>` |

Each invocation runs one thing: a script file or `-c`, never both. Give
both, give neither, or pass an unknown flag, and nlang prints the usage
and exits 1. The `-c` text is written to a temp `.n` file exactly as
given (embedded newlines preserved) and run as a script; diagnostics
carry that file's line numbers. Command-line arguments are decoded from
the wide-character form, so content outside
American Standard Code for Information Interchange (ASCII) — Chinese
strings, for example — works directly.

The quoting in the examples is for cmd and bash. In Windows
PowerShell 5.1, keep the backslashes and switch the outer quotes to
single quotes: `nlang -c 'import io; io.print(\"hello\");'`. In
PowerShell 7, use single quotes and drop the backslashes:
`nlang -c 'import io; io.print("hello");'`. Or skip quoting entirely:
put the statements in a `.n` file and run `nlang <file>`.

## Flags

| Flag | Description |
|---|---|
| `-c "<statements>"` | Inline statements — executed as a script; the exit code is the top-level return |
| `-I <dir>` (or `-I<dir>` attached) | Library search path, repeatable: compile-time lookup of imported `.n` and external `.ncu`/`.npkg`, run-time lookup of all dependency members and native dynamic libraries |
| `--version` | Version query, effective before any initialization |

The search-path layering matches ncc (`-I` > the script's own
directory > `NLANG_PATH` > the system directories including the
standard library). The standard library needs no `-I` or configuration
— after `import io;`, `io.print` works; point `-I` at third-party
libraries when you need them (see the matching entry on the
[ncc](ncc.md) page).

## Script-Mode Semantics

Top-level statements are wrapped in a synthesized `int main()`. The
synthesized entry is indistinguishable from a handwritten `main()` in
artifact and behavior, and the whole compile, link, and execute chain
matches `ncc <file>`. Synthesis happens only when top-level statements
exist: an ordinary program with a handwritten `main()` (the programs in
`examples/`, for instance) runs under nlang exactly as `ncc <file>`
runs it. Specifics:

- **Exit code = the top-level return**: `return 42;` exits the process
  with 42; with no top-level return the exit code is 0. A bare
  `return;` is an error (missing return value).
- **An empty script exits 0**: a file with no executable statements
  (empty, declarations only, or imports only) synthesizes no entry;
  nlang treats that as success and exits 0. This differs from nvm,
  which exits 1 when running an artifact without an entry point.
- **Declarations mix with statements**: functions, classes, structs,
  enums, and interfaces may interleave freely with top-level
  statements; all declarations register at load time (position
  independent) while statements execute in source order. `import` and
  `using` must still lead the file. Mixing top-level statements with a
  handwritten `main()` in one file is rejected by the duplicate-name
  check.
- **Imported units execute no statements**: a library unit loads
  declarations only. A library carrying top-level statements fails at
  compile time (`contains top-level statements`) rather than being
  silently skipped. This differs from python, where a module body runs
  on import.
- **Top-level break/continue are rejected**: a top-level `break` needs
  an enclosing loop or `switch`, and a `continue` needs an enclosing
  loop; otherwise the statement is a compile error.
- **An uncaught exception** prints `Runtime error: ...` and a call
  backtrace to stderr and exits 1.

## func-Prefixed Forward Declarations

At top level, empty parentheses belong to a zero-argument construction,
not to a function declaration: `Foo f();` constructs a `Foo` through its
zero-argument constructor, while `int f();` is rejected (`"Int32" is not a class or struct type.`). To declare a function without defining
it, mark the declaration with the explicit `func` prefix:

```nlang
func int twice(int v);   //declaration: implemented elsewhere (native or external)

int x = 21;              //this file does not call twice
return x;
```

The `func` form says the function is implemented elsewhere (native
registration or external linkage) — an implementation a standalone
script cannot supply, so a script declares such functions but does not
call them. Calling a function defined later in the same file needs no
declaration at all; `func` exists for implementations outside the file.
Modifier-led declarations are unaffected; `native int probe();` works
as before. A declaration alongside a definition in the same file is
rejected by the duplicate-definition check. Inside a function body,
`Foo f();` is a zero-argument direct initialization (a variable
declaration calling the constructor with no arguments) and keeps working;
the empty-parameter ambiguity exists only at top level (see
[Known Limitations](../language-spec/known-limitations.md)).

## Out of Scope

nlang is for ad-hoc runs: there is no interactive REPL, no script
arguments, no `.ncu`/`.npkg` output, and no `.nproj` project support.
Use ncc and nvm for those (see the [overview](overview.md)).
