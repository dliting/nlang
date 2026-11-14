# 泛型相关术语规范与修改指引

用于统一用户手册、设计文档、源码注释与代码标识符的用词。本文只给规范与改法，不含任何改动记录。

## 0. 目的

为了保证术语的准确性，方便交流沟通，需要对文档、注释和代码中的相关词汇进行矫正：用户手册（中英文各一份）、设计文档、源码注释、代码标识符。

判断一个词该不该改，看三条：外部读者能不能搜到这个说法；搜不到时能不能对上已知概念；首次出现处有没有解释。

## 1. 处置原则



* **术语一致优先于兼容**。项目当前不做向下兼容，标识符、用户可见输出、格式字段都可以为 “用词准确” 而改，不必保留旧名当别名。

* 改名要按语义定名，不按词表机械替换。同一个词在仓库里指两件不同的事时，各自按各自的语义处理，允许只改其中一支。

* 代码标识符改名属代码变更，须走完整门禁；文档改词属文本变更，中英文成对提交。

* 改名会改变用户可见输出的，同时更新手册对应章节、`CHANGELOG` 与 `VERSION`。

## 2. 术语表

推荐用词一列是唯一写法；停用词出现在用户手册、源码注释或代码标识符里都要改；标识符一列给出代码里应使用的名字。



| 概念                  | 推荐用词    | 英文                         | 标识符                             | 停用词                           |
| ------------------- | ------- | -------------------------- | ------------------------------- | ----------------------------- |
| 泛型路线：每个实例化各编一份代码    | 单态化     | monomorphization           | —                               | 印制、印码、铸造（作路线名）                |
| 泛型路线：运行时造出具体类型      | 具现      | reification                | —                               | —                             |
| 泛型路线：运行时丢弃类型参数      | 擦除      | type erasure               | —                               | —                             |
| 泛型路线：类型信息随隐藏参数传递    | 字典      | dictionary                 | —                               | —                             |
| 模板填入类型实参得到的具体类型     | 实例化     | instantiation              | `Instantiation`、`instantiation` | —                             |
| 造出对象、数组、节点（动词）      | 创建、生成   | create                     | `Create`、`create`               | 铸造、铸、mint                     |
| 与运行时、编译期并列的层面       | 语言层     | language layer             | —                               | 语言面                           |
| 使用库或 API 的一方        | 使用方     | consumer                   | —                               | 消费方                           |
| 类型参数出现的地方           | 位置      | site／location              | —                               | 位点                            |
| 选型推理中的前置条件          | 决定性前提   | decisive precondition      | —                               | 多米诺                           |
| 类型信息在运行时的存放处        | 存放位置    | storage location           | `storageLocation`               | 载体（仅指类型信息时）                   |
| 运行时拼出规范拼写所依据的记录     | 拼接记录    | composition record         | `composition`、`compositionIdx`  | 合成配方、配方（仅指泛型机制时）、recipe       |
| 不带包前缀的类型名，如 `Box`   | 非限定名    | unqualified name           | —                               | 裸名、bare name                  |
| 带包前缀的完整名字，如 `a.Box` | 全限定名    | fully qualified name       | —                               | 包限定名                          |
| 泛型与 C 互操作的边界        | C 接口边界  | C ABI boundary             | —                               | 原生边界                          |
| 实例化在运行时的标准写法        | 规范拼写    | canonical name             | —                               | 完整拼写（指规范拼写时）、canonical spelling |
| 全部实参都是具体类型          | 实参全部已知  | fully known type arguments | —                               | 拼得全                           |
| 实参里含类型参数            | 实参含类型参数 | type-parameter arguments   | —                               | 拼不全                           |
| .NET 按值类型各生成一份代码    | 专用化版本   | specialization             | —                               | 专用版                           |
| 所有实例化共用的代码体         | 泛型体     | generic body               | —                               | 共享体                           |
| 性能与复杂度上的付出          | 开销、代价   | cost                       | —                               | 一笔账                           |
| 执行调度中的 OS 线程        | 执行载体    | carrier thread             | `carrier`、`Carrier`（保留）         | —                             |

两条排版规则：中文与英文、符号相邻处不留空格；缩略语只用业界现成的（ABI、GC、IL、P/Invoke），不自造。

## 3. 代码标识符

### 3.1 拼接记录：`recipe` → `composition`

文档层英文用全名 `composition record`，代码里用短名 `composition`，两者指同一个对象。反汇编输出要一并改，属用户可见变化。



