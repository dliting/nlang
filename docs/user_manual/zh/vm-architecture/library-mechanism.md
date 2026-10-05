# 库机制：搜索路径、源码内联与 native 模块

本页记录 NLang 库系统在各轮演进（阶段 1–6）之后的整体设计：一个库
如何被发现、解析、编译和执行，以及机制为何是这个形状。这是面向维护者
的设计文档；用户使用面见[标准库](../language-spec/standard-library.md)。

## 1. 指导原则

**标准库与第三方库是同一套机制。** 标准库就是搜索路径上的
`stdlib/*.n`，和其他库没有任何区别；编译器与 VM 不区分二者。这对应
Python（标准库就是 `sys.path` 上的普通源码）与 Java/C#（托管源码与Java本地接口（JNI）、P-Invoke这类native实现共存于同一个包）。编译器和 VM 里没有任何
硬编码的标准库签名：声明文件与 native 动态链接库（DLL，dynamic-link library） 就是全部实现。

## 2. 库的形态

一个库就是搜索路径上的一个目录：

```
mylib/
  mylib.n            # 库本体：声明、NLang 实现，或两者混合
  nlang_mylib.dll    # 可选：`native` 声明的 native 实现
```

库的接口面就是 `<pkg>.n` 文件本身——包名由文件相对匹配搜索根的路径
派生（`vendor/graphics.n` 在根 `R` 下即包 `vendor.graphics`），文件内
不再有任何包装语法。函数分两类，可以自由混合（**混合库**）：

- **普通 NLang 函数**——有函数体；编译进自己所在包的单元映像
  （如 `stdlib.npkg` 的成员），运行期与消费方链接后按字节码执行；
- **`native` 函数**——只有签名与文档注释（无函数体）；运行时经宿主
  应用二进制接口（ABI，application binary interface） 分派到 `nlang_<包名>.dll`（第 5 节；DLL 按包名第一个点之前的段
  命名，因此多段包里的 `native` 声明是编译期诊断）。

标准库同形：`stdlib/io.n`、`math.n`、`fs.n` 是手写的权威声明文件
（io/math/fs 三个包目前为纯 native），实现由 `src/native/` 构建
为 `nlang_math.dll`、`nlang_io.dll`、`nlang_fs.dll`。

## 3. 编译模型：源码完整内联

单段 `import` 在搜索路径上找到的库 `.n` 会被**完整解析**（含函数体），
成为*库翻译单元*，与项目翻译单元合并进同一个 抽象语法树（AST，abstract syntax tree） 根
（`src/compiler/ModuleBuilderImports.cpp`、
`src/compiler/builder/ModuleRegistry*`）。构建顺序：

1. 解析项目源（收集 import）；
2. **库发现迭代到不动点**：worklist 以项目的 import 起步；带点名
   `a.b.c` 在第一个持有它的搜索根下定位 `<根>/a/b/c.n`（多个根提供
   同一个包是重复包错误，指名两条路径），每个定位到的文件依次
   (a) 做签名索引（`langservice::SymbolIndex`，按匹配根的包名）、
   (b) 完整解析为库 编译单元（TU，translation unit），其自身的 import 再加入 worklist——第三方
   库因此可以依赖其他库。命中项目已有源文件的包不重复内联；
3. 注册全部 TU，库 TU 带 `isLibrary` 标志，包名取自匹配的搜索根；
4. 展开别名、构建 import 门、加载外部模块（`.ncu`/`.npkg`）；
5. 解析、**逐单元**生成代码——库单元不参与产码：消费方映像只携带
   导入槽，库代码由库自身的包在运行期提供。

统一模型带来以下结论：

- **命名空间限定调用编译为普通调用**：`mylib.f(...)` 发射
  `OP_CallFunc` + 全限定名 `ns.f`；运行时按被调函数自身性质分派——
  字节码体，或 `isNative` → DLL。签名表调用路径与按库区分的 codegen
  分支已不存在。
- **函数表中命名空间限定的顶层函数使用全限定名**（含库 native 函数），
  使运行时 `m_natives["ns.name"]` 查表与 DLL 导出的注册名一致。
