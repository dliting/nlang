# 异常映射

- **IOException**：`io.readFile`/`writeFile`/`appendFile`与所有会抛错的`fs.*`函数。
- **IndexOutOfBoundsException**：substring范围错误。
- **base Exception**：实参/范围/解析错误——`clampi`/`clampf`的lo>hi、`randomi`的min>max、`split`/`replace`的空实参、`toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar`的非法输入、`charAt`/`charCount`的非法Unicode转换格式（UTF-8，Unicode Transformation Format）序列、`floor`/`ceil`/`round`/`absi`的溢出。没有`IllegalArgumentException`内建类；这些位置以base
  Exception报告。
