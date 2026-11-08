# 20. Library Exceptions

This chapter maps the failures of library operations — the
built-in classes and methods of chapters 11, 12 and 17 and the
packages of chapters 13, 14, 15, 16, 18, 19, 21 and 22 — to the exception
classes of §8.3. §20.1 is
the map; §20.2 fixes the conventions it rests on. The
semantics of every site live in the chapter that defines the
operation; this chapter adds no failure behavior of its own.

## 20.1 Raise Map by Unit

| Unit | Class | Sites |
| --- | --- | --- |
| `string` (§17.2) | `IndexOutOfBoundsException` | `substring` range errors; `charAt` out of range |
| `string` (§17.2) | `Exception` | `split` or `replace` with an empty argument; `toInt`, `toLong`, `toFloat`, `toDouble`, `toBool` or `toChar` on a malformed string; `charAt` on a continuation byte; `charCount` on an invalid Unicode Transformation Format (UTF-8) sequence |
| `List` (§12.1) | `IndexOutOfBoundsException` | `get`, `set` or `removeAt` out of range |
| `Dict` (§12.2) | `Exception` | `get` with no entry for the key |
| `math` (§13.4) | `Exception` | `absi` of the most negative `int`; `clampi` or `clampf` with `lo > hi`; `randomi` with `min > max`; `floor`, `ceil` or `round` on a NaN or a value outside the `long` range |
| `io` (§14.3, §14.4) | `IOException` | `readLine` with no input channel; `readToken` or `readChar` at end of input or with no input channel (a session without stdin); the typed readers at end of input or with no input channel; `readFile`, `writeFile` or `appendFile` failure |
| `io` (§14.3) | `Exception` | `readChar` on a byte sequence that is not valid Unicode Transformation Format (UTF-8); a typed reader on a malformed token (the string method's failure, §17.2) |
| streams (§14.6) | `NullPointerException` | a method call through a null stream handle |
| `system` (§15.2) | `DateTimeException` | `DateTime.parse` on malformed text or an out-of-range calendar field |
| `fs` (§16.3) | `IOException` | `size`, `listFiles`, `makeDirs` or `remove` failure |
| `fs` (§16.4) | `NullPointerException` | a null path to `readBytes` or `readFloats` |
| `fs` (§16.4) | `IOException` | a `readBytes` or `readFloats` open or read failure (a file above the 16 MiB bound included) |
| `fs` (§16.4) | `Exception` | `readFloats` on a byte count that is not a multiple of four |
| `concurrent` (§10.4) | `ChannelClosedException` | a send or receive on a closed channel |
| `concurrent` (§10.4) | `Exception` | construction with a negative capacity |
| `tensor` (§19.6) | `NullPointerException` | a null array argument |
| `tensor` (§19.6) | `IndexOutOfBoundsException` | a creation length past the bound of §4.10; a negative `reshape` dimension |
| `tensor` (§19.6) | `DivByZeroException` | integer `div` by zero |
| `tensor` (§19.6) | `Exception` | `arange` with a zero or non-advancing step, or a range past the bound; a length or shape mismatch; `max`, `min` or `mean` on an empty array |
| `net` (§21.2–§21.4) | `TimeoutException` | `accept`, `connect`, `read`, `readLine`, `readExactly`, `recv`, `recvFrom` or `ping` ending its wait at the deadline |
| `net` (§21.2–§21.4) | `SocketException` | bind, connect or option-setter platform failures; an unresolvable host; a closed socket; a second operation parked on the same socket direction; `peerAddress` or `peerPort` on an unconnected `UdpSocket`; `readExactly` on a stream that ends early |
| `url` (§22.1–§22.4) | `UrlException` | `Url.parse` on a syntax error or an over-long reference; `resolve` on a non-absolute base; `percentDecode` or `formDecode` on a malformed escape (`queryParams` through `formDecode` included) |

A call through a `null` receiver raises
`NullPointerException` in every unit (§11.2); the rows list
only the sites the defining chapters call out besides it.

## 20.2 Conventions

- The library declares five exception classes of its own:
  `ChannelClosedException`, a direct subclass of `Exception`,
  declared by the `concurrent` package; `SocketException` and
  `TimeoutException`, subclasses of `IOException`, declared by the
  `net` package; and `UrlException`, a subclass of `IOException`
  declared by the `url` package, and `DateTimeException`, a direct
  subclass of `Exception` declared by the `system` package (§8.3).
  Every other library failure raises a class of §8.3 (§11.4). User
  code may declare its own classes (§8.4).
- The base `Exception` is the class for argument, range and
  parse errors that have no class of their own; there is no
  `IllegalArgumentException` or peer class.
- Raising is not the only failure channel. Predicates and
  queries report by return value: the `fs` predicates and
  `remove` on a missing path (§16.3), `io.hasInput` (§14.3),
  and the domain errors of the `math` transcendental family,
  which yield a NaN (§13.4). Stream failures beyond a null
  handle are *host-level* run-time errors (§14.6, §8.1): no
  `catch` clause handles them.