- **库可以定义类型**：命名空间合并进根，库内 class/struct/enum/
  interface 在消费方与项目类型同等解析（含继承、虚分派、enum 方法）。
  类型位置写作 `ns.Type`，解析器在被内联的库单元内查找声明并绑定
  （`ModuleRegistry::FindModuleType`）；未导入的命名空间会被诊断，
  而不是静默绑定。外部 `.ncu` 不暴露源码级类型，因此 `ns.Type`
  只对内联库源生效。
- **去重**按"已完整解析的库文件"集合跟踪，与符号索引的"已索引"集合
  分离：标准库包在构建环境构造时就已签名索引，但仍必须内联，
  两者不能混用同一个判据。

### 库 TU 与项目 TU 的差别（可见性隔离）

库 TU 编译进构建，但刻意**不是**项目模块：

- 不加入同目录隐式可见池（D7 规则只适用于项目），也不把同目录其他
  文件注入自身视野；
- 通配 import 不会把库 TU 当项目模块卷入；
- 库永远看不到消费方项目的模块，只看得见自己 import 的内容；
- 同一次构建内两个单元解析出同一个点分包名是重复包错误，诊断指名
  两条来源路径（保留名表已被此规则取代——名为 `io` 的项目目录现在
  就是普通目录）。

库之间的循环 import 允许：所有 TU 先合并、后统一解析，声明在一次编译
内彼此可见，与项目多源文件的道理相同。

### 运行期：闭包装载与链接

产码不含库代码，所以执行的第一步是把引用闭包装齐、链接成一个
运行期模块（`src/vm/NcuLoader.cpp`、`src/vm/NcuLinker.cpp`）：

1. **装载**（nloader）——从入口产物出发：`.npkg` 的成员先在包内
   解析；导入槽的目标模块沿搜索路径找 `<模块路径>.ncu` 文件或包含
   该成员的 `.npkg`，递归到闭包完整。成员映像头部的模块路径必须与
   目标一致（名实不符即拒绝），每步做版本与校验和校验；缺包诊断
   一次报清，列出已搜索目录。
2. **链接**（nlink）——纯内存变换：N 幅单元映像按限定名合并去重，
   占位槽解析成全局表下标，全部操作数统一重映射；未解析符号与
   可见性违规一次报清。入口按程序包的入口记录或裸单元的
   `<模块路径>.main` 约定解析。

`nvm`、`ncc run`、ncc 的编译后立即执行与 ndb 会话全部走这同一条
路径——工具之间没有第二条装载/链接实现。标准库在运行期就是搜索
路径上的 `stdlib.npkg`，与其他库包无异。

## 4. 搜索路径

编译期发现 `.n`、运行期装载闭包成员（`.ncu`/`.npkg`）与加载 native
DLL 使用**同一个有序目录列表**——前者优先，重复目录规范化去重
（Windows 上折叠大小写）。五层，从高到
低（`include/nlang/common/LibrarySearchPath.h`）：

1. 命令行 `-I <dir>`（可重复）；
2. `.nproj` 的 `<ImportPaths>`；
3. 项目 / 源文件 / 模块所在目录；
4. 环境变量 `NLANG_PATH`（Windows `;`、POSIX `:`）；
5. 系统缺省：标准库目录、可执行文件目录、当前目录。

`ncc`、`nvm`、`ndb` 共用这一个 header（纯 STL、不依赖 VM，将来独立的
语言服务可零耦合复用）。nide 在其上叠加分层：全局配置（工具 → 选项）
+ 项目配置（项目 → 属性），项目条目优先；构建 / 运行 / 调试统一传同一
组 `-I`。修改库 `.n` 不需要编译器侧的失效机制：每次构建都从磁盘重新
解析。

## 5. Native 宿主 ABI

native 实现是普通动态库，背后只有一个稳定契约
（`include/nlang/vm/NativeHost.h`）：

- 文件名 `nlang_<ns>.dll`（`libnlang_<ns>.so`/`.dylib`），在第 4 节的
  搜索路径上定位；
- 仅一个导出入口 `nlang_native_init`，经宏注册并同时钉住宿主 ABI 版本
  （`NLANG_HOST_ABI_VERSION`）；版本不符则加载失败并给出可读错误，
  无入口的模块直接拒绝；
- native 函数拿到一个小**宿主接口**：输入输出（IO，input/output）、伪随机数生成器（PRNG，pseudorandom number generator）与字符串访问的回调，
  而不是链接 VM——第三方源码只需要这一个头文件；
- 懒加载：首次调用某命名空间才触发模块加载，未用到的库零成本。

