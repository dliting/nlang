# FAQ

### Where did the build artifact go?

`ncc build hello.n` without `-o` writes the `.ncu` to the
**current working directory**, not next to the source file; the `.npkg`
of a `-p` project build lands **next to the .nproj** — think of
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

### Running reports `module '...' not found`?

Artifacts do not embed library code: a `.ncu`/`.npkg` carries only its
own units, and external modules are loaded and linked at **run time**
along the library search path. The error looks like
`Runtime error: nloader failed:` (body line
`module 'lib' not found (searched: ...)`), with the parentheses listing
every directory searched — add the dependency's directory to `-I` or
`NLANG_PATH`, or place the dependency next to the artifact. The
standard library ships with the toolchain (`stdlib\`) and needs no
manual configuration. The mechanism is covered in
[ncc](../cli-tools/ncc.md), "Artifacts and load-time linking".

### Exit code not what you expected?

The process exit code is `main`'s return value, and Windows preserves
the full 32-bit value; but POSIX shells (bash, Git-Bash, continuous integration (CI) bash steps)
keep only the low 8 bits by convention — `return 300` is seen as 300 in
Python/cmd/PowerShell and as 44 in bash (300 mod 256). When writing
tests or scripts, keep expected exit codes within 0–255 so every
observer sees the same value.

See also: [Language Specification / Exit Code Convention](../language-spec/exit-code-convention.md).

### Garbled output in the console?

Program output is Unicode Transformation Format (UTF-8) bytes. The tools set the process active code
page to UTF-8 (declared in the tools' embedded manifest, Windows 10
1903+) and switch the attached console to the UTF-8 code page at
startup, so the default console renders Chinese output correctly — no
manual `chcp 65001` needed. `fs` and `io` accept paths and file
names beyond the American Standard Code for Information Interchange
(ASCII) set, and they round-trip as UTF-8 as well. Redirected output is untouched
bytes — an editor opening it with a non-UTF-8 encoding still shows
mojibake. The console switch outlives the tool: a program emitting a
legacy encoding in the same window afterwards may show as mojibake.

See also: [Language Specification / Standard Library](../language-spec/standard-library.md).

### Compile says `not valid UTF-8`?

Source files and `.nproj` project files must be saved as UTF-8: ncc
validates the whole file before tokenizing, and invalid bytes are
rejected with an explicit error message (`Source file is not valid UTF-8 ...
(first invalid byte at line N). Save the file as UTF-8.`);
a UTF-16 save gets a dedicated hint (re-save the file as UTF-8). This
stops legacy encoding bytes from slipping silently into string
constants. nide's build invokes ncc, so the same validation applies
there. A leading UTF-8 byte order mark (BOM) is accepted and skipped — the editor
"UTF-8 with BOM" save form needs no handling. See also:
[Language Specification / Primitives](../language-spec/primitives.md).

### Where are the docs and search?

The nide Help menu entries NLang Getting Started, Language
Specification, VM Architecture, and Command-line Tools all open in
the integrated development environment (IDE)'s embedded help window (its content is the documentation site
under the installation's `docs\site\`), with the navigation tree on
the left. The search box is in the window's top-left corner (next to
the site title) and supports full-text search.

### `v + 1` with no spaces gives a `syntax error`?

The lexer eats the digits immediately after a binary `+` as a signed
literal — `v+1` is read as the identifier `v` followed by the literal
`+1`, and two expressions in a row are not a valid statement, so you
get `syntax error` (`Invalid statement.`). Work around it by putting
spaces around the operator (`v + 1`).

See also: [Language Specification / Expressions](../language-spec/expressions.md).

### Debugging: how do I feed `io.readLine` / `finally` doesn't run on stop / breakpoints drift?

Standard input for both running and debugging lives in the terminal on
the Input & Output page: while a run is live, just type to interact;
while a debug session is live, type a line and press Enter, and it is
delivered to the program's next read
(see [Debugging in nide](debugging.md)). Stopping a debug session is a
hard stop: the process terminates directly and `finally` does not run.
Line-number drift is not tracked inside a session — one session is one
line-number snapshot, so editing or rebuilding mid-session is not
supported; reopen the debug session. See the "Known limitations"
section of [Debugging in nide](debugging.md).

### Why is `io.hasInput()` imprecise on an interactive console?

`hasInput()` must look into the stream's future without blocking, which
only file and pipe redirection allow. An interactive console delivers a
line per Enter, so the probe sees only what is already buffered — the
same position C is in on a console. Console programs usually do not
need `hasInput()`: following the C convention, treat `readLine()`'s
empty-string return as the loop's end. In a debug session (nide)
`hasInput()` reports only already-delivered parked lines; the
terminal's scrollback and copy work as usual.

### Cross-module reference gives `Module '...' is not imported`?

`import` only opens **qualified names** — after `import lib;` you must
write `lib.f()`; the bare name `f()` does not resolve. The visibility
rules for each reference form are on the [Language Specification /
Declarations](../language-spec/declarations.md) "Import Declaration"
section. See [Common Error Messages](../language-spec/common-errors.md)
for the full error text.

### Assigning an array value to another type gives `Incompatible type`?

An array value has only two legal destinations — its own array type
and every `string` target (`toString`, etc.); every other scalar
context is rejected at compile time with an explicit error
(`Incompatible type "a"`).
The full rule is on [Language Specification / Known
Limitations](../language-spec/known-limitations.md), the "Array values
in scalar contexts" item. See [Common Error
Messages](../language-spec/common-errors.md) for the full error text.

### `.ncu` version outdated, telling you to recompile?

The `.ncu` minimum format version only ever rises: a module produced by an
older ncc is refused as outdated
(`Module version ... is outdated; recompile with current ncc`) and
must be recompiled with the current toolchain. Each version bump and its
semantic change are on [VM Architecture / Module
Serialization](../vm-architecture/module-serialization.md), the
"Version history" section; the rationale for each rise is in the
corresponding CHANGELOG release section.

### Cross-module function values / complex defaults / named arguments rejected?

The `.ncu` type descriptors carry only data types — not `Func`
signatures or parameter names — so these cross-module shapes are
rejected at the consumer's compile time: referencing an imported
function as a function value, passing a function reference to an
imported function, a non-constant-foldable default parameter, and a
named argument to an imported function (use positional arguments only).
See [Language Specification / Known Limitations](../language-spec/known-limitations.md),
the "Cross-module function values are rejected, not transported",
"Default parameters on imported functions" and "Named arguments on
imported functions" items. See [Common Error
Messages](../language-spec/common-errors.md) for the full error text.

### Condition / `&&` / `||` / `!` says "must be bool"? More than 64 parameters?

The condition of `if` / `while` / `do-while` / `for` / `assert` and the
operands of `&&` / `||` / `!` must all be `bool` (comparisons and
predicates already produce bool — there is no C-style "non-zero is
true"); int, string, float, char, class, struct, and array are
rejected at compile time with explicit errors
(`if condition must be bool, not "Int32"`,
`operator '&&' requires bool operands, got "Int32"`). Count tests
should be written `if (count != 0)`. Separately, the parameter count
of a function is capped at 64; exceeding it is a compile
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
method-call result) crashes the compiler at compile time rather than
reporting an error. Work around it with an intermediate local
(`int n = s.length(); string t = n.toString();`).

### The help window says "document not found"?

The help window locates the matching `.html` next to the IDE
installation (the directory containing `docs\site\`) for the current
language tree; when it cannot find
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
