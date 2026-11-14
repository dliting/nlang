# v0.13.11 JSON库（stdlib/json.n）概要设计

## 一、引言

NLang标准库已有网络栈（`net`/`url`/`http`）、压缩（`gzip`）与正则（`regex`），但没有JSON——stdlib与用户手册全树检索零命中。HTTP消息体、配置文件、即将排期的WEB框架（v0.13.12）都需要JSON编解码作为地基。用户已裁定（2026-11-15）：JSON先行成库，占用v0.13.11版本号，原v0.13.11的WEB框架条目整体递延为v0.13.12（范围文字不动）。

本文是概要设计（Spec）：钉死目标范围、语义裁定与API面；逐任务细化留给实施计划。文风与结构仿`docs/dev/regex_library_design.md`。

## 二、目标与范围

### 2.1 本版做

- RFC 8259全语法解析器：字符串入口`json.parse(text)`，严格文法，违例抛`JsonException`（含字节偏移）；
- DOM值模型：`JsonValue`抽象基类+六个具体节点类+`JsonKind`枚举，类型化读取面；
- 序列化：紧凑`serialize()`与缩进`serialize(int spaces)`两种形态，对象键插入序稳定；
- 构建面：公开构造器与`put`/`add`自动装箱重载，支持程序化构造payload；
- 实现形态：纯nlang单文件`stdlib/json.n`，零新增原生函数、零编译器/VM改动；
- 手册新章`library/26-json.md`（中英成对），formats章26–28顺延27–29，异常登记入第20章。

### 2.2 本版不做

- JSON Pointer（RFC 6901）、JSON Merge Patch（RFC 7386）、schema校验：暂无消费者，WEB框架或示例真需要时再立；
- 流式/增量API（逐token回调、大文件SAX式解析）：标准消费场景（消息体、配置）均为整体载荷，YAGNI；
- 高精度数（BigDecimal式）：超出JSON互操作所需的IEEE精度即报错或降double，见D-2；
- `byte[]`入口：库边界是`string`——http侧已有`bodyText()`、文件侧已有`io.readFile()`完成UTF-8解码，库内不做二次解码；
- raw string字面量、lambda：属语言面独立条目（后者已排v0.13.10），本版不依赖也不等待——JSON库没有任何回调形态，与lambda零耦合。

### 2.3 版本落点

内容整体入roadmap的**v0.13.11：JSON库**（新立条目）；原v0.13.11「WEB框架」条目递延为**v0.13.12**，其范围文字原样搬移。roadmap改写与本spec提交同批。当前dev线在v0.13.8收官后直接实施本版，不依赖v0.13.9（值引用参数）与v0.13.10（lambda）的任何语言变更。

## 三、关键事实（勘察所得）

每条都在当前树核实过，是后续裁定的依据。

