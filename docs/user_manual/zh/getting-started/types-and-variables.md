# 速览：类型与变量

以下片段全部可直接编译运行（`ncc build` + `nvm`），可复制到nide或`.n`文件里试验；每节的正文给出实际输出与退出码，末尾给出「语言规格」对应章节的链接。

### 变量与类型

```nlang
import io;

int main() {
    int level = 3;              // 每个变量都声明类型，没有类型推断
    double scale = 1.5;         // 无后缀小数是 double；float 要写 1.5f
    long total = 5000000000;    // 64 位整型
    char grade = 'A';
    bool ok = level * 10 == 30; // 比较产生 bool
    string title = "demo";
    const int MAX = 100;        // const：初始化后再赋值是编译错误
    io.print(title + ": " + level * 10 + " / " + scale);
    io.print(grade + " " + ok + " " + total);
    if (ok)
        return 30;              // 退出码 30
    return 1;
}
```

输出`demo: 30 / 1.5`、`A true 5000000000`。标量基本类型共12个：整型家族（`byte` `ubyte` `short` `ushort` `int` `uint` `long` `ulong`）、`float`/`double`、`bool`与`char`（Unicode码点）；`string`是第13个基本类型（Unicode转换格式（UTF-8，Unicode Transformation Format）字节串，引用语义）。复合类型enum、struct、class在下面的章节与声明章节中有介绍。

详见 → [语言规格/类型](../language-spec/types.md)、[类型语义](../language-spec/type-semantics.md)、[声明](../language-spec/declarations.md)。

### 枚举

```nlang
import io;

enum Color { Red, Green, Blue }

int main() {
    Color c = Color.Blue;
    int n = c;                   // 枚举值就是整数
    io.print(c.toString());       // "Blue"
    io.print(n);                  // 2
    switch (c) {
        case Color.Red: return 1;
        case Color.Green: return 2;
        case Color.Blue: return 3;
    }
}
```

输出`Blue`、`2`，退出码3。枚举值是由编译器赋值的`int`常量（未显式指定时从0起自动递增）；值可直接转成`int`，`toString()`返回成员名，枚举也是合法的`switch`判别式。

详见 → [语言规格/声明](../language-spec/declarations.md)。

### 结构体

```nlang
import io;

struct Point {
    int x;
    int y;
}

int main() {
    Point p;                     // 零初始化
    p.x = 3;
    p.y = 4;
    Point q = p;                 // 深拷贝（值语义）
    q.x = 9;                     // 不影响 p
    io.print(p.x + p.y);          // 7
    if (q.x == 9 && p.x == 3)
        return 34;
    return 1;
}
```

输出`7`，退出码34。结构体是值类型：声明一个变量会把所有字段零初始化，拷贝它（`Point q = p;`）会按值复制整个结构体——包括嵌套结构体字段——因此两份拷贝相互独立。与class不同，结构体没有方法。

详见 → [语言规格/声明](../language-spec/declarations.md)。

### 类型别名

```nlang
import io;

using Ints = int[];

int main() {
    Ints xs = [10, 20, 30];
    io.print(xs[0] + xs.length());  // 10 + 3 = 13
    if (xs.length() == 3)
        return 13;
    return 1;
}
```

输出`13`，退出码13。`using Name = Type;`给一个类型起第二个名字；别名在每个使用点按文本展开，因此`Ints xs = ...`就是`int[] xs = ...`。别名可用于基本类型、数组、泛型与`Func<...>`类型形式。

详见 → [语言规格/声明](../language-spec/declarations.md)。

### 字符串

```nlang
import io;

int main() {
    string who = "NLang";
    int year = 2026;
    io.print("hello, ${who}");      // 插值
    io.print("year: " + year);      // 拼接时 int 自动转 string
    if ("${who} ${year}" == "NLang 2026")
        return 9;
    return 1;
}
```

输出`hello, NLang`、`year: 2026`，退出码9。字符串是UTF-8字节序列，`length()`/`substring()`/`indexOf()`都按字节计；按码点访问（`charAt`、`charCount`、`foreach (char c in s)`）见字符串页的「char桥接」。

详见 → [语言规格/类型](../language-spec/types.md)、[标准库](../language-spec/standard-library.md)。

