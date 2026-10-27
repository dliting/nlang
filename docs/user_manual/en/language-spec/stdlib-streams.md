# Streams — ByteStream and FileStream

The normative rules are in [specification §15.5](../specification/15-io-streams.md).

Two built-in stream classes provide binary serialization: `ByteStream` reads
and writes over an in-memory buffer, `FileStream` lands on a disk file. The
method surface is identical (FileStream has no `reset()`).

```nlang
ByteStream bs = ByteStream();
bs.writeInt(1);
bs.writeDouble(0.5);
bs.reset();                  // rewinds the cursor, keeps the buffer
int n = bs.readInt();        // 1
double d = bs.readDouble();  // 0.5
```

Reads consume the stream in write order — the example reads the int back
before the double.

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
| close | — | closes the stream (on FileStream, releases the file handle); later calls raise a runtime error |

**FileStream construction**: `FileStream(path, mode)`, where mode is
`"w"` (create/truncate), `"a"` (create/append) or `"r"` (read-only; a
missing file throws a runtime error). An invalid mode throws a runtime
error. Direct-init declarations work too: `FileStream fs("data.bin", "w");`
(see [Class](class.md) "Creating instances"). An unassigned stream
reference is null; calling a method on it throws a `null reference error`
at run time (same semantics as the containers in [Built-in Generic
Classes](builtin-generic-classes.md)).

**Argument types**: the scalar write methods check each argument per the
conversion matrix — narrower integer and `float` arguments implicitly widen
into the 8-byte slots (the same rule as `math.sqrt` arguments); a `ulong`
argument exceeds the long range and needs an explicit `as long`; `char`
and string arguments are compile errors for the numeric methods.

**end of stream (EOS) semantics**: reading from a stream whose cursor is already at the end
throws a runtime error (it does not return 0).