1. **VM调用深度硬上限1000帧**（`src/vm/VmExecutor.h:2153` `RECURSE_LIMIT = 1000`，`VmExecutorFrames.cpp:80/140`检查）。超限抛`std::runtime_error("NLang VM: recursion limit exceeded")`，手册已成文：这是*host-level*错误，**任何`catch`子句都接不住**，程序直接终止（`docs/user_manual/en/specification/07-functions/index.md:41-44`、`08-exceptions/index.md:48-51`）。VM帧堆上分配，深递归不炸C++栈，但深度就是深度。
2. **double→string已是最短往返渲染**，无需新增原生帮手：退役的`OP_Float_to_str`由注册表路径取代，`RnFloat::ValueToString`→`FormatFloatShortest`（`src/runtime/RnTypes.cpp:101-147`）逐位`%.*e`+`strtod`回探最少有效位，定点窗口[1e-4, 1e16)之外转科学计数（Python repr风格），非有限与零哨兵渲染为`"nan"`/`"inf"`/`"-inf"`/`"-0"`。手册同口径（`04-types/casts.md:94-110`）。UCRT的`%e`指数恒两位（`1e+16`），符合JSON文法。
3. **`toLong()`/`toDouble()`天然拒绝非有限与溢出**：`toLong`走`strtoll`且`ERANGE`即抛`Exception`；`toDouble`走`strtod`且结果非有限即抛（`src/vm/IntrinsicsString.cpp:497-520`，手册`library/17-string-methods.md:45-46`成文为「strict whole-string parse; a malformed string raises `Exception`」）。故解析数值token的转换管线无需自己判溢出/inf。
4. **`Dict<K, V>`保持键插入序，且覆盖写不动位**：`keys()`按插入序返回拷贝、既有键重新`set`只换值不换位置、`foreach`经`keys()`快照迭代、`toString`按插入序渲染——全是规范性条文（`docs/user_manual/en/specification/library/12-collections.md:94-101`、表`:79`）。`Dict.get`缺键抛`Exception`；`Dict.remove`返回1/0；`List`越界抛`IndexOutOfBoundsException`（`:29-32,80,82`）。类元素（含容器）在`indexOf`/`contains`中按身份比较，用户`equals`不参与（`:48-49`）。代价侧：`Dict`条目线性扫描，每次`set`/`get`/`containsKey`/`remove`均为O(n)（`src/vm/VmExecutor.h:2698-2702`自注「Linear-scan lookup is O(n) per op」，手册`12-collections.md:106-108`同口径informative），无O(1)散列查找；对JSON对象规模的成本核算见D-4。
5. **`as`类下转型失配抛可捕获的`InvalidCastException`；Object拆箱失配是host-level错误**（`04-types/casts.md:36-44`）。struct、interface、函数值没有进`Object`的通路（不可赋值、`as`也转不进——`04-types/object.md` §4.11.1`:15-17`；同节`:7-8`/`:13-14`并载class/Array直入Object免装箱、`:18-19`装箱只收标量/string/enum，见§4.11.3）。没有`is`/`instanceof`类型测试算子（手册`01-lexical.md`保留字表无`is`），`typeName(T)`是编译期形式、不测运行期值的动态种类（`04-types/generics.md:338-342`）——运行期种类判别只能走基类虚分派，不能`as`+`catch`探种类。
6. **下标语法糖仅限内建`List`/`Dict`**：编译器按`kBuiltinListTypeName`/`kBuiltinDictTypeName`门控（`src/compiler/builder/ExprResolverSubscript.cpp:70-96`；手册`04-types/generics.md` §4.12.7同口径）。用户类没有`obj["k"]`糖，API面必须走方法。
7. **`==`对类恒为身份**，协议方法`equals`/`getHashCode`/`toString`按名分派（`04-types/object.md` §4.11.2、`04-types/casts.md:112-151`）；抽象类=声明无体虚方法，实例化即编译错（`04-types/class.md` §4.8.9，`:356-403`）。
8. **stdlib新增`.n`零构建文件改动**：`CMakeLists.txt:267-268` `file(GLOB NLANG_STDLIB_SOURCES CONFIGURE_DEPENDS stdlib/*.n)`自动收录进npkg与IDE树（根部预置的`lang.n`/`containers.n`从npkg清单排除，`:276-279`）。「零登记」仅限npkg打包与IDE树解析这两面：测试钉桩重算与手册nav/散文登记是必做同步点（事实10、§八），勿由本条引申为「新增包不动任何文件」。
9. **包异常先例分两支**：`SocketException`/`TimeoutException`/`UrlException`/`HttpException`/`GzipException`/`RegexException`是`IOException`子类，`ChannelClosedException`/`DateTimeException`是直接`Exception`子类——共八个包自声明类；「参数、范围、解析类错误无自声明类时归基类`Exception`」成文（`library/20-library-exceptions.md` §20.2）。`DateTimeException`正是「解析字符串文本出错」场景的`Exception`直系先例（`system`包`DateTime.parse`）。
10. **手册树与索引钉桩现状**：library章现存11–25（`25-regex.md`为最新），formats章为26–28（`26-ncu.md`/`27-npkg.md`/`28-abi.md`），中英两树页页对齐、`tools/nlang-docs/tests/test_tree_parity.py`严管、`translation-pending.txt`现为空，nav覆盖率门禁要求构建页集恰等于nav页集（`tools/nlang-docs/src/nlang_docs/linkcheck.py:164-210`）。符号钉桩两处：`tests/test_compiler/test_library_index.cpp:33`与`tests/test_langservice/test_symbol_index.cpp:29`的`kStdlibSymbolPin = 335`——只数自由函数声明行，类成员与枚举成员不入索引（实现为`src/langservice/SymbolIndex.cpp:167-183`：只有匹配函数声明正则的顶层行入索引，class/struct/enum头开启跳过作用域，枚举成员行两种形制皆不入；`test_library_index.cpp:75-83`分解注释四处写明class/类成员 are not free functions：net`:75-76`、Url`:77-78`、Http`:80-81`、Regex/Match`:82-83`）；`test_symbol_index.cpp:129-140`另按字母序钉死11个包名（`fs, gzip, http, io, math, net, regex, system, tensor, thread, url`），插入`json`（字母序在`io`后、`math`前）即计数12，并位移其后7个包名（`math, net, regex, system, tensor, thread, url`）的下标。手册的包计数散文现分布在三个面×两语言树——仅`library/11-conventions.md:5`作「twelve packages」，`03-program-structure/packages.md:279`与user_manual顶层`docs/user_manual/en/getting-started/install-layout.md:15`（该页不在specification/子树下，系其平级节点）作「twelve standard packages」；§11.4的「eight package-declared classes」清单（`11-conventions.md:127`）与§20.2的「The library declares eight exception classes of its own」清单（`20-library-exceptions.md:48`）措辞各异，是另两处登记面。regex（0.13.7）提供了「插新章→formats整体顺延重编号→交叉引用批量改→索引重钉」的完整先例批。
11. 勘察笔记`temp/web_framework_research.md`原记「double→string=%g 6位有损」系过时结论，已按事实2更正。

## 四、调研结论与出处

参考源码已下载`temp/ref/go_json/`（Go `encoding/json` master：`scanner.go`/`decode.go`/`encode.go`/`stream.go`/`indent.go`/`fold.go`/`tables.go`/`rfc8259.txt`），逐行读过。

| 议题 | 主流做法（出处） | 本库取向 |
| --- | --- | --- |
| 文法严格度 | Go scanner完整状态机：字面量恰`true/false/null`；数字`-?(0\|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?`，拒`1.`、`.5`、`01`、`+1`；串内<0x20控制字符拒；转义集恰`b f n r t \ / " u`；顶层值后非空白即拒（`scanner.go`全612行） | 照收（D-3），即RFC 8259逐条 |
| JS宽容度 | `JSON.parse`历史上接受`Infinity`/`NaN`形态；Python `json`默认接受非标准`NaN/Infinity`字面量 | 拒之，取Go/RFC |
| 数字保真 | Go `type Number string`原文保真（`decode.go:196-211`），默认解码转float64、`UseNumber`留原文；Jackson按token自动选int/long/double/BigInteger | 取Jackson式自动选+Go原文保真（D-2） |
| 超范围数字 | Go默认解码对`1e999`报「number out of range」错（`decode.go:843-854`转换失败即错）；Python `json.loads("1e999")`静默→`inf` | 上溢取Go：解析期即拒；下溢取JS/Python：接受为0.0（与Go分歧，D-2） |
| 数字互操作精度 | RFC 8259 §6：实现间互操作精度不保证，常见IEEE double的2^53整数界 | `getInt`/`getLong`守范围，`getDouble`对超大整数明示可有损（D-2） |
| 重复键 | Go解码后写覆盖前写（`decode.go:817-819` `SetMapIndex`） | 后胜（D-4），位置取首次出现（Dict语义免费赠送） |
| 孤立代理对 | Go把`\uD800`这类孤代理/坏对替换为U+FFFD（`decode.go` `convertString`）；WHATWG `text/event-stream`等亦U+FFFD；JS原样保留 | 取U+FFFD（D-6），原始≥0x80字节直通不校验 |
| 深度上限 | Go `maxNestingDepth = 10000`（`scanner.go:148`） | 不照搬：NLang VM调用深度1000帧且超限不可捕获（事实1），须自我守卫（D-5） |
| float输出格式 | Go按ES6规则：\|f\|<1e-6或≥1e21用`e`形且去指数前导零，否则`f`形；NaN/±Inf报`UnsupportedValueError`（`encode.go:545-601`）；JS `stringify`把NaN/±Inf→`null`（静默）；Python输出非标准`NaN` | 取Go的「非有限即错」；格式语言已有最短往返渲染（事实2），转义与哨兵词另裁（D-7） |
| 串转义宽度 | Go默认HTML转义`<>&`并无条件转义U+2028/U+2029（`encode.go:993-1060`）；JS `stringify`不转义`<>&`，只转义引号反斜杠与控制字符 | 取JS最小集（D-7），偏离Go的理由见裁定 |
| 缩进排版 | JS `JSON.stringify(v, null, 2)`：键后`": "`、每层换行；Go `MarshalIndent`同形；空对象缩进模式仍输出`{}` | 照收（D-8） |

