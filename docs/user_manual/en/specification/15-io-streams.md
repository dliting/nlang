# 15. The io Package and Streams

The `io` package provides console output, console input and
whole-file content transfer. The stream classes `ByteStream`
and `FileStream` provide binary serialization of values,
structs and class graphs. §15.1 fixes the package form and the
argument admission rule that governs every package call;
§15.2–§15.4 define the io functions; §15.5 defines the stream
classes; §15.6 fixes the byte-level encoding of stream values.

## 15.1 Package Form

`io` is a standard library package (§12.4): `import io;` brings
it into scope and every function is called through the package
qualifier (`io.print(s)`). The package name is single-segment
(§3.3). Its declarations are the standard library's `.n`
source; the output trio, the input functions and the file
functions are `native` functions (§7.5), and the typed readers
are NLang source wrappers over `readToken` and the string
conversion methods (§13.3).

**Argument admission.** A call to a package function binds each
argument against the declared formal type (§7.3); the implicit
conversion to `string` of §4.12 does not apply to package
arguments. A `string` formal of a package function accepts only
a `string` argument; any other type is a compile-time error. A
call to a user-declared function keeps the §4.12 relation:
`emit(42)` compiles where `emit` declares a `string` parameter,
but `io.writeFile(42, "x")` does not.

The exception is the output trio `io.print`, `io.write` and
`io.eprint` (§15.2). The trio additionally accepts arguments
that render as text (§5.3): scalar primitives, arrays and
function values. A class, interface or enumeration value is
rejected — a class or interface value calls `.toString()` first
(§13.2), and an enumeration variable likewise; an enumeration
member constant is accepted and prints its member name (§5.3).
A struct value is rejected outright: structs have no
`toString` (§13.2). A `null` argument is rejected: it would
print as `0`. The exception is keyed to these three names under
the `io` package path; a user package that shadows `io`
inherits it on exactly those names.

## 15.2 Console Output

- `io.print(s)` writes the text of `s` to standard output,
  followed by a newline.
- `io.write(s)` writes the text of `s` to standard output with
  no trailing newline.
- `io.eprint(s)` writes the text of `s` to standard error,
  followed by a newline.

Each function takes one argument under the trio admission of
§15.1; a non-string argument is rendered as text by §5.3 and
§13.2 before it is written. The destination is the host's
output channel; the effect of a failed host write is
*unspecified*.

## 15.3 Console Input

The input functions read the program's standard input through
one shared cursor: line reads, token reads and code point reads
interleave over the same position, and the cursor advances by
exactly what a call consumes. Each input function is a blocking
native function: it declares a blocking section (§11.1), so the
calling context parks and the carrier lock is released while
the call waits.

- `io.hasInput()` returns `true` when input remains to be read
  and `false` at end of input. It never raises. With no input
  channel it returns `true` until a read raises `IOException`.
- `io.readLine()` returns the next line without its terminator;
  a trailing carriage return is stripped. At end of input it
  returns `""`: an empty line and the end of input are not
  distinguishable by the result, and `hasInput()` separates
  them. With no input channel it raises `IOException` (§9.3).
- `io.readToken()` returns the next whitespace-delimited token.
  At end of input it raises `IOException`; with no input
  channel likewise.
- `io.readChar()` returns the next non-whitespace code point,
  decoded from the input's Unicode Transformation Format
  (UTF-8) bytes; leading whitespace is skipped, as for
  `readToken`. At end of input it raises `IOException`; with no
  input channel likewise. A byte sequence that is not valid
  UTF-8 raises `Exception` (§9.3), not `IOException`.
- `io.readInt()`, `io.readLong()`, `io.readFloat()`,
  `io.readDouble()` and `io.readBool()` read the next token and
  convert it with the matching string method (§13.3): end of
  input raises `IOException` (from `readToken`), and a
  malformed token fails exactly as that string method fails.

> Implementation note. When the program blocks for input and
> both standard input and standard output are consoles, the
> host prints a `"> "` prompt before waiting; the prompt is not
> part of the language.

## 15.4 File Content IO

- `io.readFile(path)` returns the whole content of the file at
  `path` as a `string` of bytes (§4.4). It raises `IOException`
  (§9.3) when the file cannot be opened, its size cannot be
  determined, the size exceeds 16 MiB, or the read fails
  partway.
- `io.writeFile(path, content)` creates the file or truncates
  an existing one and writes `content`; `io.appendFile(path,
  content)` creates the file when needed and appends `content`.
  Both raise `IOException` when the file cannot be opened or
  written.

`path` is a host path in the platform's native form; the io
package defines no path syntax of its own (the `fs` package of
chapter 16 does). The content is bytes: the io functions
impose no text encoding on what they read or write.

## 15.5 ByteStream and FileStream

`ByteStream` and `FileStream` are built-in handle classes
(§13.1): `ByteStream` reads and writes an in-memory buffer,
`FileStream` a disk file. `ByteStream()` constructs an empty
buffer. `FileStream(path, mode)` opens the file: mode `"w"`
creates or truncates, `"a"` creates or appends, and `"r"`
opens for reading only. An unassigned stream handle is null; a
method call through a null handle raises
`NullPointerException` (§9.3). Every other stream failure is a
*host-level* run-time error (§9.1): no `catch` clause handles
it and the program ends. The failure sites are an invalid mode,
a failed open (including `"r"` on a missing file), access
through a closed or invalid handle, a write to a read-only
stream or a read from a write-only stream, a read past the end
of the stream, a struct or object read naming a type the
program does not declare, and a struct or class value with an
array or function-typed field written by `writeStruct` or
`writeObject`.

