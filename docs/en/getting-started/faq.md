# FAQ

### Where did the `.nmod` go?

`ncc build hello.n` without `-o` writes the module file to the
**current working directory**, not next to the source file — think of
this first when the build output is nowhere to be found. The default
rules and precedence for every form are on the
[ncc](../cli-tools/ncc.md) page.

On the nide side the location follows the output precedence (see
[Three Ways to Run](running.md#configuring-nide)):

- Projects: the `.nproj`'s `outputDir` > nide's global build output
  directory > the project directory;
- Standalone `.n` files: the global build output directory > the
  default location (a `nlang-nide` subdirectory of the user's temp
  directory).

### Exit code not what you expected?

The process exit code is `main`'s return value, and Windows preserves
the full 32-bit value; but POSIX shells (bash, Git-Bash, CI bash steps)
keep only the low 8 bits by convention — `return 300` is seen as 300 in
Python/cmd/PowerShell and as 44 in bash (300 mod 256). The testing
convention is expected values in 0–255 so every observer sees the same
thing.

See also: [Language Specification / Exit Code Convention](../language-spec/exit-code-convention.md).

### Garbled output in the console?

Program output is UTF-8 bytes. A Windows console's default code page
(e.g. GBK on Chinese-locale systems) renders them as mojibake; run
`chcp 65001` first to switch the console to the UTF-8 code page, then
run the program. Note that `fs` and `io` file paths and file names go
through the system active code page — non-ASCII file names do not
necessarily round-trip as UTF-8.

See also: [Language Specification / Standard Library](../language-spec/standard-library.md).

### Where are the docs and search?

The nide Help menu entries NLang Getting Started, Language
Specification, VM Architecture, and Command-line Tools all open in
the IDE's embedded help window (its content is the documentation site
under the installation's `docs\site\`), with the navigation tree on
the left. The search box is in the window's top-left corner (next to
the site title) and supports full-text search.

### `v + 1` with no spaces gives a `syntax error`?

The lexer eats the digits immediately after a binary `+` as a signed
literal — `v+1` is read as the identifier `v` followed by the literal
`+1`, and two expressions in a row is not a valid statement, so you
get `syntax error` (`Invalid statement.`). Work around it by putting
spaces around the operator (`v + 1`).

See also: [Language Specification / Expressions](../language-spec/expressions.md).

### Debugging: `io.readLine` fails / `finally` doesn't run on stop / breakpoints drift?

A debug session has no standard input — `io.readLine` throws an
`IOException` (catch it with `try/catch`; it does not hang silently).
Stopping a debug session is a hard stop: the process terminates
directly and `finally` does not run. Line-number drift is not tracked
inside a session — one session is one line-number snapshot, so editing
or rebuilding mid-session is not supported; reopen the debug session.
See the "Known v1 limitations" section of [Debugging in nide](debugging.md).

### Cross-module reference gives `Module '...' is not imported`?

`import` only opens **qualified names** — after `import lib;` you must
write `lib.f()`; the bare name `f()` does not resolve. Members of a
cross-directory **shared namespace** (two files declaring the same
`namespace NS`) are currently unreachable from the other directory —
there is neither a bare-name form nor a qualified form. The visibility
rules for each reference form are on the [Language Specification /
Declarations](../language-spec/declarations.md) "Import Declaration"
section. See [Common Error Messages](../language-spec/common-errors.md)
for the full error text.

### Assigning an array value to another type gives `Incompatible type`?

An array value has only two legal destinations — its own array type
and every `string` target (`toString`, etc.); every other scalar
context is a named compile-time rejection (`Incompatible type "a"`).
The full rule is on [Language Specification / Known
Limitations](../language-spec/known-limitations.md), the "Array values
in scalar contexts" item. See [Common Error
Messages](../language-spec/common-errors.md) for the full error text.

### `.nmod` version outdated, telling you to recompile?

The `.nmod` format floor only ever rises: a module produced by an
older ncc is refused as outdated
(`Module version ... is outdated; recompile with current ncc`) and
must be recompiled with the current toolchain. Each floor bump and its
semantic change are on [VM Architecture / Module
Serialization](../vm-architecture/module-serialization.md), the
"Version history" section; the rationale for each bump is in the
corresponding CHANGELOG release section.

### Cross-module function values / complex defaults / named arguments rejected?

The `.nmod` type descriptors carry only data types — not `Func`
signatures or parameter names — so these cross-module shapes are
rejected at the consumer's compile time: referencing an imported
function as a function value, passing a function reference to an
imported function, a non-constant-foldable default parameter, and a
named argument to an imported function (use positional arguments only).
See [Language Specification / Known Limitations](../language-spec/known-limitations.md),
the "Cross-module function values are rejected, not transported"
"Default parameters on imported functions" and "Named arguments on
imported functions" items. See [Common Error
Messages](../language-spec/common-errors.md) for the full error text.

### Condition / `&&` / `||` / `!` says "must be bool"? More than 64 parameters?

The condition of `if` / `while` / `do-while` / `for` / `assert` and the
operands of `&&` / `||` / `!` must all be `bool` (comparisons and
predicates already produce bool — there is no C-style "non-zero is
true"); int, string, float, char, class, struct, and array are named
compile-time rejections (`if condition must be bool, not "Int32"`,
`operator '&&' requires bool operands, got "Int32"`). Count tests
should be written `if (count != 0)`. Separately, the parameter count
of a function has a sanity ceiling of 64; exceeding it is a compile
error (`function "f" has 65 parameters; limit is 64.`). The condition
typing is on [Language Specification / Statements](../language-spec/statements.md)
("Condition typing"); the parameter ceiling is on [Language
Specification / Known Limitations](../language-spec/known-limitations.md)
("Parameter count ceiling" item). See [Common Error
Messages](../language-spec/common-errors.md) for the full error text.

### Member chain `s.length().toString()` crashes ncc?

This is a **known compiler defect** (the current ncc still reproduces
`ncc: internal crash (code 0xC0000005)`) — a chained member call such
as `s.length().toString()` on a `string` chain (a member access on a
method-call result) crashes the compiler at compile time instead of
giving a named diagnostic. Work around it with an intermediate local
(`int n = s.length(); string t = n.toString();`).

### The help window says "document not found"?

The help window locates the matching `.html` in the `docs\site\`
adjacent directory for the current language tree; when it cannot find
one it reports "The document '...' was not found next to the IDE
installation." This usually means the documentation site was not
deployed to `docs\site\` with the package, or the installation
directory's version does not match the documentation page being opened.

### How do I go back / forward in the help window?

The embedded help window's top bar has Back / Forward navigation
buttons to move between the documentation pages you have visited; the
help window is rebuilt each time it is reopened (it does not remember
the last browsing position).

More known limitations →
[Language Specification / Known
Limitations](../language-spec/known-limitations.md),
[VM Architecture / Known
Limitations](../vm-architecture/known-limitations.md).
