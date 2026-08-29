# NLang 入门

NLang 是一门受 EN 游戏引擎脚本语言启发的教学/研究型脚本语言，
自带编译器（ncc）、字节码虚拟机（nvm）与 IDE（nide）。

本页带你完成安装、跑通第一个程序，并用可运行的片段快速过一遍语言核心特性。
本文也是 nide 帮助菜单「NLang 入门」的落地页。

## NLang 是什么

源码（`.n`）先编译为字节码模块（`.nmod`），再由虚拟机执行。语言本身：

- **静态类型**——变量、参数、返回值都显式声明类型，没有类型推断；
- **面向对象**——`class`/`extends`/接口/虚分派，另有 struct（值类型）与 enum；
- **内建泛型集合**——`List<T>`/`Dict<K,V>`，带初始化器语法；
- **现代脚本语言标配**——异常处理、字符串插值、`out` 参数、默认参数、
  `math`/`io`/`fs` 标准库。

随包分发的四个工具：

| 工具 | 角色 |
|---|---|
| ncc | 命令行编译器：把 `.n`/`.nproj` 编译成 `.nmod`，也可编译后立即执行 |
| nvm | VM 执行器：运行 `.nmod` |
| ndisasm | 字节码反汇编器：查看 `.nmod` 里的指令 |
| nide | IDE：编辑、构建、运行，内嵌本帮助站 |

## 安装与布局

两种发行形态，内容一致：

- **NSIS 安装器**（`NLang-<版本>-win64.exe`）：默认安装到 `C:\Program Files\NLang`，
  并创建开始菜单快捷方式；
- **zip 便携包**（`NLang-<版本>-win64.zip`）：解压即用，目录结构与安装器相同。

安装根目录布局：

| 目录/文件 | 内容 |
|---|---|
| `bin\` | ncc.exe、nvm.exe、ndisasm.exe、nide.exe 与 Qt 运行时 |
| `examples\` | 全部示例程序，含多文件项目 `hello_project` |
| `docs\site\` | 本帮助文档站——nide 内嵌帮助窗口加载的正是它 |
| `LICENSE`、`README.md` | 许可证与项目说明 |

两点注意：

- `C:\Program Files` 对普通用户只读，而构建会把 `.nmod` 写在工程文件旁——
  在 nide 中打开示例前，先把 `examples\` 复制到可写目录。
- 可执行文件动态链接 MSVC 运行库，需要
  [VC++ Redistributable for Visual Studio 2015-2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  （装有 Visual Studio 2022 的机器通常已具备）。

## 五分钟上手：第一个程序

```nlang
import io;

int main() {
    io.print("hello, NLang");
    return 0;
}
```

`main` 是入口函数，返回值就是进程退出码；`io.print` 向标准输出写一行文字。

### 路径 A：在 nide 中

1. 启动 nide，菜单 文件 → 新建 → 文件...，输入上面的代码，保存为 `hello.n`。
   （也可以 文件 → 打开 → 项目...，选择 `examples/hello_project/hello_project.nproj`。）
2. 菜单 构建 → 构建项目；编译诊断显示在输出窗口的「编译输出」页。
3. 菜单 运行 → 开始运行（F5）；程序输出显示在「运行输出」页，应看到 `hello, NLang`。

不建工程也能用：通过 文件 → 打开 打开的独立 `.n` 文件同样可以构建、运行，
nide 把产物放在 `%TEMP%\nlang-nide\`，源码变化后运行会自动重新构建。

### 路径 B：命令行

在源码所在目录执行（安装包用户写 `bin\ncc`、`bin\nvm`）：

    ncc build hello.n -o hello.nmod    # 编译
    nvm hello.nmod                     # 运行，退出码 0

- `.nmod` 是编译后的字节码模块，由 nvm 直接执行——部署时不必携带源码。
- 进程退出码就是 `main` 的返回值：cmd 用 `echo %ERRORLEVEL%`、
  PowerShell 用 `$LASTEXITCODE` 查看。
- `ncc hello.n` 一步完成编译并立即执行；不带 `-o` 时 `.nmod` 写到
  当前工作目录（见下文「常见问题」）。

## 语言速览

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

详见 → [语言规格/类型](language-spec/types.md)、
[类型语义](language-spec/type-semantics.md)、[声明](language-spec/declarations.md)。

### 函数

```nlang
import io;

void divmod(int a, int b, out int q, out int r) {
    q = a / b;
    r = a - q * b;
}

int add(int a, int b = 10) {
    return a + b;
}

