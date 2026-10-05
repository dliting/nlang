# 标准库


NLang的标准库由`math`、`io`、`fs`三个包（包限定的自由函数）与string方法（经接收者分派）组成。包由其文件路径标识——`stdlib/io.n`就是包`io`——同一次构建中每个包名只能存在一份（重复是编译错误，诊断指名两条来源；名为`io`的项目目录就是普通目录）。调用只写限定名（`math.sin(x)`）；裸名不在作用域内。包名用作值（`int x = math;`）无法解析——包不是值。

标准库的**签名**（参数类型、参数个数、返回类型）写在随工具链分发的`stdlib/*.n`声明中，经语言服务的符号索引提供给编译器与编辑器（代码补全、悬停、转到定义）。标准库的形状与第三方库一致：`stdlib/*.n`声明接口，其中的`native`成员由`nlang_<ns>.dll`实现，运行期经宿主应用二进制接口（ABI，application binary interface）到达。限定调用按声明做类型检查后发射`OP_CallFunc`；内建string方法经接收者分派，发射`OP_CallIntrinsic`。查找`.n`与加载native库的目录规则见下方「库与搜索路径」。

**参数类型**：与声明的kind按转换矩阵逐实参审查——同kind原样放行，矩阵允许的隐式加宽自动施加（整型家族与float实参进入`double`形参，如`math.sqrt(4)`；收窄一律显式`as`——`math.absi(1.5)`是编译错误）。唯一的例外是io的强制转换三函数（`write`/`print`/`eprint`），它们接受string、数组、全部标量基本类型与函数值（调用点转换；函数值格式化为`func <name>`，见[函数类型与代理](function-types-and-delegates.md)）；class与enum值打印前需要显式`.toString()`（struct实参直接拒绝——struct没有`toString`）。

### math——25个函数

浮点函数族全部以**double**精度运行：实参与返回值都是`double`，整型与`float`实参经隐式加宽进入（`math.sqrt(4)`、`math.sin(1.5f)`均可编译）。`sin`/`cos`/`tan`接受弧度；`asin`/`acos`/`atan`返回弧度。`log`是自然对数。`floor`/`ceil`/`round`返回**long**（`round`为四舍五入远离零）。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| sin cos tan asin acos atan | (double) → double | 弧度 |
| atan2 | (double y, double x) → double | C/C++实参顺序 |
| sqrt pow exp log | (double[,double]) → double | pow(x,y)；log = ln |
| absi / absf | (int)→int / (double)→double | absi(int最小值)抛错 |
| mini maxi / minf maxf | (T, T) → T | int对 / double对 |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (double) → long | 超int64或NaN → Exception |
| random | () → double | [0,1)，伪随机数生成器（PRNG，pseudorandom number generator）见下 |
| srand | (int) → void | 重新播种 |
| randomi | (int min, int max) → int | 闭区间；min > max → Exception |

**PRNG确定性**：`std::mt19937`，程序启动时由`std::random_device`播种。`math.srand(n)`显式重播种——此后序列完全确定，且跨平台一致：

```text
random()  = (double)((next() >> 8) * (1.0 / 16777216.0))   // 24 位尾数，[0,1) 内精确
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // 存在小模偏差，已记录在案
```

不使用`std::uniform_*_distribution`——它们是实现定义的，不可移植。

### io——内容IO

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| write | (string\|数组\|标量基本类型) → void | stdout，**不**换行，flush |
| eprint | (string\|数组\|标量基本类型) → void | stderr + '\n' + flush |
| print | (string\|数组\|标量基本类型) → void | stdout + '\n' + flush |
| readLine | () → string | stdin一行，去掉结尾 '\r' |
| readFile | (string) → string | 整个文件按字节读取；失败 → IOException |
| writeFile | (string path, string s) → void | 创建/截断；失败 → IOException |
| appendFile | (string path, string s) → void | 创建/追加；失败 → IOException |

