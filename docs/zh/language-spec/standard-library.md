# 标准库


NLang 附带四个内建库：`math`、`io`、`fs`（命名空间限定的自由函数）
与 string 方法（经接收者分派）。命名空间名是**保留的**——声明名为
`math`、`io` 或 `fs` 的局部变量、函数、class、struct、enum、参数或
catch 变量是编译错误。调用只写限定名（`math.sin(x)`）；裸名不在
作用域内（未来的 `using` 式关键字可能放开此限制）。命名空间名用作
值（`int x = math;`）无法解析——命名空间不是值。

绑定由编译器内建：限定调用在编译期被对照内建表
识别为标准库函数并做类型检查，生成的代码
发射 `OP_CallIntrinsic`——没有函数记录，没有宿主注册。

**参数类型**：与声明的 kind 按转换矩阵逐实参审查——同 kind 原样放行，
矩阵允许的隐式加宽自动施加（整型家族与 float 实参进入 `double` 形参，
如 `math.sqrt(4)`；收窄一律显式 `as`——`math.absi(1.5)` 是编译错误）。
唯一的例外是 `io.print`，它接受 string、数组与全部标量基本类型（调用点
转换）；class 与 enum 值打印前需要显式 `.toString()`（struct 实参直接
拒绝——struct 没有 `toString`）。

### math——25 个函数

浮点函数族全部以 **double** 精度运行（0.7.5 起）：实参与返回值都是
`double`，整型与 `float` 实参经隐式加宽进入（`math.sqrt(4)`、
`math.sin(1.5f)` 均可编译）。
`sin`/`cos`/`tan` 接受弧度；`asin`/`acos`/`atan` 返回弧度。
`log` 是自然对数。
`floor`/`ceil`/`round` 返回 **long**（`round` 为四舍五入远离零）。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| sin cos tan asin acos atan | (double) → double | 弧度 |
| atan2 | (double y, double x) → double | C/C++ 实参顺序 |
| sqrt pow exp log | (double[,double]) → double | pow(x,y)；log = ln |
| absi / absf | (int)→int / (double)→double | absi(INT_MIN) 抛错 |
| mini maxi / minf maxf | (T, T) → T | int 对 / double 对 |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (double) → long | 超 int64 或 NaN → Exception |
| random | () → double | [0,1)，PRNG 见下 |
| srand | (int) → void | 重新播种 |
| randomi | (int min, int max) → int | 闭区间；min > max → Exception |

**PRNG 确定性**：`std::mt19937`，程序启动时由 `std::random_device`
播种。`math.srand(n)` 显式重播种——此后序列完全确定，且跨平台
一致：

```text
random()  = (double)((next() >> 8) * (1.0 / 16777216.0))   // 24 位尾数，[0,1) 内精确
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // 存在小模偏差，已记录在案
```

不使用 `std::uniform_*_distribution`——它们是实现定义的，不可移植。