int main() {
    int q = 0;
    int r = 0;
    divmod(17, 5, out q, out r);    // q=3, r=2
    io.print("17 = 5*" + q + " + " + r);
    if (add(q) == 13 && r == 2)     // b 缺省为 10
        return 13;
    return 1;
}
```

输出 `17 = 5*3 + 2`，退出码 13。`out` 参数让一次调用带回多个结果；
参数可带默认值（还支持具名实参）；函数可重载，支持递归与 `void` 返回。

详见 → [语言规格/函数](language-spec/functions.md)。

### 控制流

```nlang
import io;

int classify(int n) {
    switch (n) {
        case 0:
            return 100;
        case 1, 2:              // 多值标签；case 体不穿透
            return 200;
        default:
            return 300;
    }
}

int main() {
    int total = 0;
    for (int i = 0; i < 3; i = i + 1)
        total = total + i;      // 0+1+2
    while (total < 10)
        total = total + 5;      // 10
    int[] evens = [2, 4, 6];
    foreach (int e in evens)
        total = total + e;      // 25
    io.print(classify(2));      // 200
    if (total == 25)
        return 25;
    return 1;
}
```

输出 `200`，退出码 25。`switch` 支持多值标签与 `int`/`float`/`string`/enum
四种判别族，case 体默认不穿透；`foreach` 可遍历数组、`List<T>` 与 `Dict` 的键。

详见 → [语言规格/语句](language-spec/statements.md)。

### 类与构造器

```nlang
import io;

class Animal {
    public string name;

    public int Animal(string name) {    // 构造器与类同名
        this.name = name;
        return 0;
    }

    public virtual int legs() { return 0; }

    public string toString() { return name; }
}

class Dog : Animal {
    public int Dog() {
        super("dog");               // 调用基类构造器
        return 0;
    }

    public int legs() { return 4; } // 覆写
}

int main() {
    Animal a = new Dog();           // 基类引用，虚分派
    io.print(a + " has " + a.legs() + " legs");
    if (a.legs() == 4)
        return 4;
    return 1;
}
```

输出 `dog has 4 legs`，退出码 4。类是引用类型；`toString()` 覆写后，
对象可直接参与字符串拼接。

详见 → [语言规格/声明](language-spec/declarations.md)。

### 集合

```nlang
import io;

int main() {
    List<int> nums = [10, 20, 30];  // List 初始化器
    Dict<string, int> pop =
        new Dict<string, int>{"cn": 14, "us": 3};
    nums.add(40);
    pop["jp"] = 1;                  // 键值写入
    io.print(nums.length() + pop.count());   // 4 + 3
    io.print(nums[0] + pop["cn"]);           // 下标读取
    if (pop.containsKey("us"))
        return 17;
    return 1;
}
```

输出 `7`、`24`，退出码 17。`List<T>` 可增长，`Dict<K,V>` 按键存取；
两者都支持下标语法糖与 `foreach` 遍历。

详见 → [语言规格/内建泛型类](language-spec/builtin-generic-classes.md)。

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

详见 → [语言规格/类型](language-spec/types.md)、
[标准库](language-spec/standard-library.md)。

### 模块

一个工程里的多个 `.n` 文件按相对路径组成模块：同目录文件天然互见，
其他目录（或外部 `.nmod`）需要显式 `import` 并以模块路径限定调用。

`main.n`：

```nlang modules/main.n
import io;
import utils.helper;

int main() {
    io.print(twice(21));            // 同目录 helper.n：裸调用，免 import
    io.print(utils.helper.answer());// 子目录：import 后用模块路径限定
    if (utils.helper.answer() == 42)
        return 42;
    return 1;
}
```

`helper.n`（与 main.n 同目录）：

```nlang modules/helper.n
int twice(int x) { return x * 2; }
```

`utils/helper.n`（模块路径 `utils.helper`）：

```nlang modules/utils/helper.n
int answer() { return 42; }
```

工程文件把三个源文件列进 `Sources` 后构建运行：

    ncc build -p modules.nproj -o modules.nmod
    nvm modules.nmod                 # 输出 42、42，退出码 42

内建命名空间（`io`/`math`/`fs`）同样要先 `import` 才能调用——本页每个
用到 `io.print` 的片段顶部的 `import io;` 就是它；漏写会得到编译错误
「Namespace 'io' is not imported」。

`import` 还支持递归通配：`import utils.*;` 一次性导入 `utils/` 及其全部
嵌套子目录（调用仍写全限定 `utils.helper.f()`）。重复导入、通配与精确
混合都是幂等的，效果取并集。

完整的可见性矩阵（谁需要 import、允许什么调用形式）与全部六类报错
文案，见语言规格的 Import Declaration 一节。

详见 → [语言规格/声明](language-spec/declarations.md)、
[标准库](language-spec/standard-library.md)。

### 异常

```nlang
import io;