## 五、架构裁定

### D-1 值模型=DOM类族，不用`Object`装箱、不用struct联合

`JsonValue`抽象基类+`JsonNull`/`JsonBool`/`JsonNumber`/`JsonString`/`JsonArray`/`JsonObject`六子类+`JsonKind`枚举。

- 被否选项A「`Dict<string, Object>`动态值模型」：装箱路径虽存在，但拆箱失配是host-level错误接不住（事实5），`Object`读取还要用户自己`as`且失败面不对称；类型化读取的守门职责落不进库外。
- 被否选项B「struct式tagged union」：nlang struct是纯数据无方法面（协议方法面在class，`casts.md:140-142`甚至永久禁止struct参与string衔接），无法承载虚分派。
- 采纳DOM类族后：`kind()`+基类类型化访问器是零强制转型的基类虚接口（兄弟类`as`靠布局巧合是本仓已知陷阱），类型失配错误变成可捕获、带期望/实际种类的`JsonException`。
- 六类而非「一个JsonNode带可选字段」：每类字段单一、语义自明，JSON六类型与类一一对应，无非法状态。

### D-2 数字：long|double标记表示+原文保真，上溢解析期即拒、下溢接受为0.0

- 内部表示：原文token（仅parse构造时写入，不公开）+判别式（long值或double值）。
- token无`·`无指数且`toLong()`成功 → long节点；否则`toDouble()`成功 → double节点；两者皆失败（即超出long的整数超出double范围、或超出double范围的浮点，`toDouble`对非有限即抛——事实3）→ `JsonException`「number out of range」含偏移。据此**树内double恒为有限值**。
- **下溢**（如`1e-999`按IEEE舍入规则归零）：**接受为0.0**。主流普遍接受——JS `JSON.parse`、Python `json`、Rust serde_json均返回0，仅Go `ParseFloat`对下溢同报`ErrRange`（`decode.go:843-854`与上溢同条）。裁决理由是与上溢的不对称：上溢的`inf`不可序列化（serialize处抛），必须解析期拆雷；下溢的0.0是合法有限值、序列化与下游均不成毒。rawText原文token不受影响，`1e-999`节点serialize仍原样渲染，仅`getDouble()`消费者见0.0——系IEEE表示固有代价，信息量与Go拒绝路径相当（Go只是把静默损失换成整篇文档拒收）。e2e钉一例（§七）。
- 保真回收：`serialize`直接写原文token，parse→serialize字节级往返无损（`100.0`不塌成`100`、`1E+2`不塌成`100`、`-0`保留）。用户替换值后该节点改用格式化渲染：long十进制原样；double走语言内建最短往返（事实2）。
- 类型化读取规则（均为`JsonNumber`节点上的访问器）：
  - `getLong()`：long节点原值；double节点当且仅当恰为整值且落`[−2^63, 2^63)`内放行，否则`JsonException`。`[2^53, 2^63)`区间的整值double放行：该double本身已是原文数的近似（double仅53位精度），`getLong`返回的是这近似值的整数值——精度损失发生在parse的`toDouble`一步，`getLong`不额外引入损失；手册与`getDouble`同口径明示。
  - `getInt()`：同上再收窄到int32范围，越界`JsonException`；
  - `getDouble()`：long节点转double（绝对值超2^53可有精度损失，手册明示，RFC 8259 §6同口径）；double节点原值。
- 被否选项「BigDecimal式任意精度」：nlang无十进制定点底座的现状下工程量失衡；JSON互操作所需IEEE精度已由范围检查守死。

### D-3 文法逐条照RFC 8259/Go scanner（调研表第一行）

字面量恰`true`/`false`/`null`（`True`/`TRUE`/`nan`拒）；数字文法严格（`01`、`+1`、`.5`、`1.`、`1e+`、连前导空白都不允许出现在token内）；串内`< 0x20`拒且不自动跳过；转义集恰`b f n r t \ / " u`；顶层恰一个值，值后除空白（空格/tab/CR/LF）外任何字符报「after top-level value」；空输入报「unexpected end of input」。注释、BOM（U+FEFF非空白非值）、多顶层值全拒。违例一律`JsonException`，消息携违例token起始字节偏移。

### D-4 重复键：后写胜，位置取首次出现

JsonObject内部即`Dict<string, JsonValue>`（D-11），后写覆盖值而位置不动=Dict既有条文（事实4），零自研代码；序列化与`names()`稳定按插入序。这与Go「后胜」一致，也与「键序=首见序」的通行渲染（JS对象、Go ordered-map库）一致。成本核算：每个`set`/`get`线性扫描（事实4），n键对象构造总计O(n²)；JSON对象实测规模（配置与消息体通常≤数百键）下与regex/http纯脚本包同量级可接受，不设散列索引旋钮。若日后出现大对象热点，`JsonObject`内部可自行加「名→位」索引——存储表示不公开、API面不动，本裁定不锁死该演进。

