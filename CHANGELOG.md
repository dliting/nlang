# Changelog

**English** | [中文](CHANGELOG.zh-CN.md)

All notable changes to NLang are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [0.8.2] - Unreleased

### Added
- **C++ host embedding API** (`nlang::Interpreter`): load and run compiled
  `.ncu`/`.npkg` artifacts in-process, register host functions callable
  from scripts, exchange typed values through read/write proxies with
  garbage-collector root guarantees, catch script exceptions as C++
  exceptions, and redirect script output. New manual chapter "Embedding
  NLang"; see `examples/embed_host` for a minimal host.

## [0.8.1] - 2026-10-08

### Changed
- nide: the terminal adopts a light theme to match the rest of nide's light
  windows — white background with black default text, a soft light-blue
  selection highlight, and a light 16-colour ANSI palette (VSCode Light
  Modern) so externally-coloured programs stay legible on the white
  background.

## [0.8.0] - 2026-10-08

### Added
- Packages: a `.n` file's package is its path relative to the matched
  search root (`vendor/graphics.n` under `-I <root>` is
  `vendor.graphics`), and dotted imports address library sources
  directly. One build may contain only one package of each dotted name
  (a duplicate is a compile error naming both source paths); the old
  reserved-name table (`io`/`math`/`fs`) is gone — a project directory
  named `io` is an ordinary directory. The manual gains a Packages
  page in both language trees.
- Compiler: `readStruct`/`readObject` type-name literals resolve at
  compile time against the visible packages and are rewritten to the
  declaration's qualified table key; ambiguous literals are a compile
  error asking for the qualified spelling.
- Artifacts: code generation is per translation unit. A single-source
  build emits the entry unit's `.ncu` image and nothing else; a project
  build (ncc and nide alike) packs a `.npkg` program archive — one
  member per unit plus the entry record, FNV-1a checksums. Library code
  is no longer baked into consumer artifacts: a program carries only
  its own units.
- Execution: every CLI runner resolves the import closure at load
  time. `nvm`, `ncc` and `ndb` discover the referenced units along the
  search path (a `.ncu` file or a `.npkg` member), verify version and
  checksum, and link the closure in memory before execution. The
  standard library ships as `stdlib.npkg` on the search path like any
  other library package.
- Native: free `native` functions are keyed `<package>.<name>` at run
  time, and a missing key lazily loads `nlang_<package>.dll` from the
  same search path (the standard library's io/math/fs are exactly such
  DLLs). A native declaration in a multi-segment package is a
  compile-time diagnostic — a dotted package cannot name a host DLL.
- VM: `.ncu` format v2.0 — struct/class/function table keys and stream
  type-name literals are package-qualified (ownerless built-ins keep
  bare keys), per-category import-slot tables record the referenced
  external symbols, and the entry point is stored by qualified name.
  The loader refuses every v1.x image outright; recompile.
- io: `io.write` — prints to stdout without a trailing newline (the
  prompt half of interactive programs), and `io.eprint` — prints to
  stderr. `io.print` is unchanged.
- VM: `IHostIo::ReadInputLine` — the host I/O seam now carries program
  input, not just output: an installed host can supply whole lines to
  `io.readLine` (blocking is allowed); a host that does not override
  it still answers NoChannel, which makes the input reads raise a
  catchable IOException (unchanged since 0.7.5). With no host installed (nvm,
  ncc, the CLI front ends) the console behavior is unchanged.
- ndb `--machine` mode: a `stdin<TAB><payload>` data command delivers
  one program input line over the protocol channel. It is recognized
  on the raw wire (payload spaces survive), accepted at every read
  site — queued as type-ahead before `run`, queued without breaking a
  frozen stop, consumed live while the program is parked in
  `io.readLine` — and never answers; the program's next read is the
  response. EOF on the channel still ends the session.
- nide: the Run Output page is replaced by an embedded system terminal
  (the vendored libvterm emulator behind a pseudo terminal (PTY) —
  ConPTY on Windows). Run and debug share it: while a run is live,
  typing interacts with the program directly (Ctrl+C sends the
  interrupt when no text is selected, Ctrl+V pastes, drag selects,
  the wheel scrolls back through history); while a debug session is
  live, the terminal switches to local line editing — Enter delivers
  the whole line over the machine channel to the program's next
  `io.readLine`, with an in-session history of 50. Chinese input and
  output are first-class UTF-8. Debug sessions no longer reject
  interactive-input programs.
- Tools: `--verbose` (short form `-v`) on ncc, nvm and ndb prints the
  resolved import search path before proceeding normally — one directory
  per line in search order, each annotated with the layer it came from
  (`(-I)`, `(project import paths)`, `(local directory)`, `(NLANG_PATH)`,
  `(system)`). It is the observability counterpart of the not-found
  diagnostic (rerun a failing invocation with `--verbose` to see every
  directory that will be searched); in `ndb --machine` mode the listing
  goes to stderr because stdout is the protocol channel.
- Examples: `examples/libdemo` — a flat search-path library demo. The
  library unit compiles first (`ncc build mylib.n`); the consumer
  imports it and the loader links both units at load time.