The method surface is identical except where noted:

| Method | Effect |
| --- | --- |
| `writeInt(x)` / `readInt() → int` | 4-byte integer |
| `writeFloat(x)` / `readFloat() → float` | 4-byte float |
| `writeLong(x)` / `readLong() → long` | 8-byte integer |
| `writeDouble(x)` / `readDouble() → double` | 8-byte float |
| `writeString(s)` / `readString() → string` | a 4-byte length, then the bytes |
| `writeStruct(v)` / `readStruct(name)` | the struct's fields, recursively; the read names the type as a string and yields a value of that type |
| `writeObject(c)` / `readObject(name)` | the class graph, recursively; the read names the type as a string and yields a value of that type |
| `length() → int` | the total byte count of the stream |
| `position() → int` | the current cursor |
| `reset()` | rewinds the cursor to 0 and clears the object-identity state; the buffer is kept (`ByteStream` only) |
| `close()` | releases the buffer or the file handle; the handle becomes invalid |

The scalar write methods admit arguments by width. The 8-byte
writers take any value §4.12 assigns implicitly to their slot
type: an `int` enters `writeLong`, and a `float` or an integer
enters `writeDouble`, the conversion applied at the call site.
The 4-byte writers are exact: `writeInt` takes an integer of at
most 32 bits, and `writeFloat` takes exactly a `float`. A
rejected argument is a compile-time error, as is a `char` or
`string` argument to a numeric method.

A read consumes the stream in write order; reading past the end
raises the host-level error rather than yielding a value. A
`null` class reference written by `writeObject` reads back as
`null`. Within one `writeObject` graph, shared references are
preserved: two fields naming the same instance read back as one
instance. `close()` on a `FileStream` releases the
operating-system handle and invalidates the stream handle;
later calls on the same reference report the invalid-handle
error. The byte-level encoding of the values — byte order and
the struct and object forms — is fixed by §15.6.

## 15.6 Stream Encoding

This section fixes the byte-level encoding exchanged by the
write and read methods of §15.5. The encoding is the same for
`ByteStream` and `FileStream`. The stream carries no header:
each method appends its record at the cursor, and a stream
written by successive calls is the concatenation of those
records.

**Byte order.** Every multi-byte value is written in
little-endian order, least significant byte first. Integers
are written in two's-complement form at the width of their
type, and `float` and `double` are written as the IEEE 754
binary32 and binary64 encodings of the value (§4.2).

**Scalar methods.** `writeInt` and `writeFloat` append 4 bytes
and `writeLong` and `writeDouble` append 8: the encoding of
the value in the method's slot type. The implicit widening of
§15.5 is applied at the call site, so the bytes always carry
the slot type's width — `writeLong(42)` appends the 8 bytes of
the `long` value 42.

**Strings.** `writeString(s)` appends a 4-byte signed length —
the byte count of `s` — followed by exactly those bytes. The
bytes are whatever the string holds (§4.4); the encoding
imposes no text form on them. `readString` treats a negative
length, a length above 16 MiB, and a length that runs past the
end of the stream alike: each is a host-level run-time error
(§9.1).

**Structs.** `writeStruct(v)` appends no header and no type
name: the fields of `v`'s type in declaration order, each
encoded by its own type —

| Field type | Encoding |
|---|---|
| a scalar primitive (§4.2) | the value at its own width: 1 byte for `byte` and `ubyte`, 2 for `short` and `ushort`, 4 for `int`, `uint`, `float`, `bool` and `char`, 8 for `long`, `ulong` and `double`; a `bool` is `0` or `1` and a `char` is its code point |
| `string` | the `writeString` form |
| a struct | the same struct encoding, inline and recursive |
| a class | the class record below |

`readStruct(name)` consumes exactly that byte sequence against
the named type: the stream and the program must agree on the
type's shape. Array and function-typed fields are rejected on
the write side (§15.5).

**Class records.** `writeObject(c)` appends one record per
class-typed value, tagged by its first byte:

| Tag | Value | Encoding |
|---|---|---|
| `0` | `null` | the tag byte alone |
| `1` | an instance written for the first time | the tag byte, a 4-byte unsigned length, the class's qualified name (§3.6), then the instance's fields — inherited fields first, base class to derived class — each encoded as in the struct table |
| `2` | an instance already written on this stream | the tag byte and a 4-byte object id |

Object ids are assigned in write order, counting from 0, one
id per tag-`1` record. `readObject(name)` re-creates the
graph: a tag-`1` record's name must name a class the program
declares, and that class must be the named type or a subclass
of it (§8.1); the instance is registered under the next id
before its fields are read, so a cycle resolves to the same
instance. A tag other than `0`, `1` or `2`, an object id not
yet read, a name the program does not declare, and a class
that is neither the named type nor a subclass of it are
host-level run-time errors (§9.1).

**Identity state.** The id numbering belongs to the stream,
not to the call: successive `writeObject` calls on one stream
continue the numbering, and successive `readObject` calls
rebuild one shared pairing, so an instance written by an
earlier call can be referenced by a later one. `reset()` and
`close()` clear the state on both sides (§15.5).

**Depth.** A value nested more than 64 levels deep — struct
within struct, class within class, or either within the other
— raises a host-level run-time error (§9.1), on the write side
and on the read side alike.
