# 14. The io Package and Streams

The `io` package provides console output, console input and
whole-file content transfer. The stream classes `ByteStream`
and `FileStream` provide binary serialization of values,
structs and class graphs. §14.1 fixes the package form and the
argument admission rule that governs every package call;
§14.2–§14.4 define the io functions; the stream classes
(§14.5) and the byte-level encoding of stream values (§14.6)
are defined on their own page.

## 14.1 Package Form

`io` is a standard library package (§3.9.4): `import io;` brings
it into scope and every function is called through the package
qualifier (`io.print(s)`). The package name is single-segment
(§3.3). Its declarations are the standard library's `.n`
source; the output trio, the input functions and the file
functions are `native` functions (§7.5), and the typed readers
are NLang source wrappers over `readToken` and the string
conversion methods (§16.2).

**Argument admission.** A call to a package function binds each
argument against the declared formal type (§7.3); the implicit
conversion to `string` of §4.4 does not apply to package
arguments. A `string` formal of a package function accepts only
a `string` argument; any other type is a compile-time error. A
call to a user-declared function keeps the §4.4 relation:
`emit(42)` compiles where `emit` declares a `string` parameter,
but `io.writeFile(42, "x")` does not.

The exception is the output trio `io.print`, `io.write` and
`io.eprint` (§14.2). The trio additionally accepts arguments
that render as text (§5.3): scalar primitives, arrays and
function values. A class, interface or enumeration value is
rejected — a class or interface value calls `.toString()` first
(§11.2), and an enumeration variable likewise; an enumeration
member constant is accepted and prints its member name (§5.3).
A struct value is rejected outright: structs have no
`toString` (§11.2). A `null` argument is rejected: it would
print as `0`. The exception is keyed to these three names under
the `io` package path; a user package that shadows `io`
inherits it on exactly those names.

## 14.2 Console Output

- `io.print(s)` writes the text of `s` to standard output,
  followed by a newline.
- `io.write(s)` writes the text of `s` to standard output with
  no trailing newline.
- `io.eprint(s)` writes the text of `s` to standard error,
  followed by a newline.

Each function takes one argument under the trio admission of
§14.1 — printing several values takes several calls — and a
non-string argument is rendered as text by §5.3 and §11.2
before it is written. `io.write` is the prompt shape:
`io.write("Name: ")` leaves the cursor on the line, so a typed
reply lands after the prompt; `io.eprint` mirrors `io.print` on
standard error, keeping diagnostics separable from normal
output. The destination is the host's output channel; the
effect of a failed host write is *unspecified*. Inside a debug
session the two streams merge into the session's single output
view.

## 14.3 Console Input

The input functions read the program's standard input through
one shared cursor: line reads, token reads and code point reads
interleave over the same position, and the cursor advances by
exactly what a call consumes. Each input function is a blocking
native function: it declares a blocking section (§10.1), so the
calling context parks and the carrier lock is released while
the call waits.

- `io.hasInput()` returns `true` when input remains to be read
  and `false` at end of input. It never raises. With no input
  channel it returns `true` until a read raises `IOException`.
  The answer is exact for a redirected file or pipe; on a
  console it reports only the input the host has already
  buffered, and in a debug session only the buffered lines.
- `io.readLine()` returns the next line without its terminator;
  a trailing carriage return is stripped. At end of input it
  returns `""`: an empty line and the end of input are not
  distinguishable by the result, and `hasInput()` separates
  them. With no input channel it raises `IOException` (§8.3).
- `io.readToken()` returns the next whitespace-delimited token,
  skipping whitespace across lines. At end of input it raises
  `IOException`; with no input channel likewise.
- `io.readChar()` returns the next non-whitespace code point,
  decoded from the input's Unicode Transformation Format
  (UTF-8) bytes; leading whitespace is skipped, as for
  `readToken`. It yields literal characters — input `x` yields
  `'x'` — deliberately unlike the numeric parse of `toChar`
  (§16.2), where `"120"` yields `'x'`. At end of input it
  raises `IOException`; with no input channel likewise. A byte
  sequence that is not valid UTF-8 raises `Exception` (§8.3),
  not `IOException`.
- `io.readInt()`, `io.readLong()`, `io.readFloat()`,
  `io.readDouble()` and `io.readBool()` read the next token and
  convert it with the matching string method (§16.2): end of
  input raises `IOException` (from `readToken`), and a
  malformed token fails exactly as that string method fails.

Mixed reads follow the C++ `cin >>` and `getline` combination:
a token read consumes up to the token's end, and the following
`readLine()` returns the remainder of that line (leading
whitespace included); only when the line is exhausted does it
take the next one. For input `"10 20\n30\n"`: `readInt()`
yields 10, `readInt()` yields 20, `readLine()` yields `""`
(the empty remainder), `readLine()` yields `"30"`, and the
next read returns `""` (end of input).

```nlang
while (io.hasInput()) {
    string line = io.readLine();
    io.print(line);
}
```

> Implementation note. When the program blocks for input and
> both standard input and standard output are consoles, the
> host prints a `"> "` prompt before waiting — the read-eval-print
> loop (REPL) convention of Python and sqlite3, for a direct
> `nvm` run and for `ncc`'s in-process run alike; the nide
> debug terminal draws the same token itself in line mode. The
> prompt is not part of the language, and both streams stay
> byte-exact whenever either end is a pipe or a redirection.

## 14.4 File Content IO

- `io.readFile(path)` returns the whole content of the file at
  `path` as a `string` of bytes (§4.3). It raises `IOException`
  (§8.3) when the file cannot be opened, its size cannot be
  determined, the size exceeds 16 MiB, or the read fails
  partway.
- `io.writeFile(path, content)` creates the file or truncates
  an existing one and writes `content`; `io.appendFile(path,
  content)` creates the file when needed and appends `content`.
  Both raise `IOException` when the file cannot be opened or
  written.

`path` is a host path in the platform's native form; the io
package defines no path syntax of its own (the `fs` package of
chapter 15 does). The content is bytes: the io functions
impose no text encoding on what they read or write.
