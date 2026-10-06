# string方法——18个内建

string接收者上的方法（`s.substring(1)`；字面量接收者亦可：`"abc".toUpper()`）。string方法清单如下。**字节语义**（Go/Lua模型）：length、substring与indexOf以字节偏移计；Unicode转换格式（UTF-8，Unicode Transformation Format）字节顺序等于码点序（因此关系比较良定义）；大小写转换仅美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）。按码点访问（`charAt`/`charCount`/`foreach char`）是在字节核心之上的码点层——见[字符串](string.md)「char桥接」。

| 方法 | 签名 | 说明 |
|---------|-----------|-------|
| substring | (start[, end]) → string | end不含，缺省为length；越界 → IndexOutOfBoundsException |
| indexOf | (string) → int | 首个字节偏移，无则 -1 |
| startsWith / endsWith / contains | (string) → bool | 谓词 |
| toUpper / toLower | () → string | 仅ASCII |
| trim | () → string | 去除`" \t\n\r\f\v"` |
| split | (string sep) → List\<string\> | sep必须非空（否则Exception） |
| replace | (string old, string new) → string | 全部不重叠出现；old必须非空 |
| toInt / toLong | () → int / long | 严格整串解析；格式非法 → Exception |
| toFloat / toDouble | () → float / double | 严格整串解析；格式非法 → Exception |
| toBool | () → bool | 严格解析`"true"`/`"false"` |
| toChar | () → char | 严格十进制码点解析；非法标量值 → Exception |
| charAt | (int byteIndex) → char | 该字节偏移处**开始**的码点；越界 → IndexOutOfBoundsException，续接字节 → Exception |
| charCount | () → int | 码点数（对比`length()`字节数）；非法序列 → Exception |

字符串下标（`s[i]`）返回第i字节的`ubyte`（越界抛运行期错误`string index out of range`）——字节访问走下标，码点访问走`charAt`。