- User manual: new "Libraries and Integration" chapter (developing
  libraries, integrating NLang) and two getting-started introductions
  (extending NLang, embedding NLang in other programs).
- User manual: every abbreviation is expanded on its first occurrence
  per page (PRNG, ABI and friends); typography normalized to compact
  spacing (no space at CJK/Latin boundaries), navigation labels
  included.
- Docs: added `docs/user-manual-style-guide.md` as the single source
  for manual editing policies and the abbreviation glossary, enforced
  by two new automated guards in the docs test suite.
- io input symmetry: `hasInput()`, `readToken()` and `readChar()` natives
  plus `readInt`/`readLong`/`readFloat`/`readDouble`/`readBool` wrappers
  in the standard library. End-of-input follows C/C++/Java: line reads
  return the `""` sentinel, token/value reads raise `IOException`, and
  `hasInput()` tells a final empty line from end of input. Token reads
  mix with `readLine()` using the C++ `cin>>`/`getline` remainder
  semantics.
- nvm: the console code page is UTF-8 throughout — Chinese output no
  longer turns to mojibake when running directly in a terminal;
  handles redirected to a file or a pipe are unaffected.
- nide editor: go-to-definition accepts Ctrl+click and F6 alongside
  F12 — a pointing-hand cursor marks the jump targets while Ctrl is
  held — and multi-segment packages (`vendor.graphics.hue`) now
  resolve instead of only two-segment names. The symbol index also
  covers each open project's own directory and every open file's
  directory, so project-local packages are jump and completion targets
  too, and it rebuilds when a solution or project opens or closes (a
  project opened after startup previously missed its import paths in
  completion and go-to-definition until its properties dialog was
  touched).
- nide editor: the font size (6–48 pt) is a setting — set it under
  Tools → Options, or zoom with Ctrl+wheel inside an editor (one
  point per notch). Every code editor shares the one size, and it
  persists across sessions.
- nvm: an interactive console prints the runtime's `> ` prompt when
  the program is about to block on an input read — the REPL convention
  (Python, sqlite3). It is gated to consoles: both stdin and stdout
  must be consoles, so a direct run, `ncc` running a program and the
  nide run page get it, while any pipe or redirection keeps both
  streams byte-exact for scripted input.
- nide: the debug terminal's line mode heads the pending input line
  with the same `> ` prompt, and the committed line is echoed to the
  history with the prompt included.
- nide editor: an `import` statement's package name is itself a jump
  target — Go to Definition (F12, F6 or Ctrl+click) opens the file
  that declares the package. Multi-segment packages resolve through
  nested library directories: the index walk descends into
  subdirectories (dot directories are pruned), and the path-to-package
  rule now mirrors the compiler's, so the index offers only symbols a
  build can resolve.
- nide editor: while Ctrl is held, a jumpable name renders as a blue
  underlined hyperlink; the decoration clears when the pointer leaves
  the editor or the key is released.
- nide editor: the editor's context menu, and a new Edit menu, carry
  Go to Definition (F12); both entries are enabled only while the
  cursor sits on a jump target and follow the cursor as it moves.
- nide: the bottom panel tab is renamed from "Input & Output" to
  "Terminal", matching what the embedded terminal is.
- nide: the terminal gains the full Windows Terminal copy/paste family
  — a right-click menu (Copy / Paste / Select All, Select All covers
  the visible screen) plus Ctrl+Shift+C, Ctrl+Shift+V, Ctrl+Insert
  and Shift+Insert alongside the existing Ctrl+C / Ctrl+V.
- nide: Ctrl+wheel in the terminal zooms its font, persisted as its
  own setting (`ide/terminalFontPt`, 6–48) and separate from the
  editor font size; Tools > Options grows a "Terminal font size"
  field.

### Changed
- Manual: the Standard Library chapter is now a two-level section with
  one page per topic (math, io, fs, string methods, streams, libraries
  and search paths, exception mapping); the chapter page keeps the
  package model and the argument-type rules.
- Language: the `namespace` keyword is removed — the wrapper syntax,
  the cross-unit container merge and the reserved-name table went with
  it; every package identity is the path. User-visible diagnostics now
  say package (`Package 'io' is not imported. ...`), the IDE completes
  and indexes by package, and the project-properties dialog lost its
  never-read namespace field along with the `.nproj` attribute.
- Debugger/tooling spelling: function keys are qualified everywhere —
  breakpoints take `b main.main`, backtraces print `at main.main`,
  `ndisasm -func` filters by the qualified key, and function values
  render the qualified key. Type names inside value renders — an
  object's default `toString()` (`Point@1a2b`) and the debugger's value
  display (`Point{x=2, y=5}`) — use the leaf of the qualified key, the
  name as source wrote it. Cross-program object streams store the
  qualified class key (programs must agree on the package layout).
- Diagnostics: load-time failures surface as
  `Runtime error: nloader failed: ...`, reporting the missing module
  and every searched directory in one shot; link failures report in
  one shot under `Runtime error: nlink failed: ...` (unresolved or
  ambiguous imports in the closure).
