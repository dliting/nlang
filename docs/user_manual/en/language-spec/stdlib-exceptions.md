# Exception mapping

The normative rules are in [specification §19.1–§19.2](../specification/19-library-exceptions.md).

- **IOException**: `io.readFile`/`writeFile`/`appendFile` and every raising
  `fs.*` function; `io.readToken`/`readChar` and the typed readers at end
  of input (or in a session without stdin).
- **IndexOutOfBoundsException**: substring range errors and `charAt` byte
  indices outside the string.
- **base Exception**: argument/range/parse errors — `clampi`/`clampf`
  lo>hi, `randomi` min>max, `split`/`replace` empty argument,
  `toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar` malformed input,
  `readChar`/`charAt`/`charCount` invalid Unicode Transformation Format (UTF-8) sequence, `floor`/`ceil`/`round`/`absi`
  overflow. There is no
  `IllegalArgumentException` built-in; these sites report the base
  Exception.
