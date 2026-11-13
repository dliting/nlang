> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 25. regex包

`regex`包用正则表达式检索、切分与替换文本。引擎做自动机模拟而非回溯，
所以任何模式的最坏运行时间都对输入长度线性——来自不可信来源的模式
无法把程序拖垮。这份保证的代价是语法边界：反向引用与环视断言是已知
实现都需要回溯的构造，因此不受支持，而引擎在编译期明说这一点，而不是
把它们悄悄读成普通文本（§25.4）。

干活的是两个类。`Regex`编译一次模式，此后就是不可变值——可对任意多个
输入复用，也可在并发上下文之间共享。`Match`是一次匹配的只读结果。
匹配是子串检索，姿态同`indexOf`，不是整串比较：未锚定的模式在输入内
任意位置找最早的出现处，整串匹配要靠给模式加上`\A...\z`（§25.5）。
本包纯`nlang`，无`native`代码。

## 25.1 巡礼

```nlang
// A tour of the regex package: named capture groups, repeated
// matches, and replace templates.
import io;
import regex;

int main() {
    Regex re = Regex("(?P<key>\\w+)=(?P<value>[^\\s]*)");
    Match m = re.match("server=127.0.0.1 port=8080");
    if (m != null) {
        io.print("key=" + m.group("key") + " value=" + m.group("value"));
    }
    List<Match> all = re.matches("server=127.0.0.1 port=8080");
    io.print("count=" + all.length().toString());
    for (int i = 0; i < all.length(); i = i + 1) {
        Match pair = all[i];
        io.print("at " + pair.start.toString() + ": " + pair.text);
    }
    // $$ is the dollar escape: the template layer sees ${value}; and
    // expands the value group for every match.
    io.print(re.replaceAll("a=1 b=2", "$${value};"));
    io.print("ok");
    return 0;
}
```

这段围栏代码就是随发布提供的`examples/regex_tour.n`，它打印：

```text
key=server value=127.0.0.1
count=2
at 0: server=127.0.0.1
at 17: port=8080
1; 2;
ok
```

第一行演示从匹配里按名字读回捕获组；`at`各行说明偏移是输入的字节
偏移；`1; 2;`一行演示模板对两处匹配的展开——两者之间的空格不属于
任何匹配，原样穿过。源码里为何写`\\w`和`$${value};`，而模式和模板
实际说的是`\w`和`${value};`，见§25.7。

## 25.2 Regex类

| 成员 | 含义 |
| --- | --- |
| `Regex(pattern)` | 编译模式。失败抛`RegexException`；消息说明原因，大多还给出模式内的字节偏移（§25.4）。 |
| `pattern()` | 构造时传入的模式文本。 |
| `isMatch(input)` | 输入中是否存在匹配；不需要匹配内容时这是最省的测试。 |
| `match(input)` | 最左匹配，返回`Match`；没有匹配则为`null`。无匹配是正常结果而非错误（§4.3）。 |
| `matches(input, limit = -1)` | 自左向右的全部不重叠匹配，返回`List<Match>`。`limit < 0`（默认）不限数；`limit >= 0`封顶条数。空匹配遵循§25.9的跳过规则。 |
| `split(input, limit = -1)` | 匹配之间的片段（§25.9）。 |
| `replaceAll(input, template)` | 用模板替换每一处匹配（§25.8）。 |
| `replaceFirst(input, template)` | 只替换最左一处匹配。 |

`Regex`是`const class`：构造器编译，此后没有任何东西改动对象，所以
一个`Regex`可同时服务任意多个并发运行的上下文（每次匹配的工作状态
各自独立）。`RegexException`派生自`IOException`——`UrlException`与
`HttpException`也是如此——因为模式常来自配置文件、捕获文本来自
I/O，一个`catch (IOException e)`能把坏模式与读失败一起兜住。自由函数
`regex.quoteMeta(s)`转义参数里的每个元字符，使结果按字面匹配这段
文本——`quoteMeta("a.b*c")`得到`a\.b\*c`——这是把不可信文本拼进模式
的工具。

## 25.3 Match类

`Match`是描述一次成功匹配的只读值。