- Manual: both language trees rewritten for the artifact model — the
  ncc/nvm/ndb/ndisasm references, the running guide and FAQ, module
  serialization and the library-mechanism design note; README, examples
  and the issue template follow the new artifact names.
- nide: the bottom panel tab "Run Output" is renamed "Input & Output" —
  it carries program input as well as output.

### Fixed
- nide editor: the package completion popup now dismisses like a menu
  — a click elsewhere in the editor or focus moving to another panel
  closes it. It used to float on top of the window until Escape or a
  keystroke.
- Compiler: assigning to a method call result (`obj.f() = v`,
  `obj.f() += v`) is now a compile error (`cannot assign to the result
  of a method call`). Both statements used to parse and compile
  cleanly and then silently emit no code — the call was not even
  evaluated. Field stores keep working; the receiver expression of a
  member left value is still evaluated exactly once.
- nide terminal: a wide glyph (CJK) at a non-zero column was clipped to
  its left grid cell — the draw rect now spans the glyph's columns, and
  the caret block and the repaint damage follow the same span; line
  mode's pending line got the same treatment.
- nide terminal: reading a line back dropped its interior blank cells.
  conhost steps over a gap with a cursor-forward escape instead of
  writing a space, so "Name: > " read back as "Name:>", and copying
  such a line lost the spaces; a blank cell now reads back as one
  space per column it spans.
- nide editor: the package completion popup is a child of the editor
  viewport now, so a candidate can be picked with the mouse — as a
  tool-tip top-level window it swallowed the press, and the list could
  only be dismissed. Typing an identifier narrows the list to the
  names starting with what was typed instead of closing the popup.


## [0.7.6] - 2026-10-02

### Fixed
- nide debugger: clicking a row of the call-stack tree no longer
  appends a duplicate frame to the list (the machine protocol's
  `frame <n>` selection command echoed a `frame` event on every
  variables-pane refresh; frame events now belong exclusively to `bt`
  responses).
- ndb `locals` now shows the receiver of a method frame as `this`
  (expanded one level); before, a method frame's locals query came
  back empty, which left nide's variables pane blank for those frames.
- nide: the run-output and compile-output pages now decode program
  and compiler output as UTF-8 (matching the debug page); they were
  decoded with the local code page, which mojibaked every non-ASCII
  output on a non-UTF-8 system locale.
- Debugger locals now honor declaration scope: a local joins the
  variables display only once the paused statement has reached its
  declaration line — on the declaration line itself it shows the
  default zero value (the gdb/IDE convention), and locals declared on
  later lines stay hidden. Before, the whole flat frame was dumped,
  so a local declared below the paused line showed up as a zero value
  (a string read as `""`). Applies to ndb `info locals`, the machine
  protocol, and nide's variables pane alike.
- The four command-line tools (ncc/nvm/ndisasm/ndb) now run with
  UTF-8 as the process active code page (declared in an embedded
  manifest, Windows 10 1903+): non-ASCII command-line arguments and
  paths work end to end. Before, a path character outside the system
  code page (an emoji directory name on a GBK-locale system, say) was
  destroyed during argv ingestion and the tool failed hard, and even
  representable paths echoed as mojibake for UTF-8 readers (nide's
  output pages).
- `.n` source files and `.nproj` project files are now validated as
  strict UTF-8 before tokenizing: invalid bytes are rejected with a
  named error carrying the first invalid byte's line, and a UTF-16
  save gets a dedicated hint. Before, a legacy-encoded source passed
  silently (the wrong bytes ended up inside string constants), a
  legacy-encoded `.nproj` passed silently or failed with mojibake
  diagnostics (an `outputDir` in the wrong encoding even created a
  mojibake-named directory), and a leading UTF-8 BOM corrupted the
  first token — a BOM-prefixed `int main()...` even "compiled
  successfully" to a module with no main function. The UTF-8 BOM is
  now accepted and skipped, so the editors' "UTF-8 with BOM" save
  form works, and line endings normalize to LF (CRLF pairs, and lone
  CRs, alike).
- nide: quitting (or closing the solution) no longer claims unsaved
  changes when the user only opened a project. Opening a project with
  no solution open auto-creates a solution wrapper, and the wrapper's
  own bookkeeping used to count as "the solution or its projects have
  unsaved changes" — a phantom save prompt at exit with zero user
  modifications. The wrapper is now ephemeral scaffolding: it never
  prompts on its own, saving it (Save Solution) promotes it to a
  first-class solution whose changes prompt as before, and at close
  or Save All only genuinely dirty projects are persisted, each to
  its own `.nproj` — no `.nsln` name is demanded for scaffolding the
  user never created. A project authored through the New Project
  dialog still counts as unsaved from its creation (nothing reaches
  the disk until it is saved), so closing keeps prompting for it.

