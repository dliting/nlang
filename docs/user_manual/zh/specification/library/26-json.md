> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 26. json包

`json`包读写JSON文档（RFC 8259）。解析把文本变成一棵值树，程序经由`JsonValue`基类及其`kind`判别器检视它；`JsonObject`与`JsonArray`构造器让程序直接装配文档，`serialize`把任何值渲染回文本。这个包用NLang本身写成——`stdlib/json.n`承载完整可读的解析器与序列化器源码——并声明一个自己的异常类`JsonException`（§8.3、第20章）。

## 26.1 导览

导览即随附示例`examples/json_tour.n`；程序之后跟着它的输出。

```nlang
// The json package tour: parse, read through kind(), edit the DOM,
// serialize compact and indented, and catch a syntax error. The manual's
// json chapter reproduces this file verbatim as its tour; keep them in
// step when either changes.
import io;
import json;

int main() {
    string text = "{\"books\": [{\"title\": \"The Art\", \"year\": 1999, \"available\": true}, {\"title\": \"Compilers\", \"year\": 2006, \"available\": false}], \"count\": 2}";
    JsonValue doc = json.parse(text);
    JsonObject root = doc.getObject();
    JsonArray books = root.get("books").getArray();
    int i = 0;
    while (i < books.length()) {
        JsonObject book = books.get(i).getObject();
        io.print(book.get("title").getString() + " (" + book.get("year").getInt() + ")");
        i = i + 1;
    }
    // DOM edits keep insertion order: the second book gains a rating and
    // drops its availability flag.
    JsonObject second = books.get(1).getObject();
    second.put("rating", 4);
    second.remove("available");
    io.print(root.serialize());
    io.print(root.serialize(2));
    // A syntax error arrives as a catchable JsonException carrying the
    // byte offset where the document went wrong.
    try {
        json.parse("{\"year\": 199");
    } catch (JsonException e) {
        io.print("rejected: " + e.message);
    }
    io.print("ok");
    return 0;
}
```

```text
The Art (1999)
Compilers (2006)
{"books":[{"title":"The Art","year":1999,"available":true},{"title":"Compilers","year":2006,"rating":4}],"count":2}
{
  "books": [
    {
      "title": "The Art",
      "year": 1999,
      "available": true
    },
    {
      "title": "Compilers",
      "year": 2006,
      "rating": 4
    }
  ],
  "count": 2
}
rejected: json: unexpected end of input at byte offset 12
ok
```

## 26.2 文档与类别

解析出的文档是一棵`JsonValue`节点树，每个JSON值对应一个节点。每个节点都回答`kind`，而`kind`说明该用哪个访问器读取它：

| `kind()` | 值装着 | 用它读 |
| --- | --- | --- |
| `JsonKind.Null` | JSON的`null` | — |
| `JsonKind.Bool` | `true`或`false` | `getBool` |
| `JsonKind.String` | 文本 | `getString` |
| `JsonKind.Number` | 整数或小数 | `getLong`、`getInt`、`getDouble`（§26.4） |
| `JsonKind.Array` | 值的有序列表 | `getArray` |
| `JsonKind.Object` | 键值对 | `getObject` |

先检查`kind`、再调用相匹配的访问器，是消费文档的正当姿势——文档的形状就是数据，遍历它的程序在读取前先问每个节点自己是什么。对错误的类别调用访问器抛出`JsonException`，消息为`json: expected <kind>, got <kind>`。

| `JsonValue`的成员 | 含义 |
| --- | --- |
| `kind()` | 值是六种类别中的哪一种 |
| `getString()`、`getBool()`、`getLong()`、`getInt()`、`getDouble()` | 把值当作该类型 |
| `getObject()`、`getArray()` | 把值当作`JsonObject`或`JsonArray`（§26.5） |
| `serialize()` | 该值的紧凑JSON文本（§26.7） |
| `serialize(spaces)` | `spaces > 0`时缩进，否则紧凑（§26.7） |
| `toString()` | 与`serialize()`完全相同的文本（§26.7） |

两个`JsonValue`变量`==`成立仅当它们指同一个对象；`==`从不比较文档结构（§26.6）。

## 26.3 解析

```nlang
JsonValue doc = json.parse(text);
```

`parse`恰好接受RFC 8259的文法，不多不少：

- 字面量是小写的`true`、`false`与`null`；`True`、`NULL`、`NaN`、`Infinity`都是错误。
- 数就是JSON的数：可选的`-`、数字、可选的小数部分、可选的指数。没有前导`+`、没有十六进制、没有下划线分隔、没有尾逗号、没有注释，也没有单引号字符串。
- 任何JSON值都可以独立作为顶层值，包括裸数、字符串或`null`；顶层值之后还有文本就是错误。
- 字符串由`0x20`~`0x7E`的美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）可打印字符原样写出（DEL字符`0x7F`也原样直通）、或写成转义对`\"` `\\` `\/` `\b` `\f` `\n` `\r` `\t`之一、或`\uXXXX`转义；`0x80`及以上的原始字节直通（已是Unicode转换格式（UTF-8，Unicode Transformation Format）；包不要求也不禁止任何特定的非ASCII序列）。字符串内`0x20`以下的控制字节必须转义。
- 携带代理对的两个`\u`转义解码为一个辅助平面字符；落单的代理半区解码为替换字符U+FFFD，文本其余部分保持原义。
- 同一对象内的重复键被接受：后写的值生效，键在`names`里保持首次出现的位置（§26.6）。

每次解析失败都抛`JsonException`，消息形如`json: <什么> at byte offset <n>`；偏移是源文本中出错字节的零基索引，重读文件即可定位：