| 成员 | 含义 |
| --- | --- |
| `text` | 匹配到的文本。 |
| `start` / `end` | 匹配在输入中的字节偏移；`end`是开区间端点，与`substring`同一约定（§4.3）。 |
| `groupCount` | 捕获组个数，含0号组（整个匹配）。 |
| `group(index)` | 该捕获组的文本。未参与本次匹配的组给`""`；下标越界抛`RegexException`。 |
| `group(name)` | 按组名取同样的文本；组名不存在抛`RegexException`——组名拼错是bug，这里沉默反而会埋掉它。 |
| `groupStart(index)` / `groupEnd(index)` | 组的字节偏移；组未参与时为`-1`。 |

0号组就是整个匹配：`group(0)`等于`text`，`groupStart(0)`/
`groupEnd(0)`等于`start`/`end`。

## 25.4 模式语法

| 构造 | 形式 |
| --- | --- |
| 字面文本 | `abc`——任意语言的文本；字面量按完整码点匹配（§25.6）。 |
| 任意字符 | `.`——一个码点；除非开启`(?s)`，否则不含`\n`。 |
| 选择与分组 | `a|b`，`(re)`捕获，`(?:re)`只分组不捕获，`(?P<name>re)`与`(?<name>re)`为捕获组命名；两个组同名是编译错误。 |
| 重复 | `*` `+` `?` `{n}` `{n,}` `{n,m}`——默认贪婪；尾随`?`则优先少重复。重复次数超过1000被拒绝。 |
| 标志 | `(?i)`大小写不敏感，`(?m)`多行锚点，`(?s)`点号也匹配换行。`(?flags)`从所处位置起到所在组末尾生效，`(?flags:re)`只作用于`re`，`-`清除标志——`(?i-s:re)`即置`i`清`s`。 |
| 锚点 | `^ $ \A \z \b \B`（§25.5）。 |
| 转义 | `\a \f \t \n \r \v`与转义标点；`\123`八进制——共取一到三个八进制数字：首位是`0`时单独即成八进制（`\0`），首位非零时须紧跟下一个八进制数字才算八进制（`\41`、`\101`；光一个`\1`–`\7`是反向引用形式，见下被拒）；`\xHH`、`\x{10FFFF}`，以及`\Q...\E`把一段模式文本按字面引号起来。 |
| 字符类 | `[abc]`，求反`[^abc]`，范围`[a-z]`，POSIX类如`[[:alpha:]]`，以及`\d \D \s \S \w \W`——全部限于美国信息交换标准代码（ASCII）范围（§25.6）。类内转义有效且`\b`表示退格字符U+0008，这是Perl系引擎共同的读法；`&`与`~`在类内是普通成员。 |

引擎用自己的`RegexException`拒绝一切会破坏线性时间保证、或主流
引擎语义本就不一致的构造：反向引用（`\1`）、环视（`(?=) (?!`
`(?<=) (?<!)`）、Unicode属性类（`\p{...}`）、`\Z`、全局贪婪翻转
`(?U)`、扩展排版`(?x)`、独占量词（`*+`、`++`）、原子组（`(?>re)`），
以及类内集合运算——`[a&&b]`能编译，但只是普通成员集合`a`、`&`与`b`。
每次拒绝都说明原因，且大多附模式内字节偏移，例如
`regex: back references are not supported at byte 1 of pattern`。
另有三道规模护栏——模式嵌套深度、编译后程序大小、重复次数——防止
恶意模式耗尽内存，各有自己的消息。

## 25.5 锚点与行边界

- `^`——输入开头；`(?m)`时也是每行开头。
- `$`——输入末尾，或末尾`\n`之前的位置；`(?m)`时也是每行末尾。
  `Regex("abc$").isMatch("abc\n")`为真。
- `\A`与`\z`——恰好是输入的开头与末尾，不受`(?m)`影响。`$`对结尾
  换行的让礼不合用武之地时，`\z`是严格的末尾；`\A...\z`配对即锁定
  整串。
- `\b`与`\B`——词边界与非边界，按ASCII词集计算（§25.6）。

## 25.6 码点与ASCII范围

匹配把输入读作Unicode Transformation Format（UTF-8）码点序列：
`.`匹配任意文本的一个码点，字面量按完整码点匹配，绝不成字节碎片。
构不成合法UTF-8序列的字节各自算一个单字节伪码点——匹配既不在非法
输入上失败，也不改写它。报告的偏移是字节偏移，与`substring`、
`indexOf`同一基准（§4.3）。

