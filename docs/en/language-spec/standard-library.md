# Standard Library


NLang ships four built-in libraries: `math`, `io`, `fs` (namespace-qualified
free functions) and string methods (receiver-dispatched). The namespace names
are **reserved** — declaring a local, function, class, struct, enum, parameter
or catch variable named `math`, `io` or `fs` is a compile error. Calls use the
qualified name only (`math.sin(x)`); bare names are not in scope (a future
`using`-style keyword may lift this). A namespace name used as a value
(`int x = math;`) fails to resolve — namespaces are not values.

The **signatures** (parameter kinds, arity, return type) live in the
`stdlib/*.n` declarations shipped with the toolchain and reach the compiler
and editor (completion, hover, go-to-definition) through the language
service's symbol index. The runtime implementation is currently built into
the VM: qualified calls are recognized at compile time against
`kStdLibTable` (namespace + function name → intrinsic id) and type-checked;
the generated code emits `OP_CallIntrinsic` — no runtime function records,
no host registration. This table is the last hardcoded piece of the standard
library and will be replaced by the native dynamic-loading mechanism,
making the standard library identical in shape to third-party libraries;
see "Libraries and search paths" below for the directory rules.

**Parameter types**: exact match against the declared kind; the only automatic
conversion is int→float widening (`math.sqrt(4)` compiles). float→int is never
implicit (`math.absi(1.5)` is a compile error). The single exception is
`io.print`, which accepts string|int|float (converted at the call site);
class and enum values need an explicit `.toString()` before printing
(struct arguments are rejected outright — structs have no `toString`).

### math — 25 functions

`sin`/`cos`/`tan` take radians; `asin`/`acos`/`atan` return radians.
`log` is the natural logarithm.
`floor`/`ceil`/`round` return int (`round` is half-away-from-zero).

| Function | Signature | Notes |
|----------|-----------|-------|
| sin cos tan asin acos atan | (float) → float | radians |
| atan2 | (float y, float x) → float | C/C++ argument order |
| sqrt pow exp log | see below | pow(x,y); log = ln |
| absi / absf | (int)→int / (float)→float | absi(INT_MIN) throws |
| mini maxi / minf maxf | (T, T) → T | |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (float) → int | out-of-int32 or NaN → Exception |
| random | () → float | [0,1), PRNG below |
| srand | (int) → void | reseeds |
| randomi | (int min, int max) → int | inclusive bounds; min > max → Exception |

**PRNG determinism**: `std::mt19937`, seeded from `std::random_device` at
program start. `math.srand(n)` reseeds explicitly — after it, sequences are
fully deterministic and identical across platforms:

```text
random()  = (float)((next() >> 8) * (1.0f / 16777216.0f))   // 24-bit mantissa, exact in [0,1)
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // small modular bias, documented
```

No `std::uniform_*_distribution` is used — those are implementation-defined
and not portable.

### io — content IO

| Function | Signature | Notes |
|----------|-----------|-------|
| print | (string\|int\|float) → void | stdout + '\n' + flush |
| readLine | () → string | stdin line, trailing '\r' stripped |
| readFile | (string) → string | whole file as bytes; failure → IOException |
| writeFile | (string path, string s) → void | create/truncate; failure → IOException |
| appendFile | (string path, string s) → void | create/append; failure → IOException |

**EOF semantics of readLine**: EOF and an empty input line both return `""` —
indistinguishable by design (same as C++ `std::getline`). Programs that must
detect end of input should terminate on sentinel content, not on an empty
line. `io.print` takes exactly one argument; print several values with
several calls.

`readFile` enforces a 16 MiB cap (the same bound the deserializer applies to
untrusted length prefixes); larger files raise IOException.

### fs — names, directories, metadata

`fs` never reads or writes content — content belongs to `io`. The split is an
operation principle: io = all content (console + disk text, later stream
classes), fs = namespace/directory/metadata.