### D-5 深度守卫：`DEFAULT_MAX_DEPTH = 200`，parse与serialize对称

- 递归下降解析器每层JSON嵌套约耗2个nlang调用帧（`parseValue`→`parseObject`/`parseArray`→`parseValue`）；序列化同形递归。VM硬上限1000帧，超限是不可捕获的host-level错误（事实1）——库必须抢在VM之前拒绝，让深度违例成为可捕获、带偏移的`JsonException`。
- 200层×2帧≈400帧，留给调用方上下文与safepoint窗口约600帧余量；200层远超现实文档深度（互操作载荷≤20，JSON测试集最深约100层），且是固定常量不设旋钮（KISS，Go的10000在1000帧VM上本来就不可达）。
- serialize对**用户构造**的超深树同样以200拒绝（builder API可以造出比parse允许值更深的树）。
- 实施计划里先跑深度探针（Debug树实测200层parse+serialize往返、并验证400层仍能通过VM但被库拒绝）钉死常数余量，若实测余量不足才下调常数并记录。

### D-6 字符串解码：`\u`转义产UTF-8，坏代理对→U+FFFD；原始≥0x80字节直通

- `\uXXXX`：BMP内直接编码；`D800–DBFF`开头须紧跟`DC00–DFFF`低代理，合成码点按UTF-8 4字节编码；孤高/低代理或坏对→U+FFFD（Go `convertString`与WHATWG同款，调研表）。
- 非转义的原始字节≥0x80不做UTF-8良构校验，原样搬运。理由：转义解码必须产出字节所以必须有裁决规则，原始字节不动它就不存在「产什么」问题——无损、零热路径开销、与`JSON.stringify`宽容度一致；偏离Go（Go对输入整体做U+FFFD替换）在此登记为裁定而非缺陷。输入字节是什么，串里就是什么；下游（终端/HTML）自担渲染解释。
- `\u0000`进串合法（nlang string按长度持有，可含NUL）；深度/长度无专项上限（载荷体量由调用方`maxBodyBytes`类手段自理）。

### D-7 序列化转义：最小集

