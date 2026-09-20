# nvm —— 运行模块

```text
nvm <module.nmod> [--gc-stress=N]
```

运行一个编译好的模块，进程退出码 = `main` 返回值（约定详见
[退出码约定](../language-spec/exit-code-convention.md)）。模块打不开
时报 `Runtime error: Failed to open module file: <路径>`。`--gc-stress=N`
是测试旋钮：把两套 GC 阈值钳到极小值，任何漏追踪的引用会在几次分配
内变悬垂——用于验证内存管理变更，日常使用不需要。