| 旧                                                                    | 新                                                 | 位置                                         |
| -------------------------------------------------------------------- | ------------------------------------------------- | ------------------------------------------ |
| `std::vector<uint8_t> recipe`                                        | `composition`                                     | `include/nlang/vm/CompiledModule.h` 描述符行字段 |
| `row.recipe`                                                         | `row.composition`                                 | `src/vm/Disassembler.cpp`                  |
| `desc=recipe#N`                                                      | `desc=composition#N`                              | 反汇编输出                                      |
| `dst=%u recipe=%u`                                                   | `dst=%u composition=%u`                           | `OP_TypeNameCompose` 的反汇编操作数               |
| `recipeIdx`                                                          | `compositionIdx`                                  | `src/vm/BytecodeOps.h`                     |
| `recipeLen`、`child recipe bytes`                                     | `compositionLen`、`child composition record bytes` | 拼接记录的标记流格式                                 |
| `AppendArgRecipe`、`IsDescriptorRecipe`                               | `AppendArgComposition`、`IsDescriptorComposition`  | 编译期发射侧                                     |
| `NcuLinker`、`ModuleLoader`、`ModuleSaver`、`DescriptorSpelling` 内的同名字段 | `composition`                                     | 读、写、链接、拼写四处                                |

同一批要改的还有断言了旧输出文本的测试：`test_generic_descriptor.cpp`、`test_generic_runtime_info.cpp`、`test_nculinker.cpp`、`test_nculoader.cpp`。

### 3.2 造对象：`mint` → 按语义分三档

`mint` 相关标识符约 937 处 / 183 文件（`include` 76 处 / 15 文件、`src` 578 / 115、`tests` 283 / 53）。它被用在三件不同的事上，不能一律换 `create`：



| 档 | 语义            | 典型符号                                                                                                                                                                                     | 改为                                                                                                                                                                                                          |
| - | ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 一 | 造节点、造对象、造句柄   | `MintHostString`、`MintNewString`、`MintArrayString`、`MintOwnedArray`、`MintThreadObject`、`MintFutureObject`、`MintBacktraceList`、`MintInitListTypeRow`、`MintShell`、`RootMintedNativeHandle` | `CreateHostString`、`CreateNewString`、`CreateArrayString`、`CreateOwnedArray`、`CreateThreadObject`、`CreateFutureObject`、`CreateBacktraceList`、`CreateInitListTypeRow`、`CreateShell`、`RootCreatedNativeHandle` |
| 二 | 泛型实例化深度与配对    | `ExprResolverGenericMint`、`MintDepthClaim`、`m_iGenericMintDepth`、`MintDepthLimitReached`、`ExprResolverGenericMint.cpp`                                                                   | `ExprResolverGenericInstantiation`、`InstantiationDepthClaim`、`m_iGenericInstantiationDepth`、`InstantiationDepthLimitReached`                                                                                |
| 三 | “造过没有” 的状态与门控 | `NativeMintGate`、`mintGate`、`natMint`、`hsMint`、`pMinted`、`m_minted`、`remint`、`mintable`、`minter`、`minters`                                                                               | 按所在模块定名：`Created`、`hasCreated`、`CreationGate`，不要生造新比喻                                                                                                                                                       |

逐处判定的少数符号，定名前先看用途，不要猜：`SetMintRootGapMsForTests`、`m_mintRootGapMs`（与分配／GC 相关，语义需确认）、`MintNewString` 与 `MintHostString` 是否共用同一工厂。命名长度按项目习惯：局部变量短名可用 `composition`／`instantiation`，公共头文件里用完整名。

### 3.3 执行载体：`carrier` 保留

`carrier` 指执行调度里的 OS 线程（`--carriers=N`、调试器 pin 的 carrier）。Java 虚拟线程把承载虚拟线程的 OS 线程正式称为 carrier thread，这个词在并发领域是标准说法，不是自造词，**保留不改**；中文并发章节的 “执行载体” 也保留。

但 “载体” 指类型信息存放时一律改 “存放位置”，两者按「同形异义」一节判语义。`src/runtime/PrimitiveTypes.cpp` 里 `INFO_ROW` 的 `Carrier` 形参宏体虽未使用，但该列被 `SCALAR_PRIMITIVE_DECL` 的其他展开消费（`CARRIER_ROW` 以它定义`PrimCarrier<T>`的typedef），形参位次不可删；各展开共用同一行表、形参同名属X-macro惯例，维持原样，与术语无关。