只转义`"`、`\`、`< 0x20`（`\b \f \n \r \t`两字符形，其余`\u00xx`小写hex）。`<>&`、U+2028/U+2029、全部≥0x80字节一律字面输出（UTF-8透传）。偏离Go默认（escapeHTML）的理由：JSON载荷按charset无关传输是常态，XSS防线归消费端上下文，`stringify`先例成熟且用户心智是「序列化所见即所写」；库不提供转义模式开关（无消费者不开旋钮）。非有限double（只可能来自用户构造）序列化即`JsonException`（Go `UnsupportedValueError`同款立场，拒JS的NaN→null静默）。

### D-8 排版：紧凑无空白；缩进用空格、键后`": "`

- `serialize()`：无任何多余空白——`{"a":1,"b":[2,3]}`。
- `serialize(spaces)`：`spaces > 0`时每成员一行、每层`spaces`个空格、键与值间`": "`、闭括号回退缩进；`spaces ≤ 0`视同紧凑（JS负数同款）。空对象/空数组两种模式都渲染为`{}`/`[]`，内不生成换行（Go `MarshalIndent`同款）。缩进单位只有空格不给tab（两种缩进字符的旋钮无必要）。
- 数字写原文或格式化结果（D-2），键名走D-7转义。

### D-9 `toString()`=紧凑序列化；相等性保持身份

`toString()`在`JsonValue`基类一次性重写为`serialize()`，六个叶子类继承不再各自声明——使`io.print(v)`与字符串衔接对任何节点直接可读（测试与调试主力形态）。单实现即覆盖全部叶子正是因为`serialize`的行为完全由`kind()`与基类访问器决定（D-13的渲染架构），不需要每类差异化。`toString`名绑定`Object`协议虚契约（§4.11.2），基类重写签名与契约一致、且该类中此名不再重载，合法。`equals`/`getHashCode`不重写：`==`恒身份（事实7），且nlang内建容器本就身份比较（事实4末段），DOM树比对用`serialize()`文本或逐节点访问器——不提供深比较`equals`（重写它就要兑现`getHashCode`深哈希，收益与内建容器习惯冲突）。

### D-10 异常：`const class JsonException : Exception`

- 挂`Exception`直系而非`IOException`：json没有流与描述符——`parse`的输入是已在内存的`string`，访问器错误是类型/范围程序错。同场景先例=`DateTimeException`（同为「解析字符串文本」，直接挂`Exception`，事实9）；谱系内也存在同场景的反例（`Url.parse`语法错挂`IOException`，20章`:36`），故分支按资源标准而非场景归类。六个`IOException`子类的实际分布：`SocketException`/`TimeoutException`/`HttpException`三者带传输/流语义；`UrlException`全是纯字符串编解码位点（`stdlib/url.n:5`自述无原生代码，11处抛出点均为scheme/port/IPv6字面量/percent转义之类解析，20章`:36`登记面亦只有`parse`/`resolve`/`percentDecode`/`formDecode`四个codec位点）；`GzipException`系对内存缓冲做整载荷转换（`11-conventions.md:54-55`自述「whole-buffer gzip compression」、20章`:39`）；`RegexException`违例场景是图案编译被引擎拒绝与group越界（`stdlib/regex.n:10-13`、20章`:40`）——Url/Gzip/Regex三支全是纯内存操作，系「流/描述符」标准下的既有偏例。故本分支标准是前瞻性取舍（包自声明异常除非与流/描述符资源相伴，应挂`Exception`直系），非对现状的归纳；JsonException无流无描述符，对齐`DateTimeException`一侧。
- 消息约定（英文、单行、去空格冗余）：解析错=`json: <what> at byte offset <n>`；访问器错=`json: expected <kind>, got <kind>`；缺键=`json: object has no key "<k>"`；范围=`json: number out of <type> range`；深度=`json: nesting deeper than <limit> at byte offset <n>`（parse）/`json: nesting deeper than <limit>`（serialize）；非有限=`json: cannot serialize non-finite double`。
- `JsonArray`下标越界不造新词：让`List`的`IndexOutOfBoundsException`透传（与语言§5.7的下标行为同构）；缺键用`JsonException`（缺键≠越界，Dict条文里`get`也是抛异常，事实4）。

### D-11 纯nlang单文件，零原生、零VM/编译器改动

`stdlib/json.n`一包到底：类族、解析器、序列化器全在语言内。依据与regex（2024行）、http（2134行）先例同构；语言底面已备齐全部所需——严格数值转换（事实3）、最短往返渲染（事实2）、插入序容器（事实4）、可捕获下转型（事实5）、host-level深度硬上限（事实1）、O(1)串接（`04-types/string.md:64-66`：`s = s + x`先记拼接节点、首读就地展平，循环追加不整串拷贝——序列化器直接朴素累加即可，stdlib无builder类型先例）。GLOB自动收录（事实8），npkg与IDE树零登记。内部辅助类带`Json`前缀与公开面同处一文件、内部在前公开在后（regex.n的`RE*`族→`Regex`/`Match`排布先例）。

### D-12 并发姿态：DOM单主人，可变不加锁

树与其`List`/`Dict`成员不做任何同步：一个context构树/改树/序列化完毕，整体交给另一context后不再改（所有权转移），与http响应体等既有类型同款纪律；手册库章成文。不加锁、不做只读快照（无消费者不为并发读付K锁成本；跨context并发改一棵树的竞争按普通数据竞争对待，程序自守）。

### D-13 API形态：自由函数只有`parse`，构造走公开构造器

包级自由函数只暴露`json.parse(text)`；空容器`JsonObject()`/`JsonArray()`与标量包装`JsonString(s)`/`JsonNumber(l|d)`/`JsonBool(b)`/`JsonNull()`直接是构造器（与`Regex(p)`同款用法），不另造`json.object()`式别名。写入的标量重载（`put`/`add`各6形）在库内一行装箱——`put`/`add`名未绑定任何虚契约，类内非虚同名重载合法（`07-functions/overloads.md:3-4`），构造器重载亦合法（`04-types/class.md:127`）。

读取的标量访问器7个挂`JsonValue`基类为虚方法（D-1的零转型接口）。**容器不带同名转发方法**（曾考虑的`getString(string)`/`getLong(int)`式糖衣）：nlang规范规定一旦名字绑定虚契约（继承的虚方法或实现的接口方法），该类及任何子类都不得再重载此名，同签必须一一对应契约（`04-types/class.md:337-346`）——`JsonArray.getString(int)`与继承的虚`getString()`、`JsonObject.getLong(string)`与继承的虚`getLong()`都在编译期即错；改名规避则要为纯糖衣新增14个带第三种命名概念的方法，语义零增益，否掉。种类化元素读取走两段式：先`get(name)`/`get(index)`取回`JsonValue`，再施加基类访问器——与整体值读取同一条代码路径，两个方向语义一致自动成立（D-2规则复用），无需转发层兑现。同理`serialize()`/`serialize(int spaces)`两形若为虚方法也撞此禁令，定为**基类非虚单点实现**：序列化行为完全由公开虚接口（`kind()`、七访问器、`length()`/`names()`/`get()`）递归决定，无叶子差异化必要；`JsonNumber`的原文token（D-2）经基类`protected`字段携带（`class.md:53-58`：protected达声明类与子类内部，不入用户可达面），parse构造器写入、用户构造器留空走格式化渲染。不提供`opt*`缺省值族、不提供JSON Pointer、不提供`isValid`布尔探针（`parse`失败必抛，探针重复）。完整签名见第六章。

### D-14 命名与作用域

包名`json`；类名一律`Json*`前缀——裸`JsonValue`等名不入库即与用户代码冲突的概率低，前缀同时让「import后哪些名进入作用域」一目了然（`import json;`后类名裸用、自由函数限定`json.parse`，与`regex`包同款作用域规则）。枚举成员`Null/Bool/Number/String/Array/Object`按§11.4枚举成员规范取PascalCase（`library/11-conventions.md:119`）；成员命名的安全性来自访问形式而非拼写——成员只经限定名访问、不入裸名池（`04-types/enum.md:22`「qualified member name」），故`JsonKind.String`与恒可见的预置名`String`/`Array`/`Object`（`library/11-conventions.md:17-19`）互不冲突；成员唯一的跨表起名约束是不与枚举方法同名——此条手册未成文、系编译期既有检查（`src/compiler/builder/DuplicateFieldChecker.hpp:55-65`：枚举方法名与成员同名报`conflicts with an existing definition of the same name`；成员之间的互异性在`:53`同查重）。读取访问器用`get*`前缀：手册§11.4明文允许纯访问者带`get`前缀（`11-conventions.md:121-122`，内建`getHashCode`即此族），而裸名方案与类型名相撞（`string()`/`number()`）且同名重载族会把种类失配诊断搅浑——方法名里的种类词就是异常消息里的期望词，D-10的消息口径由此免费成立。

### D-15 本版语言面零变更

无新关键字、无编译器/VM行为改动、无预置包（`lang.n`/`builtins.n`）触碰。手册同步涉及库章、异常登记、包计数散文、index/nav与README等多面登记面，范围以§八触点清单为准。lambda缺席不构成任何妥协点：本版无回调。

## 六、API面

本块为API面的**签名示意**，非可直接抄录的源码：实际文件中每个方法与构造器一律带体书写（无体的方法声明即抽象义务、无体构造器是编译错，`class.md:358-371`、`:397-400`），包内声明不带`json.`前缀——块中带限定名的形式系用户视角调用形（手册库章同款风格）。

```nlang
import json;

