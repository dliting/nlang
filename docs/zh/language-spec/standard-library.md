# 标准库（Phase 11）


NLang 附带四个内建库：`math`、`io`、`fs`（命名空间限定的自由函数）
与 string 方法（经接收者分派）。命名空间名是**保留的**——声明名为
`math`、`io` 或 `fs` 的局部变量、函数、class、struct、enum、参数或
catch 变量是编译错误。调用只写限定名（`math.sin(x)`）；裸名不在
作用域内（未来的 `using` 式关键字可能放开此限制）。命名空间名用作
值（`int x = math;`）无法解析——命名空间不是值。

绑定由编译器内建：resolver 对照内建表
（`include/nlang/vm/StdLib.h`）拦截限定调用并做类型检查，代码生成
发射 `OP_CallIntrinsic`——没有函数记录，没有宿主注册。

**参数类型**：与声明的 kind 精确匹配；唯一自动施加的转换是 int→float
加宽（`math.sqrt(4)` 可编译）。float→int 永不隐式（
`math.absi(1.5)` 是编译错误）。唯一的例外是 `io.print`，它接受
string|int|float（调用点转换）；class 与 enum 值打印前需要显式
`.toString()`（struct 实参直接拒绝——struct 没有 `toString`）。

### math——25 个函数

`sin`/`cos`/`tan` 接受弧度；`asin`/`acos`/`atan` 返回弧度。
`log` 是自然对数。
`floor`/`ceil`/`round` 返回 int（`round` 为四舍五入远离零）。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| sin cos tan asin acos atan | (float) → float | 弧度 |
| atan2 | (float y, float x) → float | C/C++ 实参顺序 |
| sqrt pow exp log | 见下文 | pow(x,y)；log = ln |
| absi / absf | (int)→int / (float)→float | absi(INT_MIN) 抛错 |
| mini maxi / minf maxf | (T, T) → T | |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (float) → int | 超 int32 或 NaN → Exception |
| random | () → float | [0,1)，PRNG 见下 |
| srand | (int) → void | 重新播种 |
| randomi | (int min, int max) → int | 闭区间；min > max → Exception |

**PRNG 确定性**：`std::mt19937`，程序启动时由 `std::random_device`
播种。`math.srand(n)` 显式重播种——此后序列完全确定，且跨平台
一致：

```text
random()  = (float)((next() >> 8) * (1.0f / 16777216.0f))   // 24 位尾数，[0,1) 内精确
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // 存在小模偏差，已记录在案
```

不使用 `std::uniform_*_distribution`——它们是实现定义的，不可移植。

### io——内容 IO

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| print | (string\|int\|float) → void | stdout + '\n' + flush |
| readLine | () → string | stdin 一行，去掉结尾 '\r' |
| readFile | (string) → string | 整个文件按字节读取；失败 → IOException |
| writeFile | (string path, string s) → void | 创建/截断；失败 → IOException |
| appendFile | (string path, string s) → void | 创建/追加；失败 → IOException |

**readLine 的 EOF 语义**：EOF 与空输入行都返回 `""`——设计上不可
区分（与 C++ `std::getline` 相同）。必须检测输入结束的程序应当以
哨兵内容终止，而不是以空行判断。`io.print` 恰好接受一个实参；打印
多个值请多次调用。

`readFile` 强制 16 MiB 上限（与反序列化器对不可信长度前缀施加的
同一界限）；更大的文件抛 IOException。

### fs——名字、目录、元数据

`fs` 从不读写内容——内容属于 `io`。这一划分是操作原则：
io = 全部内容（控制台 + 磁盘文本，将来的流类），fs =
命名空间/目录/元数据。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| exists / isFile / isDirectory | (string) → int | 0/1；从不抛错 |
| size | (string) → int | 字节大小；仅普通文件 |
| listFiles | (string) → List\<string\> | 仅名字，非递归，仅普通文件，按字典序排序 |
| makeDirs | (string) → void | mkdir -p（幂等） |
| remove | (string) → void | 文件或空目录 |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()`——处处正斜杠 |

**错误模型**：`size`/`listFiles`/`makeDirs`/`remove` 失败时抛
`IOException`。三个谓词从不抛错——无法陈述的路径（不存在或不可
访问）直接回答 0。对缺失路径 `remove` 是静默无操作。对目录或特殊
文件 `size` 抛 IOException（目录的「大小」是文件系统噪音）。

**join 的边界语义**（std::filesystem 路径追加，与 Python
`os.path.join` 相同）：带根的右侧（`"/b"`）会**替换**左侧；空的
右侧留下结尾分隔符（`join("a","")` 是 `"a/"`）；空的左侧得到右侧
本身。

**Windows 限制**：路径与文件名经活动代码页转换
（`generic_string`、文件打开）；非 ASCII 文件名可能无法按 UTF-8
往返。`io.readFile`/`writeFile`/`appendFile` 受同样限制。

### string 方法——12 个内建

string 接收者上的方法（`s.substring(1)`；字面量接收者亦可：
`"abc".toUpper()`）。方法面冻结为将来的 string 类的方法清单。
**字节语义**（Go/Lua 模型）：length、substring 与 indexOf 以字节
偏移计；UTF-8 字节顺序等于码点序（因此关系比较良定义）；大小写转换
仅 ASCII。字符串下标（`s[i]`）**不支持**——string 上没有字节访问
运算符。

| 方法 | 签名 | 说明 |
|---------|-----------|-------|
| substring | (start[, end]) → string | end 不含，缺省为 length；越界 → IndexOutOfBoundsException |
| indexOf | (string) → int | 首个字节偏移，无则 -1 |
| startsWith / endsWith / contains | (string) → int | 0/1 |
| toUpper / toLower | () → string | 仅 ASCII |
| trim | () → string | 去除 `" \t\n\r\f\v"` |
| split | (string sep) → List\<string\> | sep 必须非空（否则 Exception） |
| replace | (string old, string new) → string | 全部不重叠出现；old 必须非空 |
| toInt / toFloat | () → int / float | 严格整串解析；格式非法 → Exception |

### 异常映射

- **IOException**：`io.readFile`/`writeFile`/`appendFile` 与所有会
  抛错的 `fs.*` 函数。
- **IndexOutOfBoundsException**：substring 范围错误。
- **base Exception**：实参/范围/解析错误——`clampi`/`clampf` 的
  lo>hi、`randomi` 的 min>max、`split`/`replace` 的空实参、
  `toInt`/`toFloat` 的非法输入、`floor`/`ceil`/`round`/`absi` 的
  溢出。没有 `IllegalArgumentException` 内建类；日后收窄为专门的
  子类对 `catch (Exception)` 调用方源码兼容。

### 未来方向

- `using` 式关键字，开放非限定名
- string 类化（方法面已在上方冻结）
- Stream 家族，把 ByteStream/FileStream 统一到 io 下
- `IllegalArgumentException` 内建子类
- 包管理器（已推迟；`ModuleManager::LoadFrom(istream)` 桩保留）