相对地，字母、数字与空白指的是ASCII：类简写`\d \s \w`及其否定、
POSIX类、
`(?i)`大小写折叠与`\b`词集都只覆盖ASCII。这条规则的两半都是正面
规则，不是脚注：

```nlang
Regex("a.b").isMatch("a中b")       // true  — 一个码点，任意文字
Regex("\\w+").isMatch("汉字")      // false — ASCII词集不含汉字
Regex("(?i)cafe").isMatch("café")  // false — 折叠只认ASCII字母
Regex("(?i)cafe").isMatch("cafe")  // true
```

## 25.7 模式与模板写在NLang源码里

模式文本与模板都是普通的字符串字面量，途中有两个字符要与语言自身的
字面量规则相遇：

- 模式里的反斜杠必须写成双份，因为字符串字面量先吃掉反斜杠：模式
  `\d+`要写成`"\\d+"`。NLang没有原始字符串形式，源码里光一个`\d`
  是未知的字符串转义——编译错误。
- 模板里的`$`要与`${name}`字符串插值相遇：写`$${name}`，字面量层
  把`${name}`交付给模板引擎。要在替换文本里放一个字面`$`，写
  `"$$$"`——字面量层把它折成模板的`$$`，模板再发出`$`。

```nlang
import io;
import regex;

int main() {
    Regex g = Regex("(\\w)(\\d)");
    io.print(g.replaceAll("x7", "$1x $${2}y") + "\n");  // " 7y" — 见§25.8
    io.print(g.replaceFirst("x7", "$${1}x") + "\n");    // "xx"
    io.print(Regex("a+").replaceFirst("a-aa-a", "$$$") + "\n");  // "$-aa-a"
    return 0;
}
```

## 25.8 替换模板

在传给`replaceAll`/`replaceFirst`的模板里：

- `$$`产出一个字面`$`。
- `$name`与`${name}`展开一个捕获组。`$`之后紧跟的字母、数字与下划线
  串被贪婪地读作一个名字：`$1x`指的是名为`1x`的组，不是组`1`后面
  跟个`x`；所以要接数字再缀文本得写`${1}x`。
- 全是数字且无前导零的名字是组号；0号组是整个匹配。`$10`指10号组。
  未被引用的文本原样穿过：模式`a+`配模板`[$0]`把`a-aa-a`变成
  `[a]-[aa]-[a]`。
- 不存在的组，或本次匹配未参与的组，展开为空串。
- 构不成引用的`$`——后面既非名字字符也非第二个`$`——留作字面`$`：
  `$!`仍是`$!`；`${`始终没有闭合时同样不构成引用，其后整段原样保留，
  所以`${1`仍是`${1`。

## 25.9 split与空匹配

`split(input, limit = -1)`在每处匹配处切开输入：`limit < 0`（默认）
保留全部片段，`limit == 0`返回空表，`limit > 0`至多返回那么多个
片段，最后一个装余下的全部。捕获组绝不会变成片段——答案是匹配之间
的文本，这正是`limit`计数合理的原因。

```nlang
Regex(",").split("a,b,,c", -1)    // ["a", "b", "", "c"]
Regex(",").split("a,b,,c", 2)     // ["a", "b,,c"]
Regex(",").split("a,b,,c", 0)     // []
Regex("a*").split("ab", -1)       // ["", "b"]
```

能匹配空的模式需要一条相邻规则，`split`与`matches`共用：起点恰在
上一匹配结束处的空匹配被跳过——没有这条跳过，`a*`会在它刚吞掉的`a`
后面的空位上永远空转——而其他地方出现的空匹配，包括输入末尾处的，
仍然算数。所以`Regex("a*").matches("ab")`返回两个匹配，0处的`a`和
2处的空匹配。

`split`照这些匹配切分输入，另加两条片段规则：文本位置0处的零宽匹配
不贡献开头空片段，末尾余段仅当最后一个分隔符的起点不在文本末尾时才
追加。所以上面的`split`示例给出`["", "b"]`——带宽度的`a`匹配留下
开头空片段，而2处的空匹配起于文本末尾，不留末尾片段——
`Regex("x*").split("abc")`则给出`["a", "b", "c"]`：每个空匹配贡献
它之前的文本，两端都不贡献空边片段。