// 唯一自由函数：违例（文法/深度/数值范围）抛 JsonException（消息含字节偏移）
JsonValue json.parse(string text);

enum JsonKind {
    Null, Bool, Number, String, Array, Object
}

class JsonValue {                    // 抽象基类：无体虚方法即抽象声明（§4.8.9）
    public virtual JsonKind kind();
    public virtual string getString();      // 种类失配抛 JsonException
    public virtual bool getBool();
    public virtual long getLong();
    public virtual int getInt();
    public virtual double getDouble();
    public virtual JsonObject getObject();
    public virtual JsonArray getArray();
    public string serialize();               // 紧凑；非虚基类单点实现（虚契约名不可重载，class.md:337-346，D-13）
    public string serialize(int spaces);     // spaces>0 缩进，≤0 紧凑；非虚同名重载合法
    public string toString();                // = serialize()；基类一次实现、叶子继承（D-9）
    protected string rawText;                // 非用户面：仅 JsonNumber 填原文 token（D-2），protected=达子类与基类、不出导入面
}

// 叶子类须实现 kind() 与本身种类的七访问器实现（其余种类抛 JsonException）——未兑现抽象义务
// 即编译期错误（class.md:379-385）；serialize/toString/rawText 由基类提供、叶子继承（D-9/D-13），
// 下方各叶子签名块因此只列构造器与容器独有成员
class JsonNull : JsonValue {
    public JsonNull();               // 无状态；显式 JSON null 用 JsonNull()
}
class JsonBool : JsonValue {
    public JsonBool(bool value);
}
class JsonString : JsonValue {
    public JsonString(string value);
}
class JsonNumber : JsonValue {
    public JsonNumber(long value);   // int 实参经 §4.4 隐式加宽到此形
    public JsonNumber(double value); // 有限值；非有限在 serialize 处抛
}
class JsonArray : JsonValue {
    public JsonArray();
    public void add(JsonValue value);
    public void add(string value);   // 以下五个标量形自动装箱
    public void add(int value);
    public void add(long value);
    public void add(double value);
    public void add(bool value);
    public JsonValue get(int index);            // 越界抛 IndexOutOfBoundsException（透传）
    public void set(int index, JsonValue value);
    public void removeAt(int index);            // 越界抛 IndexOutOfBoundsException
    public int length();
    // 种类化元素读取=两段式 get(index)→基类访问器（D-13：容器不得重载继承的虚契约名）
}
class JsonObject : JsonValue {
    public JsonObject();
    public void put(string name, JsonValue value);
    public void put(string name, string value); // 以下五个标量形自动装箱
    public void put(string name, int value);
    public void put(string name, long value);
    public void put(string name, double value);
    public void put(string name, bool value);
    public JsonValue get(string name);          // 缺键抛 JsonException
    public bool has(string name);
    public bool remove(string name);            // 有无删除分别返回 true/false
    public List<string> names();                // 插入序快照
    public int count();
    // 种类化读取=两段式 get(name)→基类访问器（D-13）
}

const class JsonException : Exception {
    public JsonException(string message, Exception cause = null) {
        super(message, cause);   // UrlException/RegexException 逐字同形先例
    }
}
```

约定与边界行为：

- **null与缺键的区分**：`get`到的`JsonValue`若是显式JSON null，是`JsonNull`实例（非空引用），判据`v.kind() == JsonKind.Null`；键不存在则是`JsonException`。空串`""`、`0`、`false`都是实值不误伤。
- **方法对null接收者**：经null的`JsonValue`方法调用抛`NullPointerException`（库面通则§11.2，根源§4.8.10`class.md:413-415`）。
- `getObject()`/`getArray()`=种类预检+下转型（`InvalidCastException`兜底不可能触达），错误消息仍归`JsonException`口径。
- 用户构造`JsonNumber(0.1)`序列化经语言内建渲染得`0.1`（最短往返）；parse所得`0.10000000000000001`原样保留原样输出。
- `JsonException`的构造器必须是显式转发形态：§8.4规定未声明构造器时只合成零参构造器（`message`留在默认值），携原因文本就得显式`super(message, cause)`；抛出用构造器调用形态、不带`new`（stdlib全体`throw new`出现0次，`throw UrlException(...)`等为通用形态）。

典型读侧：

```nlang
import io;
import json;

int main() {
    JsonValue root = json.parse(io.readFile("config.json"));
    JsonObject cfg = root.getObject();
    int port = cfg.get("port").getInt();            // 缺键或种类失配：JsonException，消息自描述
    if (cfg.has("verbose") && cfg.get("verbose").getBool()) {
        io.print("verbose on");
    }
    JsonArray hosts = cfg.get("hosts").getArray();
    for (int i = 0; i < hosts.length(); i = i + 1) {
        io.print("host " + hosts.get(i).getString());
    }
    return port;
}
```

典型写侧：

```nlang
import io;
import json;

