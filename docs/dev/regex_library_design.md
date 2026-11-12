# 正则表达式库设计(Spec)

v0.13.7路线图条目1：给nlang标准库一个正则表达式库。本文是概要设计，裁定目标与边界、引擎路线、语法面、API面、文本模型与验收口径；详细设计（指令编码、解析器拆分、用例编号表、数字常量）归实施计划。路线图v0.13.7的条目2（合并master与dev）是独立事项（既有裁定：等master完工再做），不在本spec管辖内。

## 一、目标与范围

### v1做

- 纯脚本标准库包`regex`（`stdlib/regex.n`）：零新增native模块、零新增native成员、零语言面变更。
- RE2式安全正则：自动机执行（Thompson构造+NFA模拟，即Pike VM），最坏情形时间线性；不支持反向引用与环视断言。
- 语义取最左优先（leftmost-first），与Python、JavaScript、Go、Rust的结果选择规则一致。
- 捕获组（编号+命名）、替换模板、split、`quoteMeta`。
- 行内标志`(?i)` `(?m)` `(?s)`及其组合、局部形态`(?flags:re)`。
- UTF-8码点粒度的通配与字面量匹配；匹配结果一律报字节偏移，与String的字节语义同口径。
- 差分测试：与本机Go标准库`regexp`（RE2系纯实现）对拍行为。
- 手册新增`library/25-regex`章（中英成对），格式章25–27顺延为26–28（照0.13.5的新章插入流程）。

### v1不做（各有独立理由，非「已知限制」式递延）

- **反向引用`\1`**：不存在已知的非回溯实现——RE2 README原文即「不支持只有回溯解法已知的构造，因此不支持反向引用与环视」；Rust regex crate以同一理由排除。支持它等于放弃线性时间上界，与D2的安全承诺互斥。编译期拒绝并给出明确诊断，不当字面量静默吞掉。
- **环视`(?=) (?!) (?<=) (?<!)`**：同上。Rust README把「缺环视与反向引用」明说成换取`O(m·n)`上界的代价——两大自动机阵营的立场一致。
- **Unicode属性类`\p{...}`与非ASCII大小写折叠**：标准库没有Unicode属性表，引入即一次性携带数千条范围数据。v1的`\d \s \w`与POSIX类取ASCII-only（与Go的Perl类口径完全相同），`(?i)`取ASCII大小写折叠——与类面边界配套。日后补足是纯增量扩展，不破任何既有接口。
- **`(?x)`扩展模式、占有量词、原子组**：回溯生态（PCRE/Perl）的专属面，RE2与Rust均不提供；补足是纯增量。
- **类集合运算符`[a&&b]`/`[a~~b]`**：所选Go基线没有这些运算符——类内`&`是普通字面量（本机Go 1.27.3探针实测：`[a&&b]`编译通过且只匹配单个`&`）。RE2 C++方言与Rust/Java提供运算符语义，将来补足须以破坏`[a&&b]`现有字面量读法的形式引入，属纯增量而非本版必要。
- **流式匹配（逐块喂入的增量API）**：需要引擎状态外显；脚本场景的输入通常已在内存，v1整串匹配。
- **`\Z`、`(?U)`**：所选Go基线两者都不提供——Go语法表里`$`已等同`\z`（"like `\z` not `\Z`"），`\Z`的「末行换行前」语义在基线内没有对应物；`(?U)`是贪婪与非贪婪（lazy）的全局交换，低频，用`.*?`等显式写法覆盖；补足是纯增量。

### 版本落点

随v0.13.7发布。VERSION与CHANGELOG在收尾时提升；用户可见功能变更同步更新手册「语言规格」的标准库清单，并核对其「已知限制」有无可以解除。

## 二、关键事实（勘察结论）

