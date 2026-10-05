# ndisasm —— 反汇编

ndisasm把`.ncu`反汇编为可读的字节码转储，回答「编译器实际生成了什么」。用它核对ndb `x`命令中的指令地址、检查加载期链接前的导入槽、排查序列化或模块加载问题。

```text
ndisasm <module.ncu | package.npkg>
ndisasm -func <name> <module.ncu | package.npkg>
```

两种调用：全量转储，或`-func <name>`只保留一个函数节（其余节仍在）。**`-func`必须写在模块路径前面**——放在后面会被静默忽略，输出与全量转储相同。`-func <name>`按函数表键逐字匹配：用户函数的键带模块或包路径限定（如`hello.main`），裸名匹配不到——此时不报错，只是不打印函数节。输入也可以是`.npkg`包归档：先打印一行`package:`，随后按成员表顺序逐个转储成员映像（每个成员自带一套完整小节）；某个成员损坏时报错但不阻断其余成员的转储，退出码反映失败。单个映像的输出节顺序固定：`module:` → `structs:` → `classes:` → `string constants:` →逐个函数节。

函数头一行给出全部元数据（`file=`镜像编译时传入的源路径形态；内建函数带`intrinsic=N`而无`file=`；native绑定函数带`native`）：

```text
function hello.main (frameSize=28, params=0, returnType=i32, file=examples/hello.n)
  bytecode:
    0000: debug 2
    0003: const_i32 42
    0008: assign 0
    ...
```

函数含try块时，指令清单之后还会转储`try blocks:`异常处理表（每行`[起止 pc) handler=... class=... catchLocal=...`）。

全量转储会列出全部内建类方法（均`(no bytecode)`，一个hello就有数十个），所以日常看单个函数用`-func`过滤。