int main() {
    JsonObject payload = JsonObject();
    payload.put("status", "ok");
    payload.put("count", 3);
    JsonArray values = JsonArray();
    values.add(1);
    values.add(2);
    payload.put("values", values);
    io.print(payload.serialize(2));
    return 0;
}
```

## 七、测试策略

TDD：每条e2e先行且先红。基线=当前dev树全绿（0.13.8收官值：e2e双腿各2039，ctest Debug 140/ASan 166/IDE树167）。

**e2e用例族**（命名随`stdlib_<包>_<侧面>.n`既有包段惯例，如`stdlib_url_parse.n`/`stdlib_regex_quote.n`；逐文件入`tests/e2e/manifest.txt`，登记格式`test_name expected_exit_code`、登记值0）：

| 文件 | 钉死内容 |
| --- | --- |
| `stdlib_json_parse_basic.n` | RFC 8259 §2全部示例接受+字段读取；缩进样例 |
| `stdlib_json_parse_reject.n` | 严格矩阵（≥60违例向量）：`True`/`nul`、`01`/`+1`/`.5`/`1.`/`1e+`、串内裸LF、`\x`转义、尾逗号、注释、BOM、`1 2`多值、空输入；逐一断言可捕获+偏移 |
| `stdlib_json_numbers.n` | long边界±9223372036854775808/7；超long降double；`1e999`拒；下溢`1e-999`接受为0.0且serialize仍原样渲染token（D-2）；`-0`保真；2^53界内外`getDouble`行为；`getInt`/`getLong`跨种类矩阵与范围异常 |
| `stdlib_json_unicode.n` | `\u` BMP/代理对合成4字节UTF-8；孤代理→U+FFFD；原始UTF-8直通；`\u0000`入串 |
| `stdlib_json_serialize.n` | 紧凑/缩进排版（空容器、嵌套缩进、键后`": "`）；转义表逐字符；非有限构造值抛；原文保真（`100.0`/`1E+2`/`-0`/超长整数）；`toString()`渲染=紧凑serialize（含经字符串衔接路径）、两个内容相同而不同节点的`==`为false（D-9身份比较） |
| `stdlib_json_roundtrip.n` | 固定向量集：`serialize(parse(x)) == x`的字节稳定子类；`parse(serialize(v))`再序列化幂等 |
| `stdlib_json_edit.n` | builder装箱重载、覆盖写位置不动、`remove`、`names()`插入序、重复键后胜、`JsonNull`显式null、覆盖parse所得token节点后serialize转格式化渲染（D-2替换条款：`100.0`节点被`put`覆盖数值后输出不再带`.0`） |
| `stdlib_json_depth.n` | 200层接受、201层拒且可捕获（parse与serialize两侧）；探针期实测钉常数余量（D-5） |

**差分对拍**：实现期拉取Go `encoding/json`的`decode_test.go`/`encode_test.go`（入`temp/ref/`），筛出与D-2/D-6/D-7无裁决分歧的用例改写进e2e断言；分歧用例的预期按本设计裁定书写并就地注记出处（先自己实现后比对，不照抄Go错处）。

**手册围栏**：`26-json.md`中全部```nlang围栏示例经手册门禁真实编译运行（regex先例同款）。

**门禁**：例行批=Debug（断言）+ASan+IDE三树与e2e串行（`tools/gate.sh`判决行为准）；本库纯函数无共享可变状态，不适用并发百次压测条款（D-12），但`stdlib_json_depth.n`在Debug树必跑（断言+慢树才是深度余量的真实考场）。慢测试条款不涉及（单文件均<30s）。

**性能**：无硬指标；`examples/json_tour.n`顺带1MB级载荷往返读数记录进台账（纯nlang解析器与regex/http纯脚本包同量级即符合预期）。

## 八、集成点与文档同步

提交批次内的全部触点（中英两树同步为完成定义的一部分）：

- 新增`stdlib/json.n`：GLOB自动入npkg与IDE树（事实8），构建文件不动；测试与文档登记面如下列条目（事实8的「零登记」不覆盖这些面）。
- 手册新增`docs/user_manual/en/specification/library/26-json.md`与`docs/user_manual/zh/specification/library/26-json.md`（同批，`translation-pending.txt`保持为空）。章内容=API面+语义要点+null/缺键区分+深度上限+单主人并发纪律+异常口径。
- formats章重编号（照regex 0.13.7先例批）：`formats/26-ncu.md`→`27-ncu.md`、`27-npkg.md`→`28-npkg.md`、`28-abi.md`→`29-abi.md`（中英两树），全部交叉引用与站点内锚点批量改，`index.md`（两树）与`docs/mkdocs.en.yml`/`docs/mkdocs.zh.yml`的nav条目行同步更新（nav覆盖率门禁：构建页集恰等于nav页集——事实10）。
- `library/20-library-exceptions.md`（两树）：§20.1表加`json`行、§20.2（`:48`）「eight exception classes」→「nine」并在列举内挂`JsonException`（注明直系挂`Exception`的D-10理由）；导语（`:5`）包章号列举「the packages of chapters 13…25」补`26`。
- 包计数与异常类计数散文（本条所列四处×两树共八处；§20.2清单与README的同类登记另见相邻两条目）：`library/11-conventions.md:5`「twelve packages」→「thirteen」并在包名列举加`json`、§11.4（`:126-133`）「eight package-declared classes」→「nine」加`JsonException`、`03-program-structure/packages.md:279`与user_manual顶层`docs/user_manual/en/getting-started/install-layout.md:15`（不在specification/子树下，系其平级节点；zh树同名）「twelve standard packages」→「thirteen」（事实10）。
- `README.md`与`README.zh-CN.md`：包清单表（`README.md:189`、`README.zh-CN.md:177`）与包计数散文（`README.md:224`「the standard twelve packages」、`README.zh-CN.md:207`「标准库的十二个包」）四处加`json`条目、twelve→thirteen/十二→十三（中英成对，同批）。
- 测试登记：两处`kStdlibSymbolPin = 335`按新库面重算（自由函数面只加`parse`，D-13成立即336；类成员与枚举成员不入自由函数索引——事实10；现stdlib树无包声明枚举，`JsonKind`系该论断首个消费者，重算时一并复核）；`test_library_index.cpp:60-83`陈旧分解注释同批重写——表头「315 functions」与现pin 335漂移、分解和仅332、缺`concurrent`等包目；`test_symbol_index.cpp:129-140`字母序11个包名钉桩（`size()==11`断言加`fs`…`url`逐行列举）改12——`json`插`namespaces[4]`（0基下标，随钉桩断言形制）、其后7个包名行（`math`…`url`）与断言同移；e2e族（`stdlib_json_*.n`）入`tests/e2e/manifest.txt`（两列`test_name expected_exit_code`），`examples/json_tour.n`入`tests/e2e/examples_manifest.txt`（行格式`name expected_exit_code [stdout_substring]`——第三列系可选stdout子串断言，清单头`tests/e2e/examples_manifest.txt:2`已声明该格式，存量行如`regex_tour 0 ok`；与`manifest.txt`两列制不同，勿照抄）。
- `CHANGELOG.md`、`VERSION`（版本号随roadmap的v0.13.11节奏，见2.3）；`docs/roadmap.md`改写（本spec同批）：原v0.13.11「WEB框架」行递延为题面v0.13.12（文字不动），新立「v0.13.11：JSON库」条目（一句话范围+用户裁定出处）。
- 收尾`tools/manual_sync_check.py`：基线以来每提交逐判定（本版为json库语言面零/标准库面=json.n+手册+异常登记），漂移清零后`--update`刷新`SYNC_BASELINE`。
- IDE树全量ctest（编译器零改动但标准库新包进预置闭包，按既有IDE树门禁要求）。