1. 标准库现有13个包，无正则面。String方法面（`stdlib/lang.n`）已冻结（Phase 11裁定），正则所需全部可由包级API满足，无需解冻String。
2. String已有字节级访问桥接：`s[i]`按字节读`ubyte`（越界抛运行期错误）、`s.charAt(byteIdx)`返回第`byteIdx`字节处开始的码点（`stdlib/lang.n:101`，手册§4.3）。引擎因此可直接在输入串上逐码点解码，扫描路径（直接`charAt`对照`io.toBytes`转`byte[]`后数组下标扫描）是实现细节、在实施计划以探针择定，不进接口与语义；结果提取一律用原输入串`substring`（偏移=字节偏移）。全部是既有接口，无语言面变更。
3. 纯脚本标准库包先例已立：`url.n`（0.13.4）、`http.n`（0.13.5）整包纯脚本零native。正则引擎=递归下降解析器+Thompson构造编译+解释循环，是同一路线的延续；且引擎的编译产物是指令数组这种纯数据，将来加native执行内核不需要动任何接口。
4. 本机参照对象：Go标准库`regexp`（`D:/dev/go/go/src/regexp/`）。RE2系纯实现——`syntax.Prog`扁平指令数组+NFA模拟+`onepass`与字面量前缀快路；包文档承诺线性时间与并发安全。其`regexp/syntax`有完整语法表文档、`regexp`有测试语料，是本机可读的对照对象与差分预期值来源。
5. RE2 README（google/re2，已取回`temp/re2_readme.md`）：安全第一、线性时间保证、明确排除反向引用与环视；文中给出Russ Cox三部曲链接（swtch.com/~rsc/regexp/regexp1–3），其中regexp2即「虚拟机方法」（Pike VM/NFA模拟）——最左优先的结果选择可以在单次扫描输入中完成，不需要回溯栈。
6. Rust regex crate README（已取回`temp/rust_regex_readme.md`）：同样排除环视与反向引用，交换条件是`O(m·n)`最坏时间——第二方印证。
7. 并发：nlang是多载体共享堆上下文（0.12.6教训：单次绿不等于并发安全），跨上下文共享的对象要么可变加锁、要么构造后不可变。
8. 手册结构：库章现为11–24（24-gzip收尾），格式章随后编号25–27；新章插到25-regex、格式章顺延，0.13.5做过一轮完整流程（交叉引用、中英成对、索引钉重算），清单照抄即可。

## 三、调研结论与出处

| 议题 | 结论 | 出处 |
|---|---|---|
| 引擎路线 | 自动机（NFA模拟/Pike VM）是安全正则的行业共识路线；回溯引擎（Python `re`、PCRE、Java、.NET）对不可信输入有指数级灾难（ReDoS） | RE2 README（`temp/re2_readme.md`）；Cox三部曲（README内链接）；Rust regex README（`temp/rust_regex_readme.md`） |
| 排除清单 | 反向引用与环视是「只有回溯解已知」的两类构造，RE2与Rust regex两家一致排除 | 上述两份README原文；RE2原理句逐字引用见D3 |
| 结果选择语义 | 最左优先（Perl序）是Python/JS/Go/Rust的公共默认；POSIX最左最长是另一语义且用户群小，v1不提供 | Go `regexp`包文档（本机源码`regexp.go`）；RE2 README（两种模式的对照说明） |
| 语法面基线 | 以Go `regexp/syntax`的Perl模式语法表为基线（覆盖PCRE常用面且不越出自动机能力） | 本机`D:/dev/go/go/src/regexp/syntax/doc.go`全表 |
| API形态 | Go的`(All|Find|FindAll)(String)?(Submatch)?(Index)?`方法族是历史包袱式的组合爆炸；按nlang「一个包一组类方法」的idiom压缩成七个方法 | 本机`regexp.go`包文档；`stdlib/lang.n`命名先例 |
| 替换模板 | `$`后取最长的字母数字下划线串作名字：全体为数字且无前导零（单独`$0`例外）按组号展开，否则按组名查找；`${name}`定界形态；`$$`产出字面`$`；不存在或未参与的组一律替换为空串 | Go `Regexp.Expand`文档与`extract`实现（本机探针实测：`$1x`整体作名字`1x`、`$01`不成组号、`$10`按组号10——无99封顶）；Rust regex的expand形态同构 |
| 类与异常形态 | `const class`+`XxxException : IOException`+包级自由函数 | `url.n`的`UrlException`、`http.n`的`HttpException`先例；Go `regexp.QuoteMeta` |
| 开源对照程序 | 先自己TDD实现，再对照本机Go源码完善；分歧记Ruling不照抄 | 用户裁定（2026-11-08开源对照程序，0.13.4/0.13.5已各走一轮） |

## 四、架构裁定