int risky(int mode) {
    try {
        if (mode == 1)
            throw new Exception("manual");
        int[] a = new int[2];
        return a[9];                // 越界 → IndexOutOfBoundsException
    } catch (IndexOutOfBoundsException e) {
        return 2;
    } finally {
        io.print("finally always runs");
    }
}

int main() {
    if (risky(0) != 2)
        return 1;
    try {
        throw new Exception("boom");
    } catch (Exception e) {
        io.print(e.message);
        return 5;
    }
    return 1;
}
```

输出 `finally always runs`、`boom`，退出码 5。内建异常类有
`NullPointerException`、`DivByZeroException`、`IndexOutOfBoundsException`、
`AssertionException`，都继承自 `Exception`；用户类也可 `extends Exception`。

详见 → [语言规格/语句](language-spec/statements.md)。

## 三种运行方式

### 1. 在 nide 中（推荐）

1. 启动 nide（安装包内位于 `bin\` 下），菜单 文件 → 打开 → 项目...，
   选 `examples/hello_project/hello_project.nproj`。
2. 左侧「解决方案」树双击 main.n 打开编辑器。
3. 菜单 构建 → 构建项目，然后 运行 → 开始运行（F5）；
   输出窗口的「运行输出」页显示程序输出。

### 2. 命令行

在仓库根目录运行；安装包用户在安装根目录运行（ncc/nvm 位于 `bin\` 下）：

    ncc build examples/hello.n -o hello.nmod   # 编译
    nvm hello.nmod                             # 运行（退出码 42）

安装包内未设置 PATH 时写 `bin\ncc build ...`、`bin\nvm <name>.nmod`。

多文件项目用 `-p` 指定 .nproj，`-o` 指定输出位置：

    ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
    nvm hello_project.nmod

### 3. 逐例浏览

`examples/README.md` 列出全部示例与其演示主题、预期退出码。

## 常见问题

### `.nmod` 写到哪里了？

`ncc build hello.n` 不带 `-o` 时，模块文件写在**当前工作目录**，
不是源文件旁边——在别的目录里找不到产物时先想到这一点。要固定输出位置，
显式给 `-o 路径/名字.nmod`。nide 不受影响：独立 `.n` 文件的产物在
`%TEMP%\nlang-nide\`，工程产物写在工程目录。

### 退出码不是想要的值？

进程退出码就是 `main` 的返回值，Windows 保留 32 位原值；但 POSIX shell
（bash、Git-Bash、CI 的 bash 步骤）按惯例只保留低 8 位——`return 300`
在 Python/cmd/PowerShell 里看到 300，在 bash 里看到 44（对 256 取模）。
测试约定预期值 0–255，让所有观察者的视图一致。

详见 → [语言规格/退出码约定](language-spec/exit-code-convention.md)。

### 中文输出乱码？

程序输出是 UTF-8 字节。Windows 控制台默认代码页（如中文系统的 GBK）
会把它们显示成乱码；先执行 `chcp 65001` 切换到 UTF-8 代码页再运行程序。
注意 `fs` 与 `io` 的文件路径、文件名经由系统活动代码页转换，
非 ASCII 文件名不一定能按 UTF-8 往返。

详见 → [语言规格/标准库](language-spec/standard-library.md)。

### 帮助文档与搜索在哪？

nide 帮助菜单的「NLang 入门」「语言规格」「VM 架构」都在 IDE 内嵌的
帮助窗口中打开（内容即安装目录 `docs\site\` 下的文档站），
左侧是导航目录。搜索框在窗口左上角（站点标题旁），支持全文检索。

## 下一步阅读

建议按下面的顺序读语言规格：

1. [概览](language-spec/overview.md)——语言定位与整体结构
2. [命名约定](language-spec/naming-convention.md)——类型/方法/变量的命名规则
3. [类型](language-spec/types.md) → [类型语义](language-spec/type-semantics.md) →
   [声明](language-spec/declarations.md) →
   [内建泛型类](language-spec/builtin-generic-classes.md)
   ——类型系统、声明与 List/Dict
4. [表达式](language-spec/expressions.md) → [语句](language-spec/statements.md)
   ——运算符、控制流、异常
5. [函数](language-spec/functions.md) →
   [函数类型与委托](language-spec/function-types-and-delegates.md)
6. [标准库](language-spec/standard-library.md)、
   [内存管理](language-spec/memory-management.md)、
   [退出码约定](language-spec/exit-code-convention.md)、
   [已知限制](language-spec/known-limitations.md)

想了解执行引擎：[VM 架构/概览](vm-architecture/overview.md)，
再按需读编译管线、栈帧布局、字节码指令等章节。

想看能跑的完整程序：`examples/README.md` 按主题列出全部示例与预期退出码，
本页速览片段也大多能在其中找到对应示例。