### Added
- Manual: the char representation chain is documented end to end in
  both trees (source files must be strict UTF-8, a leading UTF-8 BOM
  accepted and skipped; a compiled char is
  its plain 32-bit code point in a 4-byte slot — neither UTF-8 nor
  UTF-16; UTF-8 appears on the string side and on the console, which
  receives verbatim UTF-8 bytes — the tools switch the attached
  console to UTF-8, so the default console renders it).

### Changed
- The four command-line tools switch the attached console to the
  UTF-8 code page at startup (companion to the manifest above):
  non-ASCII program output
  renders in a default console without `chcp 65001`, and non-ASCII
  `fs`/`io` paths and file names round-trip inside running programs.
  Redirected output stays verbatim bytes.
- `.nmod` format floor raised to v1.14 (a layout change): each local
  descriptor in a function's locals block gains a two-byte declaration
  PC, which the debug views use for the scope visibility above.
  Modules from older toolchains are rejected and must be recompiled.

## [0.7.5] - 2026-09-30

### Added
- The scalar primitive family is complete: twelve types — `byte`
  `ubyte` `short` `ushort` `int` `uint` `long` `ulong` `float` `double`
  `bool` `char` — over a single registry (RTK 10..19). Integer literals
  tier by value range across the family (`5000000000` is a long),
  constants must fit their declared target (`byte b = 1000` is a compile
  error, `byte b = 5` is not), and mixed integer arithmetic promotes to
  the minimal type that implicitly holds both operands (`int` + `uint`
  is a long; `int` + `ulong` has no implicit common type and is a
  compile error).
- `char`: a Unicode scalar value with `'\uXXXX'` literals (BMP only;
  adjacent surrogate escapes combine into one code point inside string
  literals, and char literals reject the surrogate range). The string
  bridge: byte access `s[i]` returning `ubyte`, code-point iteration
  `foreach (char c in s)`, and the `charAt`/`charCount`/`toChar` method
  family.
- Strict bool: comparisons and library predicates return `bool`; the
  five condition positions (`if`/`while`/`do-while`/`for`/`assert`) and
  the operands of `&&`/`||`/`!` accept bool only (`if (count)` must
  become `if (count != 0)`). `equals` deliberately stays int 0/1 as a
  user-overridable protocol, so its result needs `!= 0` in conditions.
- Lossy implicit conversions now warn (`implicit conversion from 'Long'
  to 'Float' loses precision`), with constants that are exactly
  representable exempt; `ncc --no-warn` suppresses warnings, and an
  explicit `as` never warns.
- Streams: 64-bit primitives `writeLong`/`readLong` and
  `writeDouble`/`readDouble` on `ByteStream` and `FileStream`, with
  scalar arguments checked per the conversion matrix (narrower integers
  and `float` widen implicitly; `ulong` needs `as long`).
- `io.print` accepts every scalar primitive (in addition to string and
  arrays); to-string rendering for all scalars goes through one
  generalized `OP_Prim_to_str <kind>`, with doubles printed in
  shortest round-trip form.
- nide: two-level compiler options — warning suppression globally under
  Tools → Options, with a per-project override in the project
  properties.
- ndb: locals render in type-aware form (char as `'中' (U+4E2D)`, bool
  as `true`/`false`, narrow and 64-bit integers decimally); ndisasm and
  the shared disassembler name scalar wire kinds
  (`i8/u8/i16/u16/i32/u32/i64/u64/f32/f64/bool/char`).

### Changed
- Breaking: `long`, `ulong` and `double` are reserved words (they were
  valid identifiers before).
- Unsuffixed decimal literals are `double` (`1.5f` stays `float`), and
  exponent literals are double; integer targets never accept float
  constants (`int x = 2e5` is a compile error).
- Numeric literals no longer carry a leading sign: `-5` is unary minus
  applied to the literal `5` (constant-fit still covers negated
  literals).
- Narrowing assignments (`float`→`int`, `double`→`float`, wider→narrower
  integers) are compile errors unless written with `as`; `as` performs
  exactly the conversions the implicit matrix does not admit.
- The math floating-point family runs at double precision (`floor`/
  `ceil`/`round` return `long`; integer and `float` arguments widen
  implicitly).
- Overload resolution ranks scalar conversion distance by type category
  and rank: a narrow integer argument now prefers an `int` formal over a
  `float` or `string` formal.
- The 0.7.3 string-subscript compile-time rejection is rescinded: `s[i]`
  is byte access returning `ubyte`; out of range throws at runtime.
- `.nmod` format v1.13: the scalar kind code space expands in local and
  field kind bytes, type descriptors, and boxing tags; numeric
  instructions emit as kind-immediate generic families dispatched
  through function-pointer tables. The loader rejects minor < 13 —
  older modules must be recompiled.

### Fixed
- Negative enum member values (`enum E { A = -1 }`) are an explicit
  compile rejection; they hung the compiler outright before.

## [0.7.4] - 2026-09-27

### Added
- Source-size regression guard (`tools/source_size_guard`): hand-written
  source files stay <= 500 lines and function definitions <= 50 lines,
  with any exception registered alongside its reason; enforced as the
  ctest case `source_size_guard`. CONTRIBUTING documents the guidelines.
