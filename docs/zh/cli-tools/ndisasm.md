# ndisasm —— 反汇编

```text
ndisasm <module.nmod>
ndisasm -func <name> <module.nmod>
```

两种调用：全量转储，或 `-func <name>` 只保留一个函数节（其余节仍在）。
**`-func` 必须写在模块路径前面**——放在后面会被静默忽略，输出与全量
转储相同。输出节顺序固定：`module:` → `structs:` → `classes:` →
`string constants:` → 逐个函数节。

函数头一行给出全部元数据（`file=` 镜像编译时传入的源路径形态；内建
函数带 `intrinsic=N` 而无 `file=`；native 绑定函数带 `native`）：

```text
function main (frameSize=28, params=0, returnType=i32, file=examples/hello.n)
  bytecode:
    0000: debug 2
    0003: const_i32 42
    0008: assign 0
    ...
```

函数含 try 块时，指令清单之后还会转储 `try blocks:` 异常处理表
（每行 `[起止 pc) handler=... class=... catchLocal=...`）。

全量转储会列出全部内建类方法（均 `(no bytecode)`，一个 hello 也有
五十七个），所以日常看单个函数用 `-func` 过滤。典型用途：对照 ndb
的 `x` 命令核对指令地址、检查优化结果、排查序列化问题。