**D1 引擎路线：纯脚本Pike VM。** 模式文本→递归下降解析器→AST→Thompson构造→扁平指令数组→NFA模拟循环。模拟只维护一条按优先级排序的活跃线程表，每读入一个码点推进一步；线程表按指令槽位去重（同一槽位一步只留最高优先级线程），故活跃线程数≤指令数，最坏情形时间为O(输入码点数×指令数)，无引擎级指数爆炸，也无回溯栈。最左优先的结果选择在首次到达接受状态时即定案（Pike VM性质）。理由：教学项目价值（引擎本体即可读的nlang源码）、不可信输入安全、双主流参照（Go/Rust）在本机可读、纯脚本包先例成立。代价：脚本解释循环的吞吐低于原生引擎——D9承接，接口不留遗憾。

**D2 语义：最左优先，部分匹配为默认。** 未锚定模式默认在输入中搜索（同Go/Rust/JS；RE2的FullMatch是另一条API，nlang用`\A...\z`表达整串匹配）。结果选择取Perl序：交替（alternation）左枝优先、贪婪优先。理由：与用户从Python/JS/Go/Java带来的直觉一致；POSIX最左最长是独立语义且使用面窄，v1不提供，将来加`longest()`形态是纯增量。

**D3 语法面：以Go RE2语法的Perl模式子集为基线。** 收录表：

| 构造 | 形态 | 说明 |
|---|---|---|
| 字面量 | `abc` | 含多字节字面量，按UTF-8码点整体匹配 |
| 通配 | `.` | 任一码点；不匹配`\n`除非`(?s)`；无效字节序列按单字节伪码点处理（姿态同Go的RuneError，见D4） |
| 选与组 | `a|b` `(re)` `(?:re)` `(?P<name>re)` `(?<name>re)` | 命名组两种写法都收（Go同）；名字重复编译期拒绝 |
| 重复 | `* + ? {n} {n,} {n,m}` | 默认贪婪，后缀`?`转lazy（Pike VM线程表排序调整，无额外实现面）；计数上限1000（同Go syntax上限） |
| 标志 | `(?i)` `(?m)` `(?s)` `(?flags)` `(?flags:re)` | 标志语法随Go syntax表三形态：置位`xyz`、清除`-xyz`、`xy-z`置`xy`清`z`（如`i-s`）；`(?flags)`从出现处起在当前组内生效至组尾，`(?flags:`限定局部；i=ASCII大小写折叠；m=`^ $`行界；s=通配含换行 |
| 空宽 | `^ $ \A \z \b \B` | `^ $`在`(?m)`下行界；`\b \B`按ASCII `\w`集定义词界（同Go） |
| 转义 | `\a \f \t \n \r \v`、标点转义、`\123`八进制、`\xHH`、`\x{10FFFF}`、`\Q...\E` | 与Go syntax转义表一致 |
| 类 | `[abc] [^abc] [a-z]` `[[:alpha:]]`等POSIX ASCII类 `\d \D \s \S \w \W` | 类内转义支持；Perl类ASCII-only（同Go） |

排除清单及独立理由见「一、v1不做」；排除的构造在编译期一律报`RegexException`（诊断含模式内字节偏移与原因文本），绝不静默降级为字面量——唯一例外是类内的`&`与`~`恒按普通字面量处理（同Go基线，见「一、v1不做」相应条目）。程序规模设三道上限——模式嵌套高度、编译后指令数、重复计数——超限各报专文`RegexException`（RE2的内存预算思想：不可信模式不得撑爆内存）；具体数字在实施计划钉，spec钉原则与专文要求。

**D4 文本模型：码点粒度匹配，字节偏移报告。** 扫描路径是实现细节、按事实2在实施计划以探针择定；匹配单位是UTF-8码点——`.`与字面量按码点比对，无效UTF-8字节序列按单字节伪码点处理（等价于Go把无效字节解码为U+FFFD再匹配的可见行为，文本本身不做替换）。`\d \s \w`及POSIX类只认ASCII（同Go口径），`(?i)`只对ASCII做大小写折叠，`\b`按ASCII词集。理由：nlang的String本就是字节语义（Phase 11），偏移口径与`substring`/`indexOf`全库一致；`.`若按字节会让中文文本里`a.b`匹配`a中b`的部分字节对，是比「`\w`不认汉字」更隐蔽的惊讶面。手册把「`.`按码点、类与折叠按ASCII」的边界写成正面规则而非限制清单。