```
json: unexpected end of input at byte offset 12
json: unexpected data after top-level value at byte offset 4
json: nesting deeper than 200 at byte offset 200
```

## 26.4 数

JSON的数只有一种写法；NLang用它仅有的两种类型来读，访问器决定一个数怎么回答：

- 不含小数部分也不含指数的词元按64位带符号整数（`long`）保存；其余一律按`double`保存。
- 数是范围`-9223372036854775808 … 9223372036854775807`内的精确整值时`getLong`作答。小数形式的词元只要值是整数也作答——`3e2`读回`300`。装着同一整值的`double`同样作答。超出范围、或带小数部分时，`getLong`抛`JsonException`，消息为`json: number out of long range`。
- `getInt`在32位`int`范围内问同样的问题，范围外抛`json: number out of int range`。
- `getDouble`对每个数都作答。大到`long`装不下的整数词元以最近的`double`返回——`2^53`以上的`double`无法命名每个整数，所以30位数字词元在文本里保持精确，经`getDouble`则舍入。
- 从文本解析出的数保留写下的每个字符：`1e2`、`0.10`、`-0`与30位整数都按原拼写序列化。程序用`long`或`double`构造的数按能读回同值的最短形式打印（`0.1 + 0.2`打印`0.30000000000000004`）。
- 指数把数推出`double`范围时，在发现处就被拒：`1e999`抛`JsonException`，消息为`json: number out of range at byte offset <n>`。小到无法命名的数读作`0.0`。两种拼写文法都接受——RFC 8259允许它们——所以这条检查是解析器自己的范围规则，不是文法规则。

## 26.5 构建文档

```nlang
JsonObject root = JsonObject();
JsonArray ports = JsonArray();
ports.add(8080);
ports.add(8443);
root.put("name", "nlang");
root.put("ports", ports);
root.put("debug", false);
io.print(root.serialize());
// {"name":"nlang","ports":[8080,8443],"debug":false}
```

| 构造器 | 成员 |
| --- | --- |
| `JsonArray()` | `add(v)`——`v`是`JsonValue`、`string`、`int`、`long`、`double`或`bool`；`get(i)`、`set(i, v)`、`removeAt(i)`、`length()` |
| `JsonObject()` | `put(name, v)`——`v`同样六种；`get(name)`、`has(name)`、`remove(name)`、`names()`、`count()` |
| `JsonString(s)`、`JsonBool(b)`、`JsonNumber(v)`、`JsonNull()` | 单个叶值（`JsonNumber`接受`long`或`double`） |

对象保持插入顺序：`names`按键的值到达的顺序列出键，这样构建的文档也按被给予的顺序序列化。没有任何东西排序键；输出顺序重要时，就按那个顺序添加键。

## 26.6 编辑

容器随时可编辑，解析前后都一样。`set(i, v)`与`removeAt(i)`和NLang所有容器一样从`0`起索引，越界的索引按`List`的方式失败（§12）。对象一侧，`get(name)`对文档没有的键抛`JsonException`，消息为`json: object has no key "<name>"`；拿不准的程序先问`has(name)`：

```nlang
if (root.has("tls")) {
    int port = root.get("tls").getInt();
}
```

装着`null`的键与缺席的键是两种不同的状态：`has`对前者答`true`、对后者答`false`，前者的`kind`是`JsonKind.Null`。`remove(name)`回答键是否在；`removeAt(i)`把其余元素前移。

同一文档的两份分别解析的副本不`==`——`==`指同一个对象，从不指同一文本或同一形状（§26.2）。要比较结构，就比较序列化结果。

这棵树不携带锁，也没有只读视图：一个context建树、改树，完成后把整棵树移交给另一个context，自己不再触碰——这正是NLang对一切跨线程移交给的可变值所要求的移交所有权纪律。两个context同时改一棵树就是普通数据竞争；把它们隔开是程序自己的事。

## 26.7 序列化

`serialize()`渲染紧凑形式：除字符串自身携带的之外没有任何空白。`spaces > 0`的`serialize(spaces)`改为缩进——每个条目独占一行、每层深`spaces`个字符、键与值之间用`": "`分隔——而`0`或更小的`spaces`渲染紧凑，与`serialize()`相同。空数组与空对象即使在缩进模式下也渲染`[]`与`{}`，任何形式都不追加行尾换行。`toString()`渲染与`serialize()`完全相同的文本，所以值可以直接落进文本：

```nlang
string line = "config: " + doc;   // 值序列化进文本
```

输出的转义是最小集：`"`与`\`加反斜杠；`0x20`以下的字节有`\b` `\f` `\n` `\r` `\t`就用、否则用`\u00xx`（十六进制小写）；其余字节原样直通，非ASCII文本保持字面UTF-8。

两种失败终结序列化，都抛`JsonException`：嵌套超过200个容器（`json: nesting deeper than 200`），以及数不是有限值（`json: cannot serialize non-finite double`——`nan`、`inf`与`-inf`没有JSON写法）。解析与序列化在两个方向上都稳定：`serialize`产出的文本重新解析成文档，再序列化成同样的字节；规范的紧凑文本重新序列化保持不变。

## 26.8 局限

- `parse`对完整保持在字符串里的文档工作；没有对文件或套接字的流式解析。先读入文本（§14、§16）。
- 嵌套上限在解析与序列化两个方向上都是200个容器。比这更深的文档罕见；上限让两边的递归都有界。
- 非`long`的数（`getDouble`）携带`double`精度：`2^53`以上的整数会舍入。全精度重要时把这样的数保持为字符串；解析出的数保留精确的源文本（§26.4）。
- 对象只按插入顺序排键；没有排序输出模式（§26.5）。
