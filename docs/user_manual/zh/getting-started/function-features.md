# 速览: 函数与模块
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

详见 → [语言规格/函数](../language-spec/functions.md)。

### 函数值

```nlang
import io;

using Op = Func<int, int>;

int doubleIt(int x) { return x * 2; }
int apply(Op f, int x) { return f(x); }

int main() {
    int a = apply(doubleIt, 21);   // 42 —— 自由函数引用
    if (a == 42)
        return 42;
    return 1;
}
```

输出：无；退出码 42。函数可以当作值传递：`Func<Ret, Args...>` 命名一种
函数类型，自由函数或绑定方法（`c.foo`）都能绑到它上面。函数值可存进
变量、字段与集合（`List<Func<int, int>>`、`Func<int, int>[]`）。

详见 → [语言规格/函数类型与委托](../language-spec/function-types-and-delegates.md)。

### 原生函数

`calc.n`：

```nlang calc.n
native int natAdd(int a, int b);          // 无函数体：实现由宿主提供

int quad(int x) { return natAdd(x, x); }  // NLang 函数可以包装原生函数
```

输出：无——本片段只展示声明侧（编译通过即收，真正调用需要宿主侧实现）。
`native` 函数没有函数体，实现由宿主进程提供：自由函数以
`<包名>.<函数名>` 为键，命令行工具按包名在搜索路径上定位
`nlang_<包名>.dll`，嵌入宿主则按名注册实现。标准库的 `io.print` 就是
这样一个原生函数（`stdlib/io.n` 中声明为 `native void print(string)`，
实现随 `nlang_io.dll` 分发）。调用直接分派给实现，不经过 NLang 栈帧；
NLang 侧的包装函数（如 `quad`）则是普通函数。

详见 → [语言规格/函数](../language-spec/functions.md)。

### 模块

一个工程里的多个 `.n` 文件按相对路径组成模块：同目录文件天然互见，
其他目录（或外部 `.ncu`/`.npkg`）需要显式 `import` 并以模块路径限定
调用。

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

工程文件把三个源文件列进 `Sources` 后构建运行（产物是 `.npkg`
程序包）：

    ncc build -p modules.nproj -o modules.npkg
    nvm modules.npkg                # 输出 42、42，退出码 42

标准库包（`io`/`math`/`fs`）同样要先 `import` 才能调用——本页每个
用到 `io.print` 的片段顶部的 `import io;` 就是它；漏写会得到编译错误
「Package 'io' is not imported」。

`import` 还支持递归通配：`import utils.*;` 一次性导入 `utils/` 及其全部
嵌套子目录（调用仍写全限定 `utils.helper.f()`）。重复导入、通配与精确
混合都是幂等的，效果取并集。

完整的可见性矩阵（谁需要 import、允许什么调用形式）与全部六类报错
文案，见语言规格的 Import Declaration 一节。

详见 → [语言规格/声明](../language-spec/declarations.md)、
[标准库](../language-spec/standard-library.md)。