### io——内容 IO

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| print | (string\|数组\|标量基本类型) → void | stdout + '\n' + flush |
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
| exists / isFile / isDirectory | (string) → bool | 从不抛错 |
| size | (string) → int | 字节大小；仅普通文件 |
| listFiles | (string) → List\<string\> | 仅名字，非递归，仅普通文件，按字典序排序 |
| makeDirs | (string) → void | mkdir -p（幂等） |
| remove | (string) → void | 文件或空目录 |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()`——处处正斜杠 |

**错误模型**：`size`/`listFiles`/`makeDirs`/`remove` 失败时抛
`IOException`。三个谓词从不抛错——无法陈述的路径（不存在或不可
访问）直接回答 `false`。对缺失路径 `remove` 是静默无操作。对目录或特殊
文件 `size` 抛 IOException（目录的「大小」是文件系统噪音）。

**join 的边界语义**（std::filesystem 路径追加，与 Python
`os.path.join` 相同）：带根的右侧（`"/b"`）会**替换**左侧；空的
右侧留下结尾分隔符（`join("a","")` 是 `"a/"`）；空的左侧得到右侧
本身。

**Windows 限制**：路径与文件名经活动代码页转换
（`generic_string`、文件打开）；非 ASCII 文件名可能无法按 UTF-8
往返。`io.readFile`/`writeFile`/`appendFile` 受同样限制。

### string 方法——18 个内建

string 接收者上的方法（`s.substring(1)`；字面量接收者亦可：
`"abc".toUpper()`）。方法面冻结为将来的 string 类的方法清单。
**字节语义**（Go/Lua 模型）：length、substring 与 indexOf 以字节
偏移计；UTF-8 字节顺序等于码点序（因此关系比较良定义）；大小写转换
仅 ASCII。按码点访问（`charAt`/`charCount`/`foreach char`）是在字节
核心之上的码点层——见 [字符串](string.md)「char 桥接」。

| 方法 | 签名 | 说明 |
|---------|-----------|-------|
| substring | (start[, end]) → string | end 不含，缺省为 length；越界 → IndexOutOfBoundsException |
| indexOf | (string) → int | 首个字节偏移，无则 -1 |
| startsWith / endsWith / contains | (string) → bool | 谓词 |
| toUpper / toLower | () → string | 仅 ASCII |
| trim | () → string | 去除 `" \t\n\r\f\v"` |
| split | (string sep) → List\<string\> | sep 必须非空（否则 Exception） |
| replace | (string old, string new) → string | 全部不重叠出现；old 必须非空 |
| toInt / toLong | () → int / long | 严格整串解析；格式非法 → Exception |
| toFloat / toDouble | () → float / double | 严格整串解析；格式非法 → Exception |
| toBool | () → bool | 严格解析 `"true"`/`"false"` |
| toChar | () → char | 严格十进制码点解析；非法标量值 → Exception |
| charAt | (int byteIndex) → char | 该字节偏移处**开始**的码点；越界 → IndexOutOfBoundsException，续接字节 → Exception |
| charCount | () → int | 码点数（对比 `length()` 字节数）；非法序列 → Exception |

字符串下标（`s[i]`）返回第 i 字节的 `ubyte`（越界抛运行期错误
`string index out of range`）——字节访问走下标，码点访问走
`charAt`。

### 流——ByteStream 与 FileStream

两个内建流类提供二进制序列化：`ByteStream` 在内存缓冲区上读写，
`FileStream` 落到磁盘文件。方法面相同（FileStream 没有 `reset()`）。

```nlang
ByteStream bs = new ByteStream();
bs.writeInt(1);
bs.writeDouble(0.5);
bs.reset();                  // 回卷游标，保留缓冲区
double d = bs.readDouble();
```

| 方法 | 线上形式 | 说明 |
|---------|-----------|-------|
| writeInt / readInt | 4 字节 | int32 |
| writeFloat / readFloat | 4 字节 | float |
| writeLong / readLong | 8 字节 | long（窄整型实参隐式加宽进入） |
| writeDouble / readDouble | 8 字节 | double（float 实参无损加宽） |
| writeString / readString | 长度前缀 + 字节 | string |
| writeStruct / readStruct | 递归字段 | 写入取 struct 值；读取取类型名——`bs.readStruct("Point")` |
| writeObject / readObject | 递归引用图 | class 实例；读取同样按类型名 |
| length / position | — | 字节数 / 当前游标 |
| reset（仅 ByteStream） | — | 回卷游标，保留缓冲区 |
| close | — | 释放 FileStream 的文件句柄 |

**FileStream 构造**：`new FileStream(path, mode)`，mode 是
`"w"`（创建/截断）、`"a"`（创建/追加）或 `"r"`（只读；文件不存在抛
运行期错误）。非法 mode 抛运行期错误。

**实参类型**：标量写入方法按转换矩阵逐实参审查——窄整型与 `float`
实参隐式加宽进入 8 字节槽（与 `math.sqrt` 实参同一规则）；`ulong`
实参超出 long 值域，须显式 `as long`；`char` 与 string 实参对数值
方法是编译错误。

**EOS 语义**：在游标已到末尾的流上读取抛运行期错误（不是返回 0）。

### 异常映射

- **IOException**：`io.readFile`/`writeFile`/`appendFile` 与所有会
  抛错的 `fs.*` 函数。
- **IndexOutOfBoundsException**：substring 范围错误。
- **base Exception**：实参/范围/解析错误——`clampi`/`clampf` 的
  lo>hi、`randomi` 的 min>max、`split`/`replace` 的空实参、
  `toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar` 的非法输入、
  `charAt`/`charCount` 的非法 UTF-8 序列、`floor`/`ceil`/`round`/`absi`
  的溢出。没有 `IllegalArgumentException` 内建类；日后收窄为专门的
  子类对 `catch (Exception)` 调用方源码兼容。

### 未来方向

- `using` 式关键字，开放非限定名
- string 类化（方法面已在上方冻结）
- `IllegalArgumentException` 内建子类
- 包管理器