**D5 API面：一个`Regex`类+一个`Match`类+一个异常+一个自由函数。** 压缩Go方法族的组合爆炸（详见「五、脚本层API面」）。不引入`Matcher`迭代器类（Java式）：`matches()`一次返回全部结果的`List<Match>`，nlang的`List`与`foreach`已覆盖迭代需求。迭代器返回面的动机是流式惰性消费（本机Go基线已为`All`族提供迭代器形态，动机同样是流式而非回溯），而脚本输入整串在内存、结果集受输入长度界定，惰性面没有对应场景。

**D6 包与命名。** 包名`regex`（单段，同`net`/`url`）；`import regex;`后`Regex`、`Match`、`RegexException`按普通类进作用域，自由函数经限定符调用（`regex.quoteMeta(s)`，`net.ping`形态）。名为`regex`的用户包按重复包规则拒绝。`RegexException`声明为派生自`IOException`的`const class`（`UrlException`/`HttpException`先例：模式来自配置与文件读取的场景常见，`catch (IOException e)`一并覆盖）；消息带模式字节偏移与英文原因文本。该异常的契约是两组共四类、每类文案独立：编译拒绝三类（语法错误、规模超限、不支持构造）+调用方错误一类（组下标越界、未知组名——运行期传给`group()`的错参）。调用方参数错误由本包异常承载有net先例（`SocketException`兼收参数错误，手册§20登记在册），`catch (IOException)`一并接住参数错误是既受认可的口径；越界/未知名字不当字面量或空串静默吞掉——那是拼写错误，必须当响报出。

**D7 线程安全：构造后不可变。** `Regex`为`const class`：构造完成即编译完成，此后无任何可变状态；每次匹配调用自分配线程表，不写`Regex`；`Match`是独立值对象。因此`Regex`可被多载体多上下文并发复用（D1的线程表不共享、D6的const类、0.12.6教训要求并发压测钉住）。

**D8 编译复用，不提供隐式编译的自由函数。** 不学Go/Python的包级`MatchString`便捷函数：隐式编译是性能陷阱（每次调用重编译），nlang一次性的用法写`Regex(pat).isMatch(s)`已经最短。自由函数只给`quoteMeta`（把不可信文本变成字面量模式，纯脚本即可，Go `QuoteMeta`对应）。

**D9 性能姿态：线性上界是验收项，吞吐不设本版门槛。** 用户的数量级门槛（与Node.js同数量级）裁定对象是net/http基准行，本版无对应的用户门槛裁定。本版性能验收：反例形态（回溯杀手`(a*)*b`、`(a|aa)*b`、嵌套重复等）在固定长输入上有界完成——e2e钉时间上限（留足机器余量防误报）；非锚定模式有字面量前缀时走`String.indexOf`（native内建）定位起点扫描，锚定模式只从输入起点扫描一次。引擎吞吐做基准记录入文档，作日后回归参照，不进当版门槛。理由：门槛是对用户承诺，没有用户场景裁定就不发明数字；线性上界才是本版真正的性能承诺。

**D10 split语义取Go规则。** `split(input, limit)`：`limit < 0`不限段数，`limit == 0`返回空表，`limit > 0`至多`limit`段；捕获组不进结果（区别于JS/Python把捕获内容混入结果——分割的答案是文本片段序列，limit数段数的语义因此才干净；Go同）。空匹配相邻规则同Go（本机探针实测钉死）：起始于上一次匹配结束字节的空匹配跳过、其余空匹配照常产出——`"a*"`对`"ab"`得`matches = ["a", ""]`（字节1处的空匹配跳过，串尾字节2处的空匹配保留）、`split = ["", "b"]`（串尾空余量不进结果，串首空段保留）。

## 五、脚本层API面（stdlib/regex.n）

```
const class RegexException : IOException      // 编译拒绝：语法错误 / 规模超限 / 不支持构造；
                                              // 调用方错误：组下标越界 / 未知组名（D6）

const class Regex {
    public Regex(string pattern)               // 编译；失败抛RegexException（消息带模式字节偏移）
    public string pattern()                    // 原模式文本
    public bool isMatch(string input)          // 是否匹配，不提取捕获
    public Match match(string input)           // 首个匹配；无匹配返回null
    public List<Match> matches(string input, int limit = -1)
                                               // 全部不重叠匹配（左到右）；limit<0不限、limit>=0封顶条数（Go规则）；
                                               // 起始于上次匹配结束字节的空匹配跳过（D10）
    public List<string> split(string input, int limit = -1)   // D10
    public string replaceAll(string input, string template)
    public string replaceFirst(string input, string template)
}

const class Match {                            // 只读值对象
    public string text                         // 整体匹配
    public int start                           // 整体匹配起始字节偏移
    public int end                             // 整体匹配结束字节偏移（不含，同substring）
    public int groupCount                      // 组数（含0号）
    public string group(int index)             // 组内容；未参与组=空串；越界抛RegexException
    public string group(string name)           // 命名组；未知名字抛RegexException
    public int groupStart(int index)           // 组起始字节偏移；未参与组=-1（Go的SubmatchIndex -1形态）
    public int groupEnd(int index)             // 组结束字节偏移；未参与组=-1
}

public string regex.quoteMeta(string s)        // 转义全部元字符，得到按字面匹配的模式（Go QuoteMeta）
```