## 修订记录

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| 1 | 2026-11-15 | 初稿：目标范围、11条勘察事实、Go/RFC/JS/Python调研表、D-1~D-15裁定、API面、测试策略、集成点 |
| 2 | 2026-11-15 | 对照stdlib勘察报告核验修订：符号钉桩267→335更正并补字母序包名钉桩与nav/散文登记面（事实10、§八）、e2e命名改`stdlib_json_*.n`惯例（§七）、`JsonException`显式构造器形态（§8.4零参合成构造器携不了message，§六）、Dict线性扫描代价（事实4/D-4）、`is`算子缺席（事实5）、O(1)串接与内部类排布（D-11）、访问器`get`前缀理由（D-14） |
| 3 | 2026-11-15 | 第一轮第三方评审（1 High/2 Medium/6 Low/3 Info，逐条独立核实）后修订：D-13重写——虚契约名不可重载（`04-types/class.md:337-346`），容器14个同名转发删除改两段式读取（`get`→基类访问器），`serialize`两形改基类非虚单点实现、`protected rawText`携原文token；D-9相应改`toString`基类单实现；§六签名与两示例、§七断言矩阵（补D-9身份、D-2替换条款）同步；§八补README两文件登记、「零登记」限定覆盖面、测试登记条目点名`test_library_index.cpp:60-83`陈旧分解注释（表头315≠pin 335、分解和332、缺`concurrent`包目）；事实8行号`:267-268`/`:276-279`更正；事实10位移4→7；调研表`scanner.go:13`→148、613→612行；D-14枚举成员命名理由更正为限定名访问形式；D-2补`[2^53,2^63)`精度说明；代理后补的第7-10条全文经核属实，同批落实四项——D-15手册同步范围改以§八清单为准、§六补叶子类抽象义务注（八项义务未兑现即编译期错误，`class.md:379-385`）、§八点明`examples_manifest.txt`三列行格式与`manifest.txt`两列之别（`examples_manifest.txt:43-45`实证）、D-14大写理由改引§11.4枚举成员规范（`11-conventions.md:119`）。拒判两条：§六示例缺`import io;`（第248行已在，误报）、调研表补现状列（第三章事实已承载） |
| 4 | 2026-11-15 | 第二轮第三方评审（1 Medium/6 Low/3 Info，无High；十条经逐条一手核实全部属实、13条拒判全部接受）后修订：D-14恒可见名出处改`11-conventions.md:17-19`；事实5装箱通路按`object.md`§4.11.1`:13-19`改写；事实10索引论断锚点改实现`SymbolIndex.cpp:167-183`（附注释佐证句`:75-83`）；§六块首补签名示意声明（无体=抽象、无体构造器编译错、限定名系用户视角）；D-2补下溢裁定（接受0.0、rawText不受影响、与Go分歧，调研表与§七numbers行同步）；§八更正——散文登记合计六处→八处、20章导语章号列举补`26`、`install-layout.md`写清树位置（specification/平级）、`examples_manifest.txt`第三列更正为可选stdout子串断言（清单头`:2`）并补`JsonKind`系枚举不入索引首个消费者复核点；§六约定null接收者补根源引用§4.8.10。六项API新形态合法性复审通过，事实10与§八的336/12/7算术复核成立 |
| 5 | 2026-11-15 | 第三轮第三方评审（1 Medium/2 Low/4 Info另加1条不计级观察，逐条现树一手核实全部属实；第二轮十处修复全部落档且其余引注属实）后修订：D-10谱系论据更正——「六个`IOException`子类全部带传输/流语义」不实，实际仅Socket/Timeout/Http三者带传输语义（`UrlException`错归一事第四轮纠正）、Url/Gzip/Regex三支皆纯内存偏例（`regex.n:10-13`、20章`:36`/`:39-40`），分支标准改述为「有无流/描述符资源相伴」；事实10分解注释佐证三处→四处（`test_library_index.cpp:75-83`逐处列位）；两处异常类计数登记面按各自原文措辞分别引用（§11.4`:127`、§20.2`:48`）；事实5`object.md`锚点精确化（`:15-17`/`:7-8`/`:13-14`/`:18-19`）；§八「每树四处」限定为本条所列（§20.2与README在相邻条目）；字母序插位消歧为`namespaces[4]`（0基）；D-14成员起名约束补编译期实现引注（`DuplicateFieldChecker.hpp:55-65`）；v4行「§七」位置指称更正为事实10与§八 |
| 6 | 2026-11-15 | 第四轮第三方确认评审（1条计门槛Medium+1条不计门槛Low，逐条现树一手核实属实；第三轮八处实质修复全部落档、清单内引注全部属实）后修订：D-10分布再纠正——`UrlException`不属传输/流语义（`url.n:5`无原生代码、11处抛出点与20章`:36`登记面皆纯codec解析），带传输语义者仅Socket/Timeout/Http、Url/Gzip/Regex系三支偏例，补`Url.parse`同场景反例说明与「分支标准系前瞻性取舍非现状归纳」定性；v5行「实际四者带传输语义」同步更正；事实10包计数散文按三面各自原文措辞分别引用（「twelve packages」仅`11-conventions.md:5`，`packages.md:279`与`install-layout.md:15`原文「twelve standard packages」） |
