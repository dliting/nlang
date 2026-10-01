# Standard Library


NLang ships four built-in libraries: `math`, `io`, `fs` (namespace-qualified
free functions) and string methods (receiver-dispatched). The namespace names
are **reserved** — declaring a local, function, class, struct, enum, parameter
or catch variable named `math`, `io` or `fs` is a compile error. Calls use the
qualified name only (`math.sin(x)`); bare names are not in scope (a future
`using`-style keyword may lift this). A namespace name used as a value
(`int x = math;`) fails to resolve — namespaces are not values.

Binding is compiler-intrinsic: qualified calls are recognized at compile
time against the built-in table as standard-library functions and
type-checked; the generated code emits `OP_CallIntrinsic` — no function
records, no host registration.

**Parameter types**: each argument is checked against the declared kind
per the conversion matrix — same-kind passes as-is, matrix-allowed implicit
widening is applied automatically (integer-family and `float` arguments
enter `double` parameters, e.g. `math.sqrt(4)`; narrowing is always an
explicit `as` — `math.absi(1.5)` is a compile error). The single exception
is `io.print`, which accepts string, arrays and all scalar primitives
(converted at the call site); class and enum values need an explicit
`.toString()` before printing (struct arguments are rejected outright —
structs have no `toString`).

### math — 25 functions

The floating-point function family all runs at **double** precision
(since 0.7.5): parameters and return values are `double`; integer and
`float` arguments enter via implicit widening (`math.sqrt(4)` and
`math.sin(1.5f)` both compile).
`sin`/`cos`/`tan` take radians; `asin`/`acos`/`atan` return radians.
`log` is the natural logarithm.
`floor`/`ceil`/`round` return **long** (`round` is half-away-from-zero).

| Function | Signature | Notes |
|----------|-----------|-------|
| sin cos tan asin acos atan | (double) → double | radians |
| atan2 | (double y, double x) → double | C/C++ argument order |
| sqrt pow exp log | (double[,double]) → double | pow(x,y); log = ln |
| absi / absf | (int)→int / (double)→double | absi(INT_MIN) throws |
| mini maxi / minf maxf | (T, T) → T | int pair / double pair |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (double) → long | out-of-int64 or NaN → Exception |
| random | () → double | [0,1), PRNG below |
| srand | (int) → void | reseeds |
| randomi | (int min, int max) → int | inclusive bounds; min > max → Exception |

**PRNG determinism**: `std::mt19937`, seeded from `std::random_device` at
program start. `math.srand(n)` reseeds explicitly — after it, sequences are
fully deterministic and identical across platforms:

```text
random()  = (double)((next() >> 8) * (1.0 / 16777216.0))   // 24-bit mantissa, exact in [0,1)
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // small modular bias, documented
```

No `std::uniform_*_distribution` is used — those are implementation-defined
and not portable.

### io — content IO