- Manual: "Common Error Messages" reference page in both language
  trees, mapping the frequent ncc diagnostics to their causes.
- Manual: concept sections for loops and conditionals
  (`for`/`while`/`do..while`/`if`), arrays and virtual-method
  overriding, plus an `assert` example in the statements page.
- Manual: high-frequency nide FAQ entries (typical scenarios,
  help-window items) and a known-limitations cross-reference; the
  module-serialization page now documents the v1.12 format.
- Manual: the nide running guide now covers the recent-file list, the
  toolbar icon size and the project-properties dialog; the language
  overview page was renamed to match the project name
  (what-is-nlang).
- Manual: the language-spec reference was decomposed into per-construct
  pages — each type, statement, expression, and function feature now has
  its own focused page, and the overview pages retain shared semantics and
  link out to the per-construct pages.

### Changed
- Internal: the oversized core sources were split into per-concern
  files with zero behavior change — verified by a byte-identical
  bytecode golden set across all 1042 fixtures plus line-level purity
  proofs for every move. The backend emitter went from a single
  6533-line `VmBackend.cpp` to 24 files under `src/vm/backend/`, the
  expression and statement resolvers from a 4521-line
  `ExprResolver.cpp` to 23 per-concern files, and the bytecode
  interpreter from a 3894-line `VmExecutor.cpp` to 14 files with the
  largest non-dispatch function now a 30-line orchestrator.
- Internal: the decomposition pass then covered the remaining
  oversized modules, all behavior-neutral under the same test suites:
  the 695-line `ModuleBuilder.cpp` into three per-concern TUs,
  syntax-node class declarations into per-category TUs, the oversized
  functions of the CLI tools (ncc/nvm/ndb/ndisasm) and the shared
  disassembler, and the IDE — the 1974-line `MainWindow.cpp` into six
  domain TUs and the project model into in-memory operations plus
  .nproj/.nsln persistence TUs.

### Fixed
- Compiled module output is byte-reproducible again: the class table's
  per-field access byte was serialized from an uninitialized member of
  the compiler's class-field node (the node declared a member shadowing
  the base-class access value, and no constructor ever wrote it), so
  the same compiler could emit different module bytes for the same
  source across runs — with values outside the legal access range. The
  backend now reads the actual access value, and a new regression test
  (`nmod_determinism`) asserts byte-identical output across repeated
  compiles.
- Manual: the Object virtual-method documentation now matches the
  implementation (`equals`/`getHashCode`), and a stray reference to a
  separate bool type was corrected.


## [0.7.3] - 2026-09-25

### Added
- Manual: "Command-line Tools" reference chapter (ncc/nvm/ndb/ndisasm)
  in both documentation trees, including the full ndb command table with
  long aliases and both ncc default-output rules. Each tool page opens
  by stating what the tool is for and when to reach for it.
- nide: the Help menu gained a "Command-line Tools" entry, symmetric
  with the Getting Started / Language Specification / VM Architecture
  chapter entries.
- Manual: nide Tools → Options documentation (language setting and
  global build output directory with its precedence rules).

### Changed
- Runtime strings are garbage-collected immutable objects: long-running
  programs (prompt-building loops and similar) no longer grow memory
  without bound, and `s = s + x` appends are O(1) instead of O(n) copies
  (transparent concatenation nodes, flattened on first read).
- Manual: newcomer-readability pass across both documentation trees —
  internal development annotations were removed, unexplained
  implementation identifiers were replaced with named concepts, and
  tool usage is now cross-linked from the language specification and
  README.
- Array-valued expressions now carry an interned array type token in
  their static type channel: array types are first-class resolved
  entities (one interned token per element type per compilation),
  declarations and value sites share the same token, and the
  array-valued property is derived from it instead of a side channel.
  Same-type array flow compares tokens by identity, and cross-element
  array conversions (`string[] b = ia`) keep their named diagnostic.