替换模板规则（D5/D10之外的补充规则）：`$$`产出字面`$`；`$name`与`${name}`按名字引用——紧跟`$`的字母、数字、下划线串一律取最长作名字（`$1x`的名字是整个`1x`，`$10`是组号10，要引用组1再接文字须写`${1}x`）；取出的名字全体为数字且无前导零时按组号展开（组0即整个匹配），否则按组名查找；组不存在——无论数字还是名字——替换为空串，未参与组同样替换为空串；不构成引用的`$`（后面既无名字字符也无第二个`$`，或`{`没有闭合`}`）原样按字面`$`处理（Go `Expand`与`extract`规则，本机源码`regexp.go`文档与实现核对、实测`$!`→`$!`、`end$`→`end$`、`${1`→`${1`）。

`match`返回null的理由：类引用可空是nlang既有语义（§4.3只有`string`没有null），而「无匹配」是高频正常结果不是错误，不该走异常；bool场景用`isMatch`，需要内容才接`Match`。

**D11 与字符串字面量的交互：照普通字面量写，本版不引入原始字符串。** 正则库与nlang字面量层有两处叠合，都按既有语言规则消化，不是正则库自己的机制：模式里的正则反斜杠在nlang字面量里须写成双反斜杠（`\q`、`\x`这类未知转义是编译错误，nlang没有原始字符串——`(?P<key>\\w+)`即标准写法）；替换模板的`$`与字符串插值的`${identifier}`语法撞面，经`$$`美元转义递送——`"$${value};"`在nlang层产出字面`${value};`，再由正则层展开`value`组；模板里要产出字面`$`则写`"$$$"`（字面量层折成模板层`$$`）。不借本版引入原始字符串（反引号面）的理由：这是语言面变更，须按语言子项目自己的流程单独立项裁定，不能被标准库版本顺带；双反斜杠成本是C/Java/JSON系语言的行业惯例形态（Java正则同样写`"\\w"`），手册把两处叠合写成正面规则配真实运行示例。

使用示例（手册围栏示例的蓝本，随包`examples/regex_tour.n`）：

```nlang
import io;
import regex;

int main() {
    Regex re = Regex("(?P<key>\\w+)=(?P<value>[^\\s]*)");
    Match m = re.match("server=127.0.0.1 port=8080");
    if (m != null) {
        io.print("key=" + m.group("key") + " value=" + m.group("value") + "\n");
    }
    List<Match> all = re.matches("server=127.0.0.1 port=8080");
    io.print("count=" + all.length().toString() + "\n");
    io.print(re.replaceAll("a=1 b=2", "$${value};") + "\n");   //$$ is a dollar escape; the template layer sees ${value};
    return 0;
}
```

## 六、测试策略

- **TDD**：e2e用例族`tests/e2e/stdlib_regex_*.n`（语法解析、字面量与类、重复与lazy、标志、编号/命名捕获、空匹配与边界、split、替换模板、错误诊断、规模上限），每用例先RED后GREEN；编译真`.n`、VM跑真字节码（既有测试哲学）。
- **差分对拍**：从Go `regexp`与`regexp/syntax`的测试语料（本机源码树内）选取表驱动语料，先用本机Go一次性生成预期值、固化为nlang e2e断言表——nlang引擎结果必须与Go一致（排除面与ASCII口径差异处除外，差异逐条登记Ruling）。错误用例也对拍：不逐字比文案，比「拒绝/接受」与错误位置类别。已知登记差异（差分语料构造时避开或按nlang语义改写）：分组名重复Go 1.22起允许而nlang拒绝（`group(name)`歧义不可接受）；`(?i)`对非ASCII Go折叠而nlang按D4不折叠；`\1`与`(?=`两家都编译期拒绝（Go文案为`invalid escape sequence`/`invalid or unsupported Perl syntax`，行为同类仅文案不同）。
- **线性上界**：D9反例形态+固定长输入（如20万字符）钉时间上限e2e；上限取观测值的宽松倍率，防机器抖动误报。
- **并发**：`Regex`多上下文共享压测百次+ASan（单次绿不算数，0.12.6先例）。
- **示例**：`examples/regex_tour.n`随包发布，手册围栏示例真实编译运行。