`write`、`print`与`eprint`共用同一实参策略（上表的强制转换三函数）：`write("Name: ")`发出不带换行的提示，键入的回复落在同一控制台行上；`eprint`镜像`print`但写到stderr，让诊断输出与正常输出保持可分离。调试会话内两个流合并进会话的单一输出视图。

**readLine的文件结束（EOF，end of file）语义**：EOF与空输入行都返回`""`——设计上不可区分（与C++ `std::getline`相同）。必须检测输入结束的程序应当以哨兵内容终止，而不是以空行判断。三个强制转换函数（`write`/`print`/
`eprint`）恰好各接受一个实参；打印多个值请多次调用。

`readFile`强制16 MiB上限（与反序列化器对不可信长度前缀施加的同一界限）；更大的文件抛IOException。

### fs——名字、目录、元数据

`fs`从不读写内容——内容属于`io`。这一划分是操作原则：io = 全部内容（控制台 + 磁盘文本），fs = 命名空间/目录/元数据。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| exists / isFile / isDirectory | (string) → bool | 从不抛错 |
| size | (string) → int | 字节大小；仅普通文件 |
| listFiles | (string) → List\<string\> | 仅名字，非递归，仅普通文件，按字典序排序 |
| makeDirs | (string) → void | mkdir -p（幂等） |
| remove | (string) → void | 文件或空目录 |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()`——处处正斜杠 |

**错误模型**：`size`/`listFiles`/`makeDirs`/`remove`失败时抛`IOException`。三个谓词从不抛错——无法陈述的路径（不存在或不可访问）直接回答`false`。对缺失路径`remove`是静默无操作。对目录或特殊文件`size`抛IOException（目录的「大小」是文件系统噪音）。

**join的边界语义**（std::filesystem路径追加，与Python
`os.path.join`相同）：带根的右侧（`"/b"`）会**替换**左侧；空的右侧留下结尾分隔符（`join("a","")`是`"a/"`）；空的左侧得到右侧本身。

**Windows编码约定**：路径与文件名经进程活动代码页转换（`generic_string`、文件打开）。命令行工具以Unicode转换格式（UTF-8，Unicode Transformation Format）为进程活动代码页运行（内嵌清单声明，Windows 10 1903+生效），非美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）路径与文件名按UTF-8往返，`io.readFile`/`writeFile`/`appendFile`同样适用；以系统代码页运行VM的嵌入宿主仍受该代码页限制。

### string方法——18个内建

string接收者上的方法（`s.substring(1)`；字面量接收者亦可：`"abc".toUpper()`）。string方法清单如下。**字节语义**（Go/Lua模型）：length、substring与indexOf以字节偏移计；UTF-8字节顺序等于码点序（因此关系比较良定义）；大小写转换仅ASCII。按码点访问（`charAt`/`charCount`/`foreach char`）是在字节核心之上的码点层——见[字符串](string.md)「char桥接」。

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

### 流——ByteStream与FileStream

两个内建流类提供二进制序列化：`ByteStream`在内存缓冲区上读写，`FileStream`落到磁盘文件。方法面相同（FileStream没有`reset()`）。

```nlang
ByteStream bs = new ByteStream();
bs.writeInt(1);
bs.writeDouble(0.5);
bs.reset();                  // 回卷游标，保留缓冲区
double d = bs.readDouble();
```

| 方法 | 线上形式 | 说明 |
|---------|-----------|-------|
| writeInt / readInt | 4字节 | int32 |
| writeFloat / readFloat | 4字节 | float |
| writeLong / readLong | 8字节 | long（窄整型实参隐式加宽进入） |
| writeDouble / readDouble | 8字节 | double（float实参无损加宽） |
| writeString / readString | 长度前缀 + 字节 | string |
| writeStruct / readStruct | 递归字段 | 写入取struct值；读取取类型名——`bs.readStruct("Point")` |
| writeObject / readObject | 递归引用图 | class实例；读取同样按类型名 |
| length / position | — | 字节数 / 当前游标 |
| reset（仅ByteStream） | — | 回卷游标，保留缓冲区 |
| close | — | 释放FileStream的文件句柄 |

**FileStream构造**：`new FileStream(path, mode)`，mode是`"w"`（创建/截断）、`"a"`（创建/追加）或`"r"`（只读；文件不存在抛运行期错误）。非法mode抛运行期错误。

**实参类型**：标量写入方法按转换矩阵逐实参审查——窄整型与`float`实参隐式加宽进入8字节槽（与`math.sqrt`实参同一规则）；`ulong`实参超出long值域，须显式`as long`；`char`与string实参对数值方法是编译错误。

**流结束（EOS，end of stream）语义**：在游标已到末尾的流上读取抛运行期错误（不是返回0）。

### 库与搜索路径

NLang的库由**`.n`源文件**承载：标准库的`math.n`/`io.n`/`fs.n`随工具链分发，第三方库就是某个目录中的一组`.n`（可选搭配native动态库）。文件内用`native`关键字声明在NLang之外实现的函数（`native void print(string s);`）——这类声明只有签名与文档注释、没有函数体；没有`native`标记的普通函数则是可阅读、可修改的NLang实现。一个库可以同时包含两者（混合库，与Python/Java/C#相同）。编译期按源码内联解析签名；运行期从编译好的库包装载单元映像——标准库即随工具链分发的`stdlib.npkg`（每个库单元一个成员）。

**搜索路径**决定编译器到哪里查找被导入的`.n`，以及运行期到哪里装载产物依赖的`.ncu`/`.npkg`成员、加载native动态库——标准库与第三方、编译期发现与运行期加载使用**同一组目录**。目录按下列顺序拼接，前者优先；重复目录只保留第一次出现（路径经规范化，Windows上还会折叠大小写）：

1. 命令行`-I <dir>`（最高优先级，可多次指定）；
2. 项目文件`.nproj`中的`<ImportPaths>`；
3. 项目 / 源文件 / 模块所在目录（局部）；
4. 环境变量`NLANG_PATH`（Windows以`;`、POSIX以`:`分隔）；
5. 系统缺省：标准库目录、可执行文件目录、当前目录（最低）。

命令行用法：

```text
ncc build app.n -o app.ncu -I C:\libs\mylib
nvm app.ncu -I C:\libs\mylib
ndb --machine app.ncu -I C:\libs\mylib
```

项目在`.nproj`中用`<ImportPaths>`持久化搜索目录（路径相对项目文件存储）：

```xml
<Project name="app">
  <Sources><File path="src/main.n"/></Sources>
  <ImportPaths><Dir path="../libs"/></ImportPaths>
</Project>
```

**在nide中配置**：全局搜索路径在「工具 → 选项 → 库搜索路径」，项目级路径在「项目 → 属性 → 库搜索路径」；两处都支持添加、移除、上移、下移与浏览目录，项目路径优先于全局路径。更改后会自动重建符号索引，代码补全与「转到定义」随之刷新。

**查看与跳转源码**：在编辑器中对库符号使用「转到定义」（F12）即可打开对应`.n`——`native`声明显示签名与文档，普通函数显示可编辑的实现；修改后重新构建即生效。

### 异常映射

- **IOException**：`io.readFile`/`writeFile`/`appendFile`与所有会抛错的`fs.*`函数。
- **IndexOutOfBoundsException**：substring范围错误。
- **base Exception**：实参/范围/解析错误——`clampi`/`clampf`的lo>hi、`randomi`的min>max、`split`/`replace`的空实参、`toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar`的非法输入、`charAt`/`charCount`的非法UTF-8序列、`floor`/`ceil`/`round`/`absi`的溢出。没有`IllegalArgumentException`内建类；这些位置以base
  Exception报告。