## 4. 用户手册

### 4.1 硬规则



* 中文 `docs/user_manual/zh` 与英文 `docs/user_manual/en` 成对修改，任一侧改动必须同步另一侧。

* 手册是规范文本，改词不得改变规范含义；一句话里既有措辞问题又有语义问题时，拆成两个提交。

* 提交前跑 `tools/manual_sync_check.py`，把基线以来每个提交逐条判为语言层／标准库／诊断文案／退出码／ABI 五类；纯措辞统一的提交在提交信息里注明 “措辞统一，无语义变更”。提交手册后用 `--update` 刷新基线。

* 反汇编文本改名（`desc=recipe#N` → `desc=composition#N`）属用户可见输出，手册 `vm-architecture/bytecode-instructions.md` 与 `formats/26-ncu.md` 必须同批改完，再记 `CHANGELOG`。

### 4.2 待改清单



| 文件                                            | 现状                                                      | 改法                                                             |
| --------------------------------------------- | ------------------------------------------------------- | -------------------------------------------------------------- |
| `en/specification/formats/26-ncu.md`          | 8 处 `recipe`：字段表 \`                                     | recipe                                                         |
| `zh/specification/formats/26-ncu.md`          | 8 处 “配方”：字段表 \`                                         | 配方                                                             |
| `en/specification/04-types/generics.md`       | 3 处 `construction recipe`、`the recipe`、`element recipe` | `composition record`、`the record`、`element composition record` |
| `zh/specification/04-types/generics.md`       | 3 处 “构造配方”“按配方组合”                                       | “拼接记录”“按拼接记录组合”                                                |
| `en/vm-architecture/bytecode-instructions.md` | `OP_TypeNameCompose` 的操作数写成 `dst, recipeIdx`            | 改 `dst, compositionIdx`，与代码一致                                  |
| `zh/vm-architecture/bytecode-instructions.md` | 同条指令的操作数说明                                              | 与代码一致，注明拼接记录的索引                                                |
| `en/libraries/integrating-nlang.md`           | `## 4. Deployment recipe`                               | 改 `## 4. Deployment steps`（这是部署步骤，与泛型无关）                       |
| `zh/libraries/integrating-nlang.md`           | `## 4. 部署配方`                                            | 改 `## 4. 部署步骤`                                                 |

其余旧词按文件逐个处理，规模为：



| 停用词      | 中文手册         | 主要落点                                                                                                                       | 改法                                  |
| -------- | ------------ | -------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| 铸造（动词）   | 21 次 / 8 文件  | `specification/04-types/generics.md`、`04-types/array.md`、`formats/28-abi.md`、`10-concurrency/index.md`                     | 创建／生成；层数上限句改 “实例化嵌套至多展开 16 层”       |
| 裸名       | 42 次 / 19 文件 | `getting-started/faq.md`、`03-program-structure/declarations.md`、`libraries/developing-libraries.md`、`cli-tools/ndisasm.md` | 非限定名                                |
| 位点       | 19 次 / 10 文件 | `07-functions/function-values.md`（“消费者位点”）、`08-exceptions/index.md`（“发生位点”“VM 位点”）                                         | 位置；前者改 “使用方位置”                      |
| 包限定      | 9 次 / 8 文件   | 名字解析、导入可见性各节                                                                                                               | 全限定名                                |
| 消费方      | 20 次 / 8 文件  | 库调用、绑定位置                                                                                                                   | 视语境改 “使用方”；纯 API 消费者语境可保留，同一文件内不得混用 |
| 载体（类型信息） | 少数，混在 146 次里 | 泛型章节里指 “类型信息存在哪” 的句子                                                                                                       | 存放位置；其余为执行载体，不改                     |

英文手册另有四处系统性写法要统一：



| 停用写法                                | 现状                                                                                                                                                                                          | 改为                                                                                  |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| `mint` / `mints` / `minting`        | `04-types/array.md`（native minting）、`04-types/generics.md`（mints at most 16 nested instantiations）、`10-concurrency/index.md`（spawn-minted、mints a retrieval handle）、`formats/28-abi.md` 6 处 | `create` / `creates` / `creating`；层数句改 "instantiations nest at most 16 levels deep" |
| `canonical spelling`                | `04-types/generics.md` 多处                                                                                                                                                                   | `canonical name`                                                                    |
| `bare name` 与 `unqualified name` 并存 | `cli-tools/ndisasm.md`、`getting-started/faq.md`、`libraries/developing-libraries.md` 用 bare name；`04-types/enum.md`、`reference/known-limitations.md` 用 unqualified name                      | 统一 `unqualified name`                                                               |
| `fully qualified`                   | 已在用                                                                                                                                                                                         | 保留                                                                                  |

