# io——内容IO

`io`承载全部内容读写：控制台与磁盘文本文件。多字节输入按Unicode转换格式（UTF-8，Unicode Transformation Format）解码为完整码点（`readChar`）；磁盘内容按字节原样往返。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| write | (string\|数组\|标量基本类型) → void | stdout，**不**换行，flush |
| eprint | (string\|数组\|标量基本类型) → void | stderr + '\n' + flush |
| print | (string\|数组\|标量基本类型) → void | stdout + '\n' + flush |
| readLine | () → string | stdin一行，去掉结尾 '\r' |
| hasInput | () → bool | 输入还未耗尽则true（从不抛错）。文件与管道重定向精确；控制台只报已缓冲内容；调试会话只报已停驻行 |
| readToken | () → string | 跳过空白（可跨行）取下一个非空白串；输入结束处取不到词抛IOException |
| readChar | () → char | 取下一个非空白字符（完整码点，UTF-8解码）；输入结束抛IOException；非法序列抛Exception |
| 数值读取 | readInt()/readLong()/readFloat()/readDouble()/readBool() | readToken取的词交给对应string方法解析 |
| readFile | (string) → string | 整个文件按字节读取；失败 → IOException |
| writeFile | (string path, string s) → void | 创建/截断；失败 → IOException |
| appendFile | (string path, string s) → void | 创建/追加；失败 → IOException |

`write`、`print`与`eprint`共用同一实参策略（上表的强制转换三函数）：`write("Name: ")`发出不带换行的提示，键入的回复落在同一控制台行上；`eprint`镜像`print`但写到stderr，让诊断输出与正常输出保持可分离。调试会话内两个流合并进会话的单一输出视图。

**交互控制台的输入提示符**：当程序即将阻塞在输入读取上，且stdin与stdout都是控制台时，运行时先输出`"> "`提示符再等待——与Python、sqlite3等REPL的做法一致，`nvm`直接运行与ncc进程内运行都适用。只要任一端是管道或重定向（脚本化输入、`nvm x > out.txt`），两个流都保持逐字节不变；nide调试页终端的行模式提示符是同一记号，由nide本地绘制。

**readLine的文件结束（EOF，end of file）语义**：EOF与空输入行都返回`""`（与C++ `std::getline`相同）；空串哨兵区分不了两者，这由`hasInput()`谓词承担——见下节。三个强制转换函数（`write`/`print`/`eprint`）恰好各接受一个实参；打印多个值请多次调用。

`readFile`强制16 MiB上限（与反序列化器对不可信长度前缀施加的同一界限）；更大的文件抛IOException。

### 取词读取与混合语义

除按行读取外，io还提供按词读取：`readToken()`跳过空白（空格、制表符、回车、换行，可跨行）返回下一个非空白串；`readChar()`返回下一个非空白字符（完整码点，UTF-8解码）。以它们为基础，`readInt()`、`readLong()`、`readFloat()`、`readDouble()`、`readBool()`取一个词并交给对应的string方法解析，解析失败与直接调用该方法报错完全一致。

输入结束的行为与C/C++/Java对齐：行级消费（`readLine`）把输入结束当正常终点，返回空串；要取某个数据（`readToken`、`readChar`与五个取值读取）在输入结束处取不到内容，抛出IOException。空串哨兵区分不了空行与输入结束，这由`hasInput()`谓词承担（从不抛错）：

```nlang
while (io.hasInput()) {
    string line = io.readLine();
    io.print(line);
}
```

混合读取遵循C++里`cin >>`与`getline`的组合语义：取词读取只消费到词尾，随后的`readLine()`返回该行剩余部分（含前导空白），当前行耗尽后才取下一行。输入`"10 20\n30\n"`时：`readInt()`得10，`readInt()`得20，`readLine()`得`""`（行剩余为空），`readLine()`得`"30"`，再读返回`""`（输入结束）。

`readChar()`返回字面字符：输入x得'x'。它与string.toChar的数值解析语义（`"120"`得'x'）有意不同。输入的字节序列不是合法UTF-8时抛出Exception。