- An array value now converts only to its own array type — element
  identity, not representation equivalence: the covariant upcast
  `Base[] ba = da` and the `enum[]` ↔ `int[]` shared-representation
  interop (both directions) are compile errors ("an array value only
  converts to the same array type"). This closes the unchecked
  covariant store hole at construction — `ba[0] = new Base()`
  previously passed both the compiler and the runtime and silently
  stored a `Base` object into a `Derived[]` slot.
- String subscripting (`s[i]`, read or store position) is now a
  compile-time rejection ("string does not support subscript access");
  it previously compiled and aborted at runtime with "null array
  access".
- String coercion of array values is uniform across all string targets:
  string parameters (`f(arr)` where `f` takes a `string`) and
  `io.print(arr)` now yield `"[1, 2]"` like assignment, return and
  concatenation positions, and string element slots coerce the same way
  (`string[] sa; sa[0] = ia`, `List<string>` subscript stores and
  `.add`) — the 0.7.2 whole-value-positions-only boundary is dropped.
- `foreach` accepts array-valued sources directly: all seven formerly
  rejected shapes — container `get` results (`li.get(0)` on a
  `List<int[]>`), container subscripts, call results, member-invoke
  chains, `new int[3]` allocations, `Dict` sources and dict subscripts —
  no longer need a typed local first; the source is evaluated exactly
  once (it is bound to a hidden iteration local).
- Array comparisons and conditions no longer pass silently through the
  degraded element type: `==`/`!=` between arrays, or against `null`,
  are identity comparisons, while cross-type comparisons (`arr == 5`),
  relational operators (`arr < arr2`), numeric binary operands
  (`arr + 1`) and condition positions (`if (arr)`, `while (arr)`) are
  compile errors. Overload resolution no longer picks arbitrarily
  between array-parameter candidates on a `null` argument —
  equally-legal candidates are an "ambiguous call" error.
- Container method value arguments are type-checked through the cast
  table: `List<int>.add(arr)` is a compile error ("Incompatible type")
  instead of storing a raw handle, `List<string>.add(arr)` coerces to
  the string form, and `l[0] = 5` on a `List<int[]>` rejects the
  non-null int ("only the null literal converts from int to a class,
  interface or array type").
- `Dict.set`'s key argument is type-checked through the same cast
  table as its value argument, in both spellings — the method form
  `d.set(k, v)` and the subscript sugar `d[k] = v`: a string key in
  `Dict<int, int>` is a compile error ("Incompatible type") instead
  of being stored raw — which corrupted the key slot and crashed
  later lookups — while a coercible key (an int into
  `Dict<string, int>`) converts implicitly like every other string
  target. Read positions (`get`, `containsKey`, `remove`) stay
  unchecked, as before.
- `.ncu` format floor raised to v1.11 → v1.12: recursive type
  descriptors record the true formal, return and field types (nested
  arrays, `List`/`Dict` instantiations, struct/class indices; depth
  capped at 8). Imported function stubs are rebuilt with real
  signatures instead of return-kind placeholders, so cross-module
  call-site type checking matches same-module calls — `lib.mk()`
  returning `float[]` into an `int[]` local is rejected, a float
  argument widens exactly as in same-module calls, and `out` arguments
  round-trip. Older modules are rejected as outdated and must be
  recompiled.

### Performance
- Short strings (up to 40 bytes) created at runtime are interned and
  reused; equality between interned strings is a handle comparison.

### Fixed
- Storing into an array element now goes through the same implicit
  conversion checks as plain assignment, for every base shape —
  identifier, member (`c.arr[i] = v`), chained (`li[0][1] = v`) and
  call (`mk()[0] = v`): primitives box into `Object[]` elements,
  `int` → `string` elements coerce, struct values deep-copy into
  `struct[]` elements, and mismatched stores (a struct into an
  `Object[]` element, a string into an `int[]` element, an array
  handle into a non-string element) are compile errors instead of
  silently storing a handle the garbage collector cannot trace.
  Array-form init lists (`int[] a = [1, 2]`) run the same
  per-element checks: entries box or coerce like element stores,
  struct entries deep-copy, and an array-valued entry into a
  non-string element is a compile error. Container subscript stores
  (`li[i] = v`) run the same element-type checks as array element
  stores (`int` → `float` elements coerce, a class value into
  `List<int>` is a compile error) and reject stores whose array-ness
  disagrees with a non-string container element — an array value
  into `List<int>`, or a scalar into `List<int[]>`. String element
  slots are the exception on the array side: they coerce an array
  value to its string form (see the uniform string coercion under
  Changed) instead of rejecting it.
- An array value no longer leaks through scalar positions as its
  degraded element type: `int x = arr`, `Object o = arr`,
  `return arr` from an `int` function, and `ia as int` /
  `ia as Object` are compile errors ("the stored value is an
  array" / "the returned value is an array") instead of compiling
  and passing the raw handle through as an int. Passing an array to
  a non-array parameter — or a value whose array-ness or element
  type disagrees with an array parameter, including `out`
  arguments — is likewise rejected by the invoke compatibility
  check. The two legal targets — the same array type, and string
  coercion at every string position — are described under
  Changed above.
- Assigning a non-null `int` or enum value to a class- or
  interface-typed target is now a compile error ("only the null
  literal converts from int to a class, interface or array type");
  previously it compiled and stored a garbage handle. Variable
  initialization, assignment and element stores are all covered.
- `null as T` keeps its null identity through the cast: storing it
  into an `Object[]` element (`oa[0] = null as Object`) stores the
  raw null handle instead of a boxed 0, so later null comparisons and
  protocol calls behave as with a plain null literal. One corner
  changed from silent garbage
  to an error: an argument that must not be null
  (`io.print(null as int)`) is now rejected at compile time instead
  of printing `0`.
- Runtime diagnostics no longer mention internal phase names: the
  `WriteStruct`/`ReadStruct` "does not support array/Func fields"
  errors read the same without the development-phase suffix.
- `Object`-declared storage holding a boxed primitive now works end to
  end: `Object o = 5; o.toString()` returns `"5"` instead of garbage
  like `Object@1` (virtual dispatch no longer misreads the boxed type
  tag as a class index), and the garbage collector traces boxed
  records held in class/struct `Object` fields and `Object[]` elements
  (previously swept while still reachable, leaving dangling handles).
- Assigning a primitive into an `Object[]` element now boxes it, so a
  later `as int` read returns the value instead of crashing.
- `nvm --gc-stress=N` is accepted both before and after the module
  path (previously only after it).
- `List.indexOf`/`contains` now compare string elements by content;
  concatenated or interned equivalents of a stored element now match.
- `Exception.backtrace.get(i)` no longer raises
  "unbox on null/invalid reference"; frame entries read normally.
- Manual: FAQ ".ncu output location" answer now reflects the 0.7.0
  global build output directory; the shipped-tools table now lists all
  five tools.
- Manual: the stack-frame layout page was rewritten to match the actual
  frame layout, and the exit-code limitation now states precisely that
  only a POSIX shell's `$?` truncates to the low 8 bits.

## [0.7.0] - 2026-09-19

### Added

- nide `Tools → Options` dialog: UI language (system / Chinese /
  English, applied on restart) and a global build output directory
  (standalone `.ncu` files land there; projects fall back to it when
  the `.nproj` sets no output directory).
- The manual is now fully bilingual: the docs site builds two complete
  trees (`zh/` + `en/`) behind a language-detecting landing page with
  a cross-tree switch link, and the nide Help menu opens the tree
  matching the language setting.
- Root documentation is bilingual: `README.zh-CN.md` and
  `CHANGELOG.zh-CN.md` mirror the English originals (cross-linked at
  the top of each file) and ship in the release packages.
- nide: while the build output directory is unset, the Tools → Options
  field shows the default location (`%TEMP%\nlang-nide`) as a
  placeholder, and Browse starts there.

## [0.6.2] - 2026-09-15

### Added

- Generic type arguments may be array types (`List<int[]>`,
  `Dict<int[], int>`): instantiation keys carry per-argument
  array-ness, array-typed elements flow as raw GC-traced handles
  instead of boxed primitives, and `Func` signature matching and
  `Dict.keys()` preserve array-ness across the erasure boundary.
- `foreach` and `for` loop variables may be array-typed
  (`foreach (int[] row in grid)`); the loop-variable type must match
  the element type exactly — same field and same arrayness, no numeric
  widening (`foreach_var_mismatch_reject`, `foreach_var_widen_reject`).
- Array element opcodes (`OP_LoadElement`, `OP_StoreElement`,
  `OP_ArrayLength`) validate the base slot kind at run time and fail
  with a named diagnostic instead of reading a dangling handle.

### Changed

- `.ncu` format floor raised to v1.10 → v1.11: a semantic change in
  generic container element storage (raw traced handles, no primitive
  boxing); older modules are rejected as outdated and must be
  recompiled.

## [0.6.1] - 2026-09-12

### Changed

- Positioning rewrite across README, CONTRIBUTING and the docs site:
  NLang is described as a statically-typed scripting language for
  embedding and automation — with a small C++ host API, native bindings
  and in-process debug hooks — and a testbed for AI-friendly language
  features. All references to the private predecessor codebase were
  removed from the shipped sources and documentation.
- The getting-started `switch` example no longer returns from each case
  arm; the accumulated-result form makes the no-fall-through semantics
  visible.

### Added

- Public-text guard: a single pattern source scans every tracked file
  (ctest `nlang_docs_pytest`) and every release package
  (`verify_package.py`), failing the build or the release when
  predecessor references reappear in public prose.

## [0.6.0] - 2026-09-12

### Added

- Array-valued expressions carry a resolve-time type property; jagged
  array declarations (`T[][]`) and non-container `foreach` sources are
  rejected at compile time with named diagnostics.
- GC tracks array records held in array-typed element slots.

### Changed

- `.ncu` format floor raised to v1.10: array struct/class fields now
  store `RTK_Array` as their field kind (previously the element kind);
  older modules must be recompiled.
- Streaming a struct with an array field (`bs.writeStruct`) now throws
  a named error instead of silently writing the raw heap handle.

### Fixed

- Class fields typed `int[]` no longer break `toString()` dispatch.
- `.length` resolves on any array-valued receiver (`li.get(0).length`,
  `lib.mk(3).length`), not just identifier locals/fields.

## [0.5.0] - 2026-09-09

### Added

- nide: a debugging suite. F5 starts a session — the program builds,
  then runs to the first breakpoint or to completion — and pressing F5
  again continues; Shift+F5 stops the session at any time (a hard
  terminate that always works, including inside infinite loops or
  native code). F9 or a gutter click toggles a breakpoint (filled dot =
  bound in the live session, hollow = not bound); breakpoints persist
  across restarts and follow file renames. F10/F11/Shift+F11 step
  over/into/out. The new 调试 page in the output area shows the session
  status, a 抛异常时中断 (break-on-throw) switch, the call stack
  (clicking a frame selects it, jumps to the line and refreshes locals)
  and the selected frame's locals; the paused line is highlighted in
  the editor with a gutter arrow. Program output and error backtraces
  stream to the 运行输出 page. Build/Run are disabled while a session
  is live, and closing nide terminates the debugged process.
- VM: `IHostIo` — a host I/O seam for embedded front ends: output bytes
  arrive verbatim through a callback, and an installed host that
  declares no input makes `io.readLine` raise a catchable IOException
  instead of silently consuming the embedder's stream. With no host
  installed (the default) the console behavior is unchanged, so ncc,
  nvm and the CLI debugger are unaffected.
- ndb: `--machine` mode — a line protocol over stdin/stdout for IDE
  embedding (tab-joined events with escaped fields:
  hello/bp/stopped/frame/local/done/output/exited/error/err; breakpoint
  setup before `run`).

### Changed

- VM: while/for/do-while line breakpoints and stepping now hit on every
  iteration — the back edge lands on the anchor (condition entry for
  while/for, tail condition for do-while; gdb semantics). Previously
  the anchor fired only at loop entry, so an empty-body loop had no
  per-iteration checkpoint.
- ndb: breakpoint identity is now one id per source line — a line
  carrying several statement anchors (e.g. a loop header) merges them
  under a single breakpoint id, so setting, deleting and reporting
  breakpoints behave identically in the CLI and machine mode.
- nide: 运行 → 开始运行 moved from F5 to Ctrl+F5; F5 now starts (and
  continues) the debugger.

## [0.4.0] - 2026-09-07

### Added

- ndb: a CLI debugger for compiled modules. Breakpoints
  (`b <file.n:LINE | LINE | funcName>`), continue, step into/over/out,
  backtrace, frame selection, `info locals`, `p`, source listing `l`,
  disassembly `x`, `catch on|off` (break on throw). Initial stop at the
  first statement (like gdb `start`); stdin EOF behaves like `q`; ndb
  exits with the debugged program's exit code.
- VM: in-process debug hooks (`IDebugHooks` — statement and throw
  checkpoints) plus a read-only frozen-state view (`IVmDebugView`);
  front-end-agnostic interfaces a future DAP adapter or the IDE can
  reuse. Disassembly printing extracted into a shared `Disassembler`
  (ndisasm output byte-identical).
- `.ncu` v1.9: each function records its source file path (cross-file
  breakpoint addressing). The import merge now also copies
  `func.locals`, fixing a pre-existing GC root-set hole where imported
  frames had an empty root set (live objects could be swept).

### Changed

- `.ncu` format floor raised from 8 to 9: older modules are refused
  by the loader and must be recompiled.

## [0.3.0] - 2026-08-31

### Added

- nide: the About dialog now shows the project's GitHub address
  (https://github.com/dliting/nlang) as a blue underlined link; clicking
  it opens the default browser.

### Changed

- Language: `&&` and `||` now short-circuit (the skipped operand is never
  evaluated — no side effects, no throws), matching C/C++/Java/Python
  conventions; results stay `int` `0`/`1`. Operands of `&&`, `||` and `!`
  must now be `int` (float/string operands were previously read as raw
  bits with meaningless truthiness — now a compile error). Old bytecode
  modules containing the removed eager `OP_LogicalAnd`/`OP_LogicalOr`
  instructions must be recompiled.

## [0.2.0] - 2026-08-31

### Added

- nide: **File > Recent** submenu — recent solutions, projects and files,
  most-recently-used first, persisted across sessions. Entries with the same
  file name are disambiguated by parent directory and carry full-path
  tooltips; opening, creating, save-as and rename all feed the list.
- Version management: the repository-root `VERSION` file is now the single
  source of the version — `ncc`/`nvm`/`ndisasm --version`, the IDE About
  dialog, the documentation-site footer and package names all derive from
  it. This changelog is part of that workflow.

## [0.1.0] - 2026-08-30

First public release.

### Added

- Language: a statically-typed scripting language — classes and structs
  with inheritance and `super()`, functions, methods and delegates, type
  aliases, arrays, `List`/`Dict`, `foreach`, `switch`/`enum`, exception
  handling (`try`/`catch`/`finally`/`throw` with built-in exception
  classes), string interpolation, incremental assignment, `assert` and
  `const` locals.
- Toolchain: `ncc` (compile and run; single files and `.nproj` projects),
  `nvm` (bytecode runner), `ndisasm` (bytecode disassembler).
- nide: a Qt5 IDE — solution tree with standalone files, editor, build and
  run, embedded offline documentation viewer, Chinese/English UI.
- Standard library: `math`/`io`/`fs` namespaces and built-in string
  methods.
- Cross-file programming: explicit `import` (single, wildcard and
  precompiled `.ncu` module forms).
- Documentation: a fully offline-capable documentation site and runnable
  `examples/`.
- Windows packaging: portable zip and NSIS installer.

[0.7.0]: https://github.com/dliting/nlang/compare/v0.6.2...v0.7.0
[0.6.2]: https://github.com/dliting/nlang/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/dliting/nlang/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/dliting/nlang/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/dliting/nlang/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/dliting/nlang/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/dliting/nlang/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/dliting/nlang/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/dliting/nlang/releases/tag/v0.1.0