| Function | Signature | Notes |
|----------|-----------|-------|
| exists / isFile / isDirectory | (string) → int | 0/1; never raise |
| size | (string) → int | byte size; regular files only |
| listFiles | (string) → List\<string\> | names only, non-recursive, regular files only, lexicographically sorted |
| makeDirs | (string) → void | mkdir -p (idempotent) |
| remove | (string) → void | file or empty directory |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()` — forward slashes everywhere |

**Error model**: `size`/`listFiles`/`makeDirs`/`remove` raise `IOException` on
failure. The three predicates never raise — a path that cannot be stated
(missing, or inaccessible) simply answers 0. `remove` on a missing path is a
silent no-op. `size` on a directory or special file raises IOException
(directory "sizes" are filesystem noise).

**join edge semantics** (std::filesystem path append, same as Python
`os.path.join`): a rooted right side (`"/b"`) **replaces** the left side; an
empty right side leaves a trailing separator (`join("a","")` is `"a/"`); an
empty left side yields the right side alone.

**Windows limitation**: paths and filenames convert through the active code
page (`generic_string`, file opens); non-ASCII filenames may not round-trip
as UTF-8. The same limitation applies to `io.readFile`/`writeFile`/
`appendFile`.

### string methods — 12 built-in

Methods on the string receiver (`s.substring(1)`; literal receivers work:
`"abc".toUpper()`). The method surface is frozen as the future string class's
method list. **Byte semantics** (Go/Lua model): length, substring
and indexOf are byte offsets; UTF-8 byte order equals code point order (so
relational comparison is well-defined); case conversion is ASCII-only.
String subscripting (`s[i]`) is **not supported** — there is no byte-access
operator on strings (a subscript is a compile-time rejection: "string
does not support subscript access"; see Known Limitations).

| Method | Signature | Notes |
|---------|-----------|-------|
| substring | (start[, end]) → string | end exclusive, defaults to length; out of range → IndexOutOfBoundsException |
| indexOf | (string) → int | first byte offset, -1 if absent |
| startsWith / endsWith / contains | (string) → int | 0/1 |
| toUpper / toLower | () → string | ASCII only |
| trim | () → string | strips `" \t\n\r\f\v"` |
| split | (string sep) → List\<string\> | sep must be non-empty (else Exception) |
| replace | (string old, string new) → string | all non-overlapping occurrences; old must be non-empty |
| toInt / toFloat | () → int / float | strict whole-string parse; malformed → Exception |

### Libraries and search paths

NLang libraries are carried by **`.n` source files**: the standard
`math.n`/`io.n`/`fs.n` ship with the toolchain, and a third-party library is
just a directory of `.n` files (optionally alongside native dynamic
libraries). A function implemented outside NLang is declared with the
`native` keyword (`native void print(any s);`) — such a declaration carries
only the signature and documentation, with no body; an ordinary function
without `native` is a readable, editable NLang implementation. A library may
contain both (a hybrid library, as in Python/Java/C#).

The **search path** determines where the compiler looks for imported `.n`
files and where native dynamic libraries are loaded at run time — the
standard library and third-party libraries, compile-time discovery and
run-time loading all use the **same set of directories**. Directories are
assembled in the following order, earlier ones winning; duplicates keep only
the first occurrence (paths are normalized, and case-folded on Windows):

1. command-line `-I <dir>` (highest priority; repeatable);
2. `<ImportPaths>` in the `.nproj` project file;
3. the project / source / module directory (local);
4. the `NLANG_PATH` environment variable (`;` on Windows, `:` on POSIX);
5. system defaults: the standard-library directory, the executable directory,
   the current directory (lowest).

Command-line usage:

```text
ncc build app.n -o app.nmod -I C:\libs\mylib
nvm app.nmod -I C:\libs\mylib
ndb --machine app.nmod -I C:\libs\mylib
```

A project persists its search dirs in `.nproj` under `<ImportPaths>` (paths
are stored relative to the project file):

```xml
<Project name="app">
  <Sources><File path="src/main.n"/></Sources>
  <ImportPaths><Dir path="../libs"/></ImportPaths>
</Project>
```

**Configuring in nide**: global search paths live under Tools → Options →
Library search paths, and project-level paths under Project → Properties →
Library search paths; both support add, remove, move up/down and browse,
with project paths taking precedence over global ones. A change rebuilds the
symbol index, refreshing completion and go-to-definition.

**Viewing and jumping to source**: use Go to Definition (F12) on a library
symbol to open its `.n` — a `native` declaration shows the signature and
documentation, an ordinary function shows an editable implementation;
rebuild after editing to pick up the change.

### Exception mapping

- **IOException**: `io.readFile`/`writeFile`/`appendFile` and every raising
  `fs.*` function.
- **IndexOutOfBoundsException**: substring range errors.
- **base Exception**: argument/range/parse errors — `clampi`/`clampf`
  lo>hi, `randomi` min>max, `split`/`replace` empty argument,
  `toInt`/`toFloat` malformed input, `floor`/`ceil`/`round`/`absi`
  overflow. There is no
  `IllegalArgumentException` built-in; narrowing these to a dedicated
  subclass later is source-compatible for `catch (Exception)` callers.

### Future directions

- retire `kStdLibTable`: move the standard-library runtime to native dynamic
  libraries, fully matching third-party libraries (signatures already in
  `stdlib/*.n`)
- hybrid (native + NLang) third-party libraries with automatic recompilation
  after edits
- `using`-style keyword to open up unqualified names
- string class-ification (method surface frozen above)
- Stream family unifying byte streams / file streams under io
- `IllegalArgumentException` built-in subclass
- package manager