## 5. 设计文档与注释

设计文档只改仍在维护的部分：`docs/dev/*_design.md` 与 `docs/dev/README.md`。`*_plan.md` 与 `docs/superpowers/plans/` 是实施过程记录，按归档处理，不回溯改写。

待改较多的文件：`generic_runtime_typeinfo_design.md`（载体 11、铸造 3、配方 9）、`generic_completion_design.md`（配方 10）、`generics_design.md`（印制 6、载体 5、消费方 3）、`name_resolution_design.md`（裸名 18）、`abstract_functions_generic_interfaces_design.md`（铸造 3）。注意 `concurrency_design.md`、`net_library_design.md`、`net_http_perf_design.md`、`http_perf_baseline.md` 里的 “载体” 与 “配方” 分别是执行载体和性能基线配方，不是本表对象。

源码注释：注释用英文（项目规则），中文注释属历史遗留，可随修改顺手换掉。中文注释现状为 “载体” 133 次 / 8 文件（多数是执行载体）、“位点” 9 / 4、“裸名” 7 / 4、“铸造” 6 / 5、“配方” 3 / 3、“消费方” 2 / 2。英文注释里的 `mint` 与代码标识符同步改，不要只改注释留下对不上的代码。

## 6. 验收

改完一批，统计四件事并写进提交信息：



1. 该词剩余次数（用户手册、源码注释、代码标识符分别统计）。

2. 是否新增同义混用（如 “非限定名” 与 “裸名” 同时存在、`composition` 与 `composition record` 混指两物）。

3. 中英文是否对齐。

4. 手册与代码是否出现说法不一致。

代码改名另加三道：



* 门禁：Debug（断言）＋ ASan ＋ IDE 树；只改标识符不动逻辑，可并行重建，e2e 串行。

* 输出比对：反汇编改名前后的文本差异逐条确认为预期变化，测试断言同批更新。

* 发布记录：用户可见输出变化写进 `CHANGELOG`，`VERSION` 按补丁位递增。

## 7. 同形异义：先判语义，再决定改不改



| 词       | 含义 A                | 含义 B                    | 判定方法                                       |
| ------- | ------------------- | ----------------------- | ------------------------------------------ |
| 载体      | 类型信息的存放位置（改 “存放位置”） | 执行载体：OS 线程、调度槽（保留）      | 看上下文有没有 `carrier`、`--carriers`、线程、调度；有则属 B |
| 配方      | 泛型拼接记录（改 “拼接记录”）    | 部署配方、性能基线配方             | 看是否与部署、安装、基准测试同段                           |
| 铸造／mint | 旧路线名（已废，改 “单态化”）    | 造一个对象／数组／节点（改 “创建”）     | 看宾语是路线还是具体对象                               |
| 位点      | 类型参数出现的位置           | 报错发生的位置                 | 两者都改 “位置”，但要写清限定语                          |
| 裸名      | 不带包前缀的类型名（改 “非限定名”） | 不带类型实参的构造写法，如 `Hold(3)` | 看是否在讲包与名字解析                                |

## 8. 执行顺序



1. 泛型格式与字节码章节（`26-ncu`、`04-types/generics`、`bytecode-instructions`）先行：它们同时被手册和格式规范引用，先把基准术语定下来，中英文同批。

2. 手册其余章节的铸造／裸名／包限定名／位点，按节处理、逐节验收。

3. 仍在维护的设计文档。

4. 源码中文注释。

5. 代码标识符：`recipe` → `composition` 优先（牵涉格式字段与反汇编输出，牵一处要牵一串），`mint` 三档次之；`carrier` 不动。代码改名独立分支单独走门禁。

## 9. 不做的地方



* `CHANGELOG.md`、`CHANGELOG.zh-CN.md`：历史记录，不回头改；只有新增条目记录本次改名。

* `_CPack_Packages/` 下的打包副本、`build-*`、`temp/`：构建产物与临时目录。

* `docs/dev/*_plan.md`、`docs/superpowers/plans/`：实施过程记录，保留原样。