DLL 里的普通 C++ 函数可以被同一个 `.n` 中的 NLang 函数体包装组合（例
如 NLang 的 `quad` 两次调用 native 的 `dbl`），标准库与
`tests/fixtures/native/mylib/` 都是这个形态。

## 6. 标准库由什么构成

不存在签名表调用路径：codegen 发射限定的 `OP_CallFunc`，resolver 直接
对内联 AST 绑定，VM 不携带任何标准库知识。

| 对象 | 作用 |
|---|---|
| `stdlib/*.n` | 手写的权威声明——唯一的签名来源 |
| `src/native/{math,io,fs}/` | native 实现，构建为 `nlang_math.dll`、`nlang_io.dll`、`nlang_fs.dll` |
| 重复包检查（`ModuleRegistry::RegisterUnit` ＋ 发现） | 一次构建内每个点分包名只能存在一份；错误指名两条来源路径 |
| `kStringMethodTable`（`include/nlang/vm/StdLib.h`） | 仅 string 方法——接收者分派的内建方法，仍是 intrinsic；库命名空间不用这套机制 |
| ctest `no_builtin_stdlib` | 一旦硬编码表、math/io/fs 的 intrinsic 家族或它们的 id 回归，或 `stdlib/*.n` 被删除而非表被删除，即失败 |

`io.print` 即 `native void print(string)`：int/float/array 经通用 string
形参隐式转换，class/enum/func 要求显式 `.toString()`。没有
`TypeKind::Any`，也没有 print 专属的实参转 string codegen。

## 7. 决策记录

- **编译期内联源码解析签名，运行期装载预编译库包**（阶段 6 的分工）：
  内联让函数体可读、可改、可重编，并保住单一解析路径；少量 `.n`
  每次重析的成本在没有实测数据前可以忽略。产码只生成消费方自己的
  单元，库单元的映像随其包分发（标准库即 `stdlib.npkg`），链接在
  加载期完成——「无源码分发」与源码内联两种形态由此并存，且都按
  函数逐个分派，混合 native/托管库两边都成立。
- **不设过渡双路径**（标准库走签名、第三方走 AST）：内联机制对两者
  完全相同，4a 一步切换所有库并直接删除签名驱动的 codegen，行为不变
  由全量回归兜底。
- **放弃通用 `any` 而不是实现它**：string 形参的隐式转换已覆盖
  int/float/array，与语言其余部分一致；顶类型意味着第二套特例实参
  ABI。
- **nide 不编译第三方 `.cpp` 为 DLL**：采用 C/Python 扩展模型——库
  作者随包提供二进制或构建脚本；DLL 过期只做告警，不做自动编译。内置
  通用 native 构建命令需要工具链发现与跨平台 flag 支持，当前收益不足。
- **库源码不做增量缓存**：正确性来自每次构建重新解析；缓存留待编译
  耗时数据证明必要之后再谈。

## 8. 剩余工作（设计状态截至 2026-10-04）

已落地的机制：4a 统一内联（提交 `ab4546a`）、in-process 的混合 native +
NLang 库测试（4b-1，`test_thirdparty.cpp`）、库的类型面（4b-2，第 3 节）、
内建标准库的退役（4c，第 6 节）、搜索路径（3d）、native ABI 与加载器
（3b/3c）、加载期闭包装载与链接（阶段 6——nloader/nlink、逐单元产物、
`stdlib.npkg`，第 3 节「运行期」）。未结项：

1. **4d**——变更感知的自动重编译：nide 的 standalone 过期检测要把内联
   的库 `.n` 纳入比较，而不能只看主源。两个候选设计，共同点是库发现
   规则只活在编译器一处：把参与编译的库源（路径 + mtime）序列化进
   `.ncu`（格式升版；项目不要求向下兼容），或提供 `ncc deps` 查询模式
   输出发现列表。另有：ndb 进入库源断点/步进的验证、nide
   `runNccBuild` 异步化。

## 9. 测试约定

按仓库规则：真实编译 `.n`、真实运行 `VmExecutor`（不 mock 内部）——
见 `test_library_source.cpp`、`test_thirdparty.cpp`、
`test_native_loader.cpp`/`test_native_abi.cpp`、
`tests/fixtures/native/` 下的 fixture，以及 `check_nvm_native.py` /
`check_ndb_native.py` e2e 脚本。
