# 库机制：搜索路径、源码内联与native模块

本页说明NLang库系统的整体设计：一个库如何被发现、解析、编译和执行，以及机制为何是这个形状。这是面向运行时维护者的设计文档；库作者视角的实用指南见[开发第三方库](../libraries/developing-libraries.md)，用户使用面见[标准库](../language-spec/standard-library.md)。

## 1. 指导原则

**标准库与第三方库是同一套机制。**标准库就是搜索路径上的
`stdlib/*.n`，和其他库没有任何区别；编译器与VM不区分二者。这对应
Python（标准库就是`sys.path`上的普通源码）与Java/C#（托管源码与Java本地接口（JNI）、P-Invoke这类native实现共存于同一个包）。编译器和VM里没有任何
硬编码的标准库签名：声明文件与native动态链接库（DLL，dynamic-link library）就是全部实现。

## 2. 库的形态

一个库就是搜索路径上的一个目录：

```
mylib/
  mylib.n            # 库本体：声明、NLang 实现，或两者混合
  nlang_mylib.dll    # 可选：`native` 声明的 native 实现
```

库的接口面就是`<pkg>.n`文件本身——包名由文件相对匹配搜索根的路径
派生（`vendor/graphics.n`在根`R`下即包`vendor.graphics`），文件内
不再有任何包装语法。函数分两类，可以自由混合（**混合库**）：

- **普通NLang函数**——有函数体；编译进自己所在包的单元映像
  （如`stdlib.npkg`的成员），运行期与消费方链接后按字节码执行；
- **`native`函数**——只有签名与文档注释（无函数体）；运行时经宿主
  应用二进制接口（ABI，application binary interface）分派到`nlang_<包名>.dll`（第5节；DLL按包名第一个点之前的段
  命名，因此多段包里的`native`声明是编译期诊断）。

标准库同形：`stdlib/io.n`、`math.n`、`fs.n`是手写的权威声明文件
（io/math/fs三个包目前为纯native），实现由`src/native/`构建
为`nlang_math.dll`、`nlang_io.dll`、`nlang_fs.dll`。

## 3. 编译模型：源码完整内联

单段`import`在搜索路径上找到的库`.n`会被**完整解析**（含函数体），
成为*库翻译单元*，与项目翻译单元合并进同一个抽象语法树（AST，abstract syntax tree）根
（`src/compiler/ModuleBuilderImports.cpp`、
`src/compiler/builder/ModuleRegistry*`）。构建顺序：

1. 解析项目源（收集import）；
2. **库发现迭代到不动点**：worklist以项目的import起步；带点名
   `a.b.c`在第一个持有它的搜索根下定位`<根>/a/b/c.n`（多个根提供
   同一个包是重复包错误，指名两条路径），每个定位到的文件依次
   (a)做签名索引（`langservice::SymbolIndex`，按匹配根的包名）、
   (b)完整解析为库编译单元（TU，translation unit），其自身的import再加入worklist——第三方
   库因此可以依赖其他库。命中项目已有源文件的包不重复内联；
3. 注册全部TU，库TU带`isLibrary`标志，包名取自匹配的搜索根；
4. 展开别名、构建import门、加载外部模块（`.ncu`/`.npkg`）；
5. 解析、**逐单元**生成代码——库单元不参与产码：消费方映像只携带
   导入槽，库代码由库自身的包在运行期提供。

统一模型带来以下结论：

- **命名空间限定调用编译为普通调用**：`mylib.f(...)`发射
  `OP_CallFunc` + 全限定名`ns.f`；运行时按被调函数自身性质分派——
  字节码体，或`isNative` → DLL。签名表调用路径与按库区分的codegen
  分支已不存在。
- **函数表中命名空间限定的顶层函数使用全限定名**（含库native函数），
  使运行时`m_natives["ns.name"]`查表与DLL导出的注册名一致。
- **库可以定义类型**：命名空间合并进根，库内class/struct/enum/
  interface在消费方与项目类型同等解析（含继承、虚分派、enum方法）。
  类型位置写作`ns.Type`，解析器在被内联的库单元内查找声明并绑定
  （`ModuleRegistry::FindModuleType`）；未导入的命名空间会被诊断，
  而不是静默绑定。外部`.ncu`不暴露源码级类型，因此`ns.Type`
  只对内联库源生效。
- **去重**按"已完整解析的库文件"集合跟踪，与符号索引的"已索引"集合
  分离：标准库包在构建环境构造时就已签名索引，但仍必须内联，
  两者不能混用同一个判据。

### 库TU与项目TU的差别（可见性隔离）

库TU编译进构建，但刻意**不是**项目模块：

- 不加入同目录隐式可见池（该规则只适用于项目内文件），也不把同目录其他文件注入自身视野；
- 通配import不会把库TU当项目模块卷入；
- 库永远看不到消费方项目的模块，只看得见自己import的内容；
- 同一次构建内两个单元解析出同一个点分包名是重复包错误，诊断指名
  两条来源路径（保留名表已被此规则取代——名为`io`的项目目录现在
  就是普通目录）。

库之间的循环import允许：所有TU先合并、后统一解析，声明在一次编译
内彼此可见，与项目多源文件的道理相同。

### 运行期：闭包装载与链接

产码不含库代码，所以执行的第一步是把引用闭包装齐、链接成一个
运行期模块（`src/vm/NcuLoader.cpp`、`src/vm/NcuLinker.cpp`）：

