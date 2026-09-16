# 速览: 类型与变量

以下片段全部真实编译运行过（`ncc build` + `nvm`），可直接复制到 nide 或
`.n` 文件里试验；每节的正文给出实际输出与退出码，末尾给出「语言规格」对应章节的链接。

### 变量与类型

```nlang
import io;

int main() {
    int level = 3;              // 每个变量都声明类型，没有类型推断
    float scale = 1.5;
    string title = "demo";
    const int MAX = 100;        // const：初始化后再赋值是编译错误
    io.print(title + ": " + level * 10 + " / " + scale);
    if (level * 10 == 30)
        return 30;              // 退出码 30
    return 1;
}
```

输出 `demo: 30 / 1.5`。基本类型 `int`（32 位整型）、`float`（32 位浮点）、
`string`（UTF-8 字节串，引用语义）；复合类型 enum、struct、class 另见声明章节。

详见 → [语言规格/类型](../language-spec/types.md)、
[类型语义](../language-spec/type-semantics.md)、[声明](../language-spec/declarations.md)。

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

输出 `hello, NLang`、`year: 2026`，退出码 9。字符串是 UTF-8 字节序列，
`length()`/`substring()`/`indexOf()` 都按字节计。

详见 → [语言规格/类型](../language-spec/types.md)、
[标准库](../language-spec/standard-library.md)。

