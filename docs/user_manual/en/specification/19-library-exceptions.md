# 19. Library Exceptions

This chapter maps the failures of library operations — the
built-in classes of chapter 13 and the packages of chapters
14–18 — to the built-in exception classes of §9.3. §19.1 is
the map; §19.2 fixes the conventions it rests on. The
semantics of every site live in the chapter that defines the
operation; this chapter adds no failure behavior of its own.

## 19.1 Raise Map by Unit

| Unit | Class | Sites |
| --- | --- | --- |
| `string` (§13.3) | `IndexOutOfBoundsException` | `substring` range errors; `charAt` out of range |
| `string` (§13.3) | `Exception` | `split` or `replace` with an empty argument; `toInt`, `toLong`, `toFloat`, `toDouble`, `toBool` or `toChar` on a malformed string; `charAt` on a continuation byte; `charCount` on an invalid Unicode Transformation Format (UTF-8) sequence |
| `List` (§13.4) | `IndexOutOfBoundsException` | `get`, `set` or `removeAt` out of range |
| `Dict` (§13.4) | `Exception` | `get` with no entry for the key |
| `math` (§14.4) | `Exception` | `absi` of the most negative `int`; `clampi` or `clampf` with `lo > hi`; `randomi` with `min > max`; `floor`, `ceil` or `round` on a NaN or a value outside the `long` range |
| `io` (§15.3, §15.4) | `IOException` | `readLine` with no input channel; `readToken` or `readChar` at end of input or with no channel; the typed readers at end of input; `readFile`, `writeFile` or `appendFile` failure |
| `io` (§15.3) | `Exception` | `readChar` on a byte sequence that is not valid Unicode Transformation Format (UTF-8); a typed reader on a malformed token (the string method's failure, §13.3) |
| streams (§15.5) | `NullPointerException` | a method call through a null stream handle |
| `fs` (§16.3) | `IOException` | `size`, `listFiles`, `makeDirs` or `remove` failure |
| `Channel` (§11.4) | `ChannelClosedException` | a send or receive on a closed channel |
| `Channel` (§11.4) | `Exception` | construction with a negative capacity |
| `tensor` (§18.5) | `NullPointerException` | a null array argument |
| `tensor` (§18.5) | `IndexOutOfBoundsException` | a creation length past the bound of §4.3; a negative `reshape` dimension |
| `tensor` (§18.5) | `DivByZeroException` | integer `div` by zero |
| `tensor` (§18.5) | `Exception` | `arange` with a zero or non-advancing step, or a range past the bound; a length or shape mismatch; `max`, `min` or `mean` on an empty array |

A call through a `null` receiver raises
`NullPointerException` in every unit (§13.2); the rows list
only the sites the defining chapters call out besides it.

## 19.2 Conventions

- The library declares no exception classes of its own
  (§13.5): every library failure that raises raises a class
  of §9.3. User code may declare its own classes (§9.4).
- The base `Exception` is the class for argument, range and
  parse errors that have no class of their own; there is no
  `IllegalArgumentException` or peer class.
- Raising is not the only failure channel. Predicates and
  queries report by return value: the `fs` predicates and
  `remove` on a missing path (§16.3), `io.hasInput` (§15.3),
  and the domain errors of the `math` transcendental family,
  which yield a NaN (§14.4). Stream failures beyond a null
  handle are *host-level* run-time errors (§15.5, §9.1): no
  `catch` clause handles them.