| Function | Signature | Notes |
|----------|-----------|-------|
| print | (string\|array\|scalar primitive) → void | stdout + '\n' + flush |
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
| exists / isFile / isDirectory | (string) → bool | never raise |
| size | (string) → int | byte size; regular files only |
| listFiles | (string) → List\<string\> | names only, non-recursive, regular files only, lexicographically sorted |
| makeDirs | (string) → void | mkdir -p (idempotent) |
| remove | (string) → void | file or empty directory |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()` — forward slashes everywhere |

**Error model**: `size`/`listFiles`/`makeDirs`/`remove` raise `IOException` on
failure. The three predicates never raise — a path that cannot be stated
(missing, or inaccessible) simply answers `false`. `remove` on a missing path
is a silent no-op. `size` on a directory or special file raises IOException
(directory "sizes" are filesystem noise).

**join edge semantics** (std::filesystem path append, same as Python
`os.path.join`): a rooted right side (`"/b"`) **replaces** the left side; an
empty right side leaves a trailing separator (`join("a","")` is `"a/"`); an
empty left side yields the right side alone.

**Windows encoding note**: paths and filenames convert through the
process active code page (`generic_string`, file opens). The
command-line tools run with UTF-8 as the active code page (declared
in the tools' embedded manifest, Windows 10 1903+), so non-ASCII
paths and filenames round-trip as UTF-8 —
`io.readFile`/`writeFile`/`appendFile` included. Embedding hosts
that run the VM under the system code page are still bound by that
code page.

### string methods — 18 built-ins

Methods on the string receiver (`s.substring(1)`; literal receivers work:
`"abc".toUpper()`). The method surface is frozen as the future string class's
method list. **Byte semantics** (Go/Lua model): length, substring
and indexOf are byte offsets; UTF-8 byte order equals code point order (so
relational comparison is well-defined); case conversion is ASCII-only.
Code-point access (`charAt`/`charCount`/`foreach char`) is a code-point layer
over the byte core — see [String](string.md) "The char bridge".

| Method | Signature | Notes |
|---------|-----------|-------|
| substring | (start[, end]) → string | end exclusive, defaults to length; out of range → IndexOutOfBoundsException |
| indexOf | (string) → int | first byte offset, -1 if absent |
| startsWith / endsWith / contains | (string) → bool | predicates |
| toUpper / toLower | () → string | ASCII only |
| trim | () → string | strips `" \t\n\r\f\v"` |
| split | (string sep) → List\<string\> | sep must be non-empty (else Exception) |
| replace | (string old, string new) → string | all non-overlapping occurrences; old must be non-empty |
| toInt / toLong | () → int / long | strict whole-string parse; malformed → Exception |
| toFloat / toDouble | () → float / double | strict whole-string parse; malformed → Exception |
| toBool | () → bool | strict parse of `"true"`/`"false"` |
| toChar | () → char | strict decimal code-point parse; invalid scalar → Exception |
| charAt | (int byteIndex) → char | the code point **starting at** that byte offset; out of range → IndexOutOfBoundsException, continuation byte → Exception |
| charCount | () → int | code-point count (contrast `length()`, bytes); invalid sequence → Exception |

String subscripting (`s[i]`) returns byte i as a `ubyte` (out of range
throws the runtime error `string index out of range`) — byte access goes
through the subscript, code-point access through `charAt`.

### Streams — ByteStream and FileStream

Two built-in stream classes provide binary serialization: `ByteStream` reads
and writes over an in-memory buffer, `FileStream` lands on a disk file. The
method surface is identical (FileStream has no `reset()`).

```nlang
ByteStream bs = new ByteStream();
bs.writeInt(1);
bs.writeDouble(0.5);
bs.reset();                  // rewinds the cursor, keeps the buffer
double d = bs.readDouble();
```

| Method | Wire form | Notes |
|---------|-----------|-------|
| writeInt / readInt | 4 bytes | int32 |
| writeFloat / readFloat | 4 bytes | float |
| writeLong / readLong | 8 bytes | long (narrower integer arguments enter via implicit widening) |
| writeDouble / readDouble | 8 bytes | double (float arguments widen losslessly) |
| writeString / readString | length-prefixed bytes | string |
| writeStruct / readStruct | recursive fields | write takes the struct value; read takes the type name — `bs.readStruct("Point")` |
| writeObject / readObject | recursive reference graph | class instances; read likewise by type name |
| length / position | — | byte count / current cursor |
| reset (ByteStream only) | — | rewinds the cursor, keeps the buffer |
| close | — | releases the FileStream's file handle |

**FileStream construction**: `new FileStream(path, mode)`, where mode is
`"w"` (create/truncate), `"a"` (create/append) or `"r"` (read-only; a
missing file throws a runtime error). An invalid mode throws a runtime
error.

**Argument types**: the scalar write methods check each argument per the
conversion matrix — narrower integer and `float` arguments implicitly widen
into the 8-byte slots (the same rule as `math.sqrt` arguments); a `ulong`
argument exceeds the long range and needs an explicit `as long`; `char`
and string arguments are compile errors for the numeric methods.

**EOS semantics**: reading from a stream whose cursor is already at the end
throws a runtime error (it does not return 0).

### Exception mapping

- **IOException**: `io.readFile`/`writeFile`/`appendFile` and every raising
  `fs.*` function.
- **IndexOutOfBoundsException**: substring range errors.
- **base Exception**: argument/range/parse errors — `clampi`/`clampf`
  lo>hi, `randomi` min>max, `split`/`replace` empty argument,
  `toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar` malformed input,
  `charAt`/`charCount` invalid UTF-8 sequence, `floor`/`ceil`/`round`/`absi`
  overflow. There is no
  `IllegalArgumentException` built-in; narrowing these to a dedicated
  subclass later is source-compatible for `catch (Exception)` callers.

### Future directions

- `using`-style keyword to open up unqualified names
- string class-ification (method surface frozen above)
- `IllegalArgumentException` built-in subclass
- package manager