1. **装载**（nloader）——从入口产物出发：`.npkg`的成员先在包内
   解析；导入槽的目标模块沿搜索路径找`<模块路径>.ncu`文件或包含
   该成员的`.npkg`，递归到闭包完整。成员映像头部的模块路径必须与
   目标一致（名实不符即拒绝），每步做版本与校验和校验；缺包诊断
   一次报清，列出已搜索目录。
2. **链接**（nlink）——纯内存变换：N幅单元映像按限定名合并去重，
   占位槽解析成全局表下标，全部操作数统一重映射；未解析符号与
   可见性违规一次报清。入口按程序包的入口记录或裸单元的
   `<模块路径>.main`约定解析。

`nvm`、`ncc run`、ncc的编译后立即执行与ndb会话全部走这同一条
路径——工具之间没有第二条装载/链接实现。标准库在运行期就是搜索
路径上的`stdlib.npkg`，与其他库包无异。

## 4. 搜索路径

编译期发现`.n`、运行期装载闭包成员（`.ncu`/`.npkg`）与加载native
DLL使用**同一个有序目录列表**——前者优先，重复目录规范化去重
（Windows上折叠大小写）。五层，从高到
低（`include/nlang/common/LibrarySearchPath.h`）：

1. 命令行`-I <dir>`（可重复）；
2. `.nproj`的`<ImportPaths>`；
3. 项目 / 源文件 / 模块所在目录；
4. 环境变量`NLANG_PATH`（Windows `;`、POSIX `:`）；
5. 系统缺省：标准库目录、可执行文件目录、当前目录。

`ncc`、`nvm`、`ndb`共用这一个header（纯STL、不依赖VM，将来独立的
语言服务可零耦合复用）。nide在其上叠加分层：全局配置（工具 → 选项）
+ 项目配置（项目 → 属性），项目条目优先；构建 / 运行 / 调试统一传同一
组`-I`。修改库`.n`不需要编译器侧的失效机制：每次构建都从磁盘重新
解析。

## 5. Native宿主ABI

native实现是普通动态库，背后只有一个稳定契约
（`include/nlang/vm/NativeHost.h`）：

- 文件名`nlang_<ns>.dll`（`libnlang_<ns>.so`/`.dylib`），在第4节的
  搜索路径上定位；
- 仅一个导出入口`nlang_native_init`，经宏注册并同时钉住宿主ABI版本
  （`NLANG_HOST_ABI_VERSION`）；版本不符则加载失败并给出可读错误，
  无入口的模块直接拒绝；
- native函数拿到一个小**宿主接口**：输入输出（IO，input/output）、伪随机数生成器（PRNG，pseudorandom number generator）与字符串访问的回调，
  而不是链接VM——第三方源码只需要这一个头文件；
- 懒加载：首次调用某命名空间才触发模块加载，未用到的库零成本。

DLL里的普通C++ 函数可以被同一个`.n`中的NLang函数体包装组合（例
如NLang的`quad`两次调用native的`dbl`），标准库与
`tests/fixtures/native/mylib/`都是这个形态。

## 6. 标准库由什么构成

不存在签名表调用路径：codegen发射限定的`OP_CallFunc`，resolver直接
对内联AST绑定，VM不携带任何标准库知识。

| 对象 | 作用 |
|---|---|
| `stdlib/*.n` | 手写的权威声明——唯一的签名来源 |
| `src/native/{math,io,fs}/` | native实现，构建为`nlang_math.dll`、`nlang_io.dll`、`nlang_fs.dll` |
| 重复包检查（`ModuleRegistry::RegisterUnit`＋发现） | 一次构建内每个点分包名只能存在一份；错误指名两条来源路径 |
| `kStringMethodTable`（`include/nlang/vm/StdLib.h`） | 仅string方法——接收者分派的内建方法，仍是intrinsic；库命名空间不用这套机制 |
| ctest `no_builtin_stdlib` | 一旦硬编码表、math/io/fs的intrinsic家族或它们的id回归，或`stdlib/*.n`被删除而非表被删除，即失败 |

`io.print`即`native void print(string)`：int/float/array经通用string
形参隐式转换，class/enum/func要求显式`.toString()`。没有
`TypeKind::Any`，也没有print专属的实参转string codegen。

## 7. 设计取舍

- **编译期解析源码签名，运行期装载预编译库包**：内联让函数体可读、可改、可重编，并保住单一解析路径；少量`.n`每次重析的成本可以忽略。产码只生成消费方自己的单元，库单元的映像随其包分发（标准库即`stdlib.npkg`），链接在加载期完成——「无源码分发」与源码内联两种形态由此并存，且都按函数逐个分派，混合native/托管库两边都成立。
- **标准库与第三方库共用一条内联路径**：内联机制对两者完全相同，不存在按库来源分流的第二套代码生成。
- **放弃通用`any`而不是实现它**：string形参的隐式转换已覆盖int/float/array，与语言其余部分一致；顶类型意味着第二套特例实参ABI。
- **nide不编译第三方`.cpp`为DLL**：采用C/Python扩展模型——库作者随包提供二进制或构建脚本；DLL过期只做告警，不做自动编译。内置通用native构建命令需要工具链发现与跨平台flag支持，当前收益不足。
- **库源码不做增量缓存**：正确性来自每次构建重新解析；缓存留待编译耗时数据证明必要之后再谈。

## 8. 测试约定

按仓库规则：真实编译`.n`、真实运行`VmExecutor`（不mock内部）——
见`test_library_source.cpp`、`test_thirdparty.cpp`、
`test_native_loader.cpp`/`test_native_abi.cpp`、
`tests/fixtures/native/`下的fixture，以及`check_nvm_native.py` /
`check_ndb_native.py` e2e脚本。
