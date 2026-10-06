# Exception mapping

- **IOException**: `io.readFile`/`writeFile`/`appendFile` and every raising
  `fs.*` function.
- **IndexOutOfBoundsException**: substring range errors.
- **base Exception**: argument/range/parse errors — `clampi`/`clampf`
  lo>hi, `randomi` min>max, `split`/`replace` empty argument,
  `toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar` malformed input,
  `charAt`/`charCount` invalid Unicode Transformation Format (UTF-8) sequence, `floor`/`ceil`/`round`/`absi`
  overflow. There is no
  `IllegalArgumentException` built-in; these sites report the base
  Exception.