## 七、风险与开放问题

1. **脚本引擎吞吐**：热点文本场景（大日志扫描类）会慢于原生引擎一个数量级。缓解：字面量前缀走native搜索快路（D9）；手册把定位写诚实；native执行内核的扩展路径已留（指令数组是纯数据）。本版不赌门槛（D9），若日后读数证明需要，接口不变换内核。
2. **解析器诊断分叉**：错误恢复路径与Go的诊断文案不会逐字一致。差分只对拒绝/接受与位置定性对拍，文案以nlang自己的可读性为准。
3. **ASCII折叠的用户惊讶**：非ASCII大小写不折叠（`(?i)`对中文无意义、对é/É不折叠）。手册把D4边界写成正面规则；差分语料钉住行为。日后补Unicode折叠与`\p{}`是纯增量。
4. **线程表分配抖动**：逐码点分配活跃线程表在长输入上开销明显。实施计划处理：visited槽位戳去重+线程表跨步复用，均为引擎内部细节，不影响接口与语义。
5. **开放问题**：`byte[]`输入的匹配面（对非文本字节流跑正则）本版不做——String入参覆盖文本场景，字节入参是纯增量扩展位。

## 八、集成点与文档同步

- `stdlib/regex.n`新增；标准库打包清单加入`regex.n`（npkg清单与IDE树可见性同步，IDE树门禁要求）。
- 手册：新增`library/25-regex`（中英成对；成员表+语义+围栏示例真实运行）；格式章25–27重编号为26–28并核对全部交叉引用；「语言规格」标准库清单与「已知限制」核对；索引钉与符号索引重算——全套照0.13.5插入流程。
- `examples/regex_tour.n`与示例清单、e2e清单登记。
- 手册`library/20-library-exceptions.md`的按包异常登记表加入`RegexException`，并同步「库声明N个自己的异常类」的计数句。
- `manual_sync_check.py`全量判定：`regex.n`属标准库面（用户可见），手册必须在本批次体现。
- `VERSION`→0.13.7、`CHANGELOG.md`条目；本spec不承诺语言面/预置包/ABI/退出码变更，手册同步判定以此为凭。

## 修订记录

| 日期 | 修订 |
|---|---|
| 2026-11-12 | 初稿：D1–D10裁定；语法面基线、API面、文本模型、线性上界验收与差分对拍策略定稿。初稿自审用本机Go探针（`temp/go_regprobe/`）实测钉死空匹配相邻与split/FindAll行为，修正D10凭记忆的示例；同步登记两处已知差分 |
| 2026-11-12 | 第三方审核（4 Medium+6 Low，全部经独立复核确认）落实：F1示例改双反斜杠与`$$`转义并新增D11字面量交互裁定；F2类内`&`定字面量（Go探针实证）、集合运算措辞修正；F3`RegexException`契约扩为「编译拒绝三类+调用方错误一类」；F4事实2改引char桥接（`s[i]`/`charAt(byteIdx)`）；F5模板数字规则改Go extract规则；F6去JS出处措辞；F7迭代器理由改流式动机；F8补20-library-exceptions登记项；F9补标志清除与生效范围语法；F10`\Z`论证改Go口径 |
| 2026-11-12 | 第三方审核第二轮（2 Medium+4 Low，全部经Go源码`regexp.go`/`doc.go`独立复核确认）落实：N1模板数字引用改`Expand`/`extract`原文规则（最长字母数字下划线名、纯数字且无前导零才按组号）；N2标志语法改`doc.go`原文三形态并删去实测不成立的「空`-`清空」；N3 D4删「输入先转`byte[]`」断言统一到事实2；N4排除总则补类内`&`/`~`字面量例外；N5去Go版本史断言改可核验措辞；N6示例行内注释改英文；第三轮零Medium定稿（余1 Low即时并入：N7补「不构成引用的`$`原样按字面处理」，本机Go实测`$!`/`end$`/`${1`钉死） |
