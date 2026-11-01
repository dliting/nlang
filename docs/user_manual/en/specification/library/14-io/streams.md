# 14.5 ByteStream and FileStream

`ByteStream` and `FileStream` are built-in handle classes
(§11.1): `ByteStream` reads and writes an in-memory buffer,
`FileStream` a disk file. `ByteStream()` constructs an empty
buffer. `FileStream(path, mode)` opens the file: mode `"w"`
creates or truncates, `"a"` creates or appends, and `"r"`
opens for reading only; a direct-initialization declaration
works too (`FileStream fs("data.bin", "w");`, §4.8.4). An
unassigned stream handle is null; a method call through a null
handle raises `NullPointerException` (§8.3). Every other stream
failure is a *host-level* run-time error (§8.1): no `catch`
clause handles it and the program ends. The failure sites are
an invalid mode, a failed open (including `"r"` on a missing
file), access through a closed or invalid handle, a write to a
read-only stream or a read from a write-only stream, a read
past the end of the stream, a struct or object read naming a
type the program does not declare, and a struct or class value
with an array or function-typed field written by `writeStruct`
or `writeObject`.

```nlang
ByteStream bs = ByteStream();
bs.writeInt(1);
bs.writeDouble(0.5);
bs.reset();                  // rewinds the cursor, keeps the buffer
int n = bs.readInt();        // 1
double d = bs.readDouble();  // 0.5
```

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
writers take any value §4.4 assigns implicitly to their slot
type: an `int` enters `writeLong`, and a `float` or an integer
enters `writeDouble`, the conversion applied at the call site;
a `ulong` exceeds the `long` range and needs an explicit
`as long`. The 4-byte writers are exact: `writeInt` takes an
integer of at most 32 bits, and `writeFloat` takes exactly a
`float`. A rejected argument is a compile-time error, as is a
`char` or `string` argument to a numeric method.

A read consumes the stream in write order — the example reads
the int back before the double — and reading past the end
raises the host-level error rather than yielding a value. A
`null` class reference written by `writeObject` reads back as
`null`. Within one `writeObject` graph, shared references are
preserved: two fields naming the same instance read back as one
instance. `close()` on a `FileStream` releases the
operating-system handle and invalidates the stream handle;
later calls on the same reference report the invalid-handle
error. The byte-level encoding of the values — byte order and
the struct and object forms — is fixed by §14.6.

## 14.6 Stream Encoding

This section fixes the byte-level encoding exchanged by the
write and read methods of §14.5. The encoding is the same for
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
§14.5 is applied at the call site, so the bytes always carry
the slot type's width — `writeLong(42)` appends the 8 bytes of
the `long` value 42.

**Strings.** `writeString(s)` appends a 4-byte signed length —
the byte count of `s` — followed by exactly those bytes. The
bytes are whatever the string holds (§4.3); the encoding
imposes no text form on them. `readString` treats a negative
length, a length above 16 MiB, and a length that runs past the
end of the stream alike: each is a host-level run-time error
(§8.1).

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
the write side (§14.5).

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
of it (§4.8.5); the instance is registered under the next id
before its fields are read, so a cycle resolves to the same
instance. A tag other than `0`, `1` or `2`, an object id not
yet read, a name the program does not declare, and a class
that is neither the named type nor a subclass of it are
host-level run-time errors (§8.1).

**Identity state.** The id numbering belongs to the stream,
not to the call: successive `writeObject` calls on one stream
continue the numbering, and successive `readObject` calls
rebuild one shared pairing, so an instance written by an
earlier call can be referenced by a later one. `reset()` and
`close()` clear the state on both sides (§14.5).

**Depth.** A value nested more than 64 levels deep — struct
within struct, class within class, or either within the other
— raises a host-level run-time error (§8.1), on the write side
and on the read side alike.
