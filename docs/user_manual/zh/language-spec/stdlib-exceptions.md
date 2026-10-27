# 异常映射

规范性规则见规范§19.1–§19.2。

- **IOException**：`io.readFile`/`writeFile`/`appendFile`与所有会抛错的`fs.*`函数；`io.readToken`/`readChar`与数值读取在输入结束（或会话没有stdin）时。
- **IndexOutOfBoundsException**：substring范围错误，以及`charAt`字节下标越界。
- **base Exception**：实参/范围/解析错误——`clampi`/`clampf`的lo>hi、`randomi`的min>max、`split`/`replace`的空实参、`toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar`的非法输入、`readChar`/`charAt`/`charCount`的非法Unicode转换格式（UTF-8，Unicode Transformation Format）序列、`floor`/`ceil`/`round`/`absi`的溢出。没有`IllegalArgumentException`内建类；这些位置以base
  Exception报告。
