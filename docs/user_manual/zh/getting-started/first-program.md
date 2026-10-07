# 五分钟上手

```nlang
import io;

int main() {
    io.print("hello, NLang");
    return 0;
}
```

`main`是入口函数，返回值就是进程退出码；`io.print`向标准输出写一行文字。

### 路径A：在nide中

1. 启动nide，菜单文件 → 新建 → 文件...，输入上面的代码，保存为`hello.n`。（也可以文件 → 打开 → 项目...，选择`examples/hello_project/hello_project.nproj`。）
2. 菜单构建 → 构建项目；编译诊断显示在输出窗口的「编译输出」页。
3. 菜单运行 → 开始运行（Ctrl+F5）；程序输出显示在「终端」页，应看到`hello, NLang`。

不建工程也能用：通过「文件 → 打开」打开的独立`.n`文件同样可以构建、运行，nide把产物放在`%TEMP%\nlang-nide\`，源码变化后运行会自动重新构建。

### 路径B：命令行

在源码所在目录执行（安装包用户使用`bin\ncc`、`bin\nvm`）：

    ncc build hello.n -o hello.ncu    # 编译nvm hello.ncu                     # 运行，退出码0

- `.ncu`是编译后的字节码单元映像，由nvm直接执行——部署时不必携带源码；它引用的库（如标准库）在运行期由工具链沿搜索路径自动定位。
- 进程退出码就是`main`的返回值：cmd用`echo %ERRORLEVEL%`、PowerShell用`$LASTEXITCODE`查看。
- `ncc hello.n`一步完成编译并立即执行；不带`-o`时`.ncu`写到当前工作目录（见下文「常见问题」）。

