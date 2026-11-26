# 编译器/虚拟机与共享核心库的分层设计

概要设计（spec）。锚定提交：`59aad4ed`（v0.13.12，分支`refactor/nlang-core`）。文件级拆分清单与逐步迁移属实施计划，另文。

## 一、问题

`nlang_vm`与`nlang_compiler`互为依赖：vm内的字节码生成后端include编译器头，编译器的模块构建器include虚拟机头。环不在CMake链接边（vm对compiler的PRIVATE link只是符号兜底），而在头文件包含面；静态库把符号解析推迟到最终可执行文件，环因此静默存在。

后果不是编译失败，而是边界失控：任何一个库的文件都可能合法地触碰另一个库的内部；新代码继续沿这个方向生长；一旦切共享库，静态库掩盖的环立即成为不可解的DLL互锁。

## 二、事实面

以下均在`59aad4ed`上核实。

**链接边现状**
- `src/vm/CMakeLists.txt:180-183`：`nlang_vm` PUBLIC `nlang_runtime`，PRIVATE `nlang_compiler`、`nlang_langservice`。
- `src/compiler/CMakeLists.txt:197-200`：`nlang_compiler` PUBLIC `nlang_runtime`，PRIVATE `nlang_langservice`（不link vm，靠消费者兜底）。
- 问题域五库（runtime/langservice/compiler/vm/embed）的`add_library`均为无关键字形态，全仓无`BUILD_SHARED_LIBS`赋值→均为静态库（问题域之外：`src/native/CMakeLists.txt:48,85`的原生模块为SHARED、3rdparty显式写STATIC）；`NLANG_COMPILER_API`/`NLANG_RUNTIME_API`为空宏占位（`include/nlang/{compiler,runtime}/Config.h:4`），为未来共享链接预留。
- 消费者：`ncc`/`nlang`链compiler+runtime+vm+langservice；`nvm`/`ndb`链vm+langservice；`ndisasm`链整个vm；`nlang_embed`（PRIVATE）链vm+runtime+compiler，且PRIVATE借用`src/vm`私有include目录；`ncc`同样PRIVATE借`src/vm`目录并以裸名消费其私有头（`src/tools/ncc/CMakeLists.txt:11-14`、`main.cpp:8-13`）；nide各组件按需。install规则装工具exe与stdlib、docs、examples、Qt运行库等运行内容，不装`include/`树与库（`CMakeLists.txt:452`）。
- 环的符号级自供：`ncc`的链名表手工把compiler排在vm之前（`src/tools/ncc/CMakeLists.txt:16-24`）；`tests/CMakeLists.txt:2269-2271`注释明言「nlang_vm is required at link time (nlang_compiler references the VM backend/module-loader symbols in the build path)」；`test_primitivetypes`（`:1221-1226`）的三库排序还与其他套件不同；全仓无`--start-group`/`$<LINK_GROUP>`兜底。反向的compiler→vm符号边从未在CMake声明，全靠消费者把两库都列上且顺序得当。
- `src/compiler/builder/BindingTable.h:8`注释自认「the sanctioned link direction is vm -> compiler」——声明面只承认单向，而第二节的包含面证明真实依赖是双向。

**vm→compiler包含边（全部在码生成面）**
- vm→compiler包含边共59个文件（其中3个是头文件）：`src/vm/backend/`内含边的55个文件（目录共57个文件，`BuiltinClasses.cpp`/`WalkersExprRef.cpp`无跨库include；CMake源码表的57条目系`RegisterStructs.cpp`在`src/vm/CMakeLists.txt:80,88`重复列出）＋`VmBackend.h`＋`GenericInstDescriptor.{h,cpp}`＋`TypeDesc.cpp`。边内容：编译器公开AST头（`nlang/compiler/Sn*.h`系共198处、6个头，另有`TypeArgSpelling.h`、`BuildEnvironment.h`、`Logger.h`等）与编译器私有builder头（`builder/ModuleRegistry.h`13处、`builder/BuiltinNames.h`6处、`builder/SymbolSlots.hpp`、`builder/FieldInitializerChecks.hpp`、`builder/BindingTable.h`、`builder/ThreadNamespace.h`）。
- `src/vm/VmBackend.h`是公开可见的汇聚点：include `ICodeBackend.h`、`SyntaxNodeVisitor.h`、`SnArrayTypeToken.h`、`TypeArgSpelling.h`与私有`builder/BindingTable.h`。
- backend之外的vm文件：`GenericInstDescriptor.{h,cpp}` include `TypeArgSpelling.h`（.cpp另include `CompiledModule.h`/`CompiledModuleRecords.h`，拆分后两者与自身同属core内部边）；`TypeDesc.cpp`只在**写侧AST构造助手**（AST节点→类型描述符，消费`SnArrayTypeToken.h`/`SnMisc.h`，错误出口用`SyntaxNodeConsts.h`的`NodeKindDebugName`）里依赖AST，其wire序列化/解析半（`AppendTypeDescBytes`/`ParseOne`）是纯格式面——这一混合文件按§五的簇切分函数级拆家。
- 执行面（`VmExecutor*`、`VmScheduler*`、GC、intrinsics实现、native host、`DebugSessionController`、`IoPoller*`）无一include AST或builder头——执行器只需要字节码镜像，不需要AST。
- include面之外还有一条**符号穿边**：`CanonicalArgSpelling`定义在compiler侧`src/compiler/builder/TypeArgSpelling.cpp:40`（纯`TypeArgDesc`→字符串函数），被`GenericInstDescriptor.cpp:32`与执行面`VmExecutorOpsDescriptor.cpp:80,133,336,382`、`VmExecutorOpsArrayCheck.cpp:153`调用；现状由CMake把nlang_vm的PRIVATE compiler依赖自动附加到消费者链名表兜底（`nvm`/`ndb`/`ndisasm`链名表本身不列compiler）。该.cpp整体含AST/builder include（`:9-13`：`ModuleRegistry.h`、`GenericInstanceRegistry.h`、`SnArrayTypeToken.h`/`SnExpressions.h`/`SnMisc.h`），无法整体入core——处置见§五。

**compiler→vm包含边（全部在模块镜像与加载面）**
- 镜像模型：`ModuleBuilder.h:9`（公开头）include `nlang/vm/CompiledModule.h`；`builder/CompiledModule*Stub*.hpp`系列在解析期直接构造镜像stub；`builder/BindingTable.cpp`、`ExprResolverBuiltinClasses.cpp` include `nlang/vm/Intrinsics.h`。
- 导入加载：`ModuleBuilder.cpp:21`、`ModuleBuilderImports.cpp:16`、`ModuleBuilderExternalImports.cpp:12`裸名include `ModuleLoader.h`（vm私有头被跨库include）；`NcuPackage.h`在导入与单元装配位点消费（含`ModuleBuilderUnits.cpp:14`）。
- 码生成驱动：`ModuleBuilder.cpp:192`直接`new VmBackend()`，`ModuleBuilderUnits.cpp:157`调`GenerateUnits`——编译管线由编译器持有，码生成实现却住在vm。

**既是码生成又是唯一langservice消费者的vm文件**
- `src/vm/backend/EmitCall.cpp:10`是vm侧唯一的`SymbolIndex`消费者（读标准库声明面）；编译器侧经`BuildEnvironment::LibraryIndex()`正常使用。即P3落地后vm不再需要langservice。

**共享词汇已经存在且已下沉过一程**
- `include/nlang/compiler/TypeArgSpelling.h`文件头自述为「类型实参的共享词汇……解析器、后端与加载侧拼写解析器共用一份分类」；其include仅有runtime两出头（`PrimitiveTypes.h`、`RuntimeTypeCodes.h`），AST类型均为前置声明——它住在compiler命名空间纯属历史。
- 词汇根基更早已在runtime：`NodeKind`定义于runtime树（`RuntimeNodeConsts`系，`include/nlang/runtime/NodeConsts.h:113`有`enum RuntimeNodeKind : NodeKind`，compiler侧`SyntaxNodeConsts.h:95`有`enum CompNodeKind : NodeKind`），标量关键字列的权威表在`runtime/PrimitiveTypes.h`。也就是说「两侧共用的数据面」这个项目自己已经承认并逐级下放过一次，本设计是顺势走完。
- `ICodeBackend`（`include/nlang/compiler/ICodeBackend.h`）Phase 6后只剩`OnModuleCreate`一个钩子，注释明言「码生成本身是后端特定的，VmBackend::BuildUnitImages直接驱动」——抽象已空心化。

**文档矛盾**
- `AGENTS.reference.md`「模块边界」声称runtime无依赖、compiler=前端+runtime、vm=后端+runtime，无compiler↔vm边；该节与上述事实不符，属过期描述，随本任务修正（该文件为本地独占文档，不入库）。

## 三、业界先例

- **LLVM**：库分层单向——`lib/Support`←`lib/IR`←`lib/Analysis`/`lib/Transforms`←`lib/CodeGen`/`lib/Target`；前端（clang）的AST永不进入后端，后端与执行引擎（`lib/ExecutionEngine`）只消费IR。映射到本项目：`.ncu`镜像≈IR；`src/vm/backend`的码生成消费AST与解析语义（BindingTable、ModuleRegistry），按LLVM的归类它就是前端侧CodeGen，归属编译器而非虚拟机。
- **OpenJDK JEP 466（Class-File API）**：把类文件格式的解析/生成/转换从JDK内部拆成独立公共模块，编译器与工具共同依赖，动机正是格式契约散落在各消费者内部会随格式演进漂移（https://openjdk.org/jeps/466 ）。映射到本项目：`ModuleLoader*`/`ModuleSaver*`/`Ncu*`/镜像模型是编译器与虚拟机之间的交付契约，应独立成库而不是寄生在执行库里。
- **Wasmtime/Cranelift**：运行时与编译器是两个独立组件，共享面仅为字节码格式解码；cranelift不反向依赖运行时内部（https://docs.wasmtime.dev/internals.html ）。
- **反例Lua/CPython**：单一库整包、无分层。适用条件是规模小、消费者只有解释器自身。本项目有六个消费者目标、独立调试器/IDE树、可选LLVM后端与独立的标准库预置包流水线，形态不符；整包化会把Bison/Flex生成物与解析机制推给纯执行工具（`nvm`/`ndb`/`ndisasm`），故排除。

## 四、候选拓扑与裁决

| 候选 | 形态 | 判定 |
|---|---|---|
| P0 | 维持现状，靠include目录互加+消费者兜底 | 否。环被静态库掩盖而非消除：共享库化即爆；文件归属无准则可循；`AGENTS.reference.md`边界描述与事实冲突。 |
| P1 | 编译器+vm合并为单一库 | 否。图无环但回到Lua/CPython形态；`nvm`/`ndisasm`被迫背前端生成物与解析机制；可选LLVM后端被拖进所有消费者；与六目标+IDE树的规模不符。 |
| P2 | 码生成迁入compiler，不建新库 | 不足。`vm→compiler`消失，但`compiler→vm`仍在（`CompiledModule`、`ModuleLoader`、`NcuPackage`），格式契约继续寄生在执行库内，「vm」名义上成了编译器的下层而实际包含调度器/GC/宿主桥——语义倒挂未修，共享库拆分仍不可行。 |
| P3 | 新增下层共享库，承载模块格式与共享词汇；码生成迁入compiler；vm瘦身为纯执行 | **采纳。**全部依赖单向向下；每层职责单一；P2的全部残余洞均被闭合。 |
| P3变体 | 格式面并入compiler侧（vm经include目录读格式头） | 否。执行器反过来链编译器，方向倒挂比P2的残余更重：`nvm`/`ndisasm`被迫背整个前端。 |

P3内部两个变体的裁决：
- **AST是否也进核心库（P3b）**：否。执行器不需要AST（事实面已证）；码生成迁入compiler后没有任何非编译器消费者需要AST。把AST压进核心库只为迁就一个已被「码生成归编译器」消解的依赖，是为保留P2残余项而付出的永久性边界模糊。
- **格式并入runtime（P3c）**：否。runtime的定位是执行期数据结构与GC，`.ncu`是编译↔执行的交付契约，二者的变更动因不同（格式随ABI演进、GC随内存策略演进）；`NcuPackage`/`NcuLinker`这类交付面文件进runtime会把构建期概念灌进执行内核。
- **langservice去向**：保持独立叶子库。它是语言声明面索引（IDE/补全/解析共用），与模块格式（ABI交付面）变更动因不同，不并入；P3后vm不再链接它（vm侧唯一消费者是码生成）。
- **命名**：库名`nlang_core`，公开头目录`include/nlang/core/`。理由：该层承载两侧平等依赖的ABI契约（模块镜像/格式/共享词汇），"core"传达的是「共同地基」而非杂项共用；既有`include/nlang/common/`保持header-only纯STL工具的既有约定不动，不与其职责混写。

## 五、采纳形态

```
        nlang_runtime（不动：执行内核词汇、GC、NodeKind、标量表）
              ↑
        nlang_core（新：模块格式与镜像——编译器与虚拟机的交付契约）
         ↑          ↑
   nlang_compiler   nlang_vm
   （前端+解析+码生成）（解释器+调度+GC执行+宿主）
```

**nlang_core准入判据**：只依赖`nlang_runtime`与STL；出现AST、builder解析机制、调度器、GC分配器之一的include即违规。内容按簇（文件级清单归实施计划）：
- 模块镜像模型：`CompiledModule`、`CompiledModuleRecords`、`TypeDesc`（仅wire序列化/解析半与消费侧索引面`TypeDescIndices`；AST→描述符的写侧构造助手随codegen留compiler）、`ExprDesc`、`GenericInstDescriptor`、`Intrinsics.h`（ID词汇；实现留vm）。混合文件按函数级拆家，簇归属以「该函数是否消费AST/解析机制」为准，文件级切分清单归实施计划。
- 字节码词汇：`BytecodeOps`/`BytecodeStrides`/`OpCodeTable`、`BytecodeEmitter`（标签/操作数记录，写入面）、`BytecodeReader`（读取面）。
- 交付流水线：`ModuleLoader*`、`ModuleSaver*`、`NcuPackage`、`NcuLoader`、`NcuLinker*`、反汇编器族（`Disassembler*`，纯格式消费，`ndisasm`由此只需链core）。
- 共享拼写词汇：`TypeArgSpelling`（头从`include/nlang/compiler/`迁入）。实现按函数级拆家：AST无关的拼写拼接半边（`CanonicalArgSpelling`族，`src/compiler/builder/TypeArgSpelling.cpp:40`起）随头迁入core，AST侧（节点拼写，消费`ModuleRegistry`/`Sn*`头，`TypeArgSpelling.cpp:9-13`）留compiler。这不是可选项：格式面与执行面都调用`CanonicalArgSpelling`（§二符号穿边条目），只迁头不迁这半边，core与vm对compiler仍有符号级反向边，「全部依赖单向向下」即不成立。

**nlang_compiler**：现有前端/解析/`ModuleBuilder`原样，新并入码生成簇（`src/vm/backend/*`全部、`VmBackend.h/.cpp`、`DescriptorSpelling`、`FieldOffsets`、`WriteModuleArtifact`等），建议目录`src/compiler/codegen/`（实施计划定）。`ICodeBackend`已空心化：随码生成收编后裁决退役或保留为编译器内部接口，不留跨库抽象（实施计划定）。编译器不再include任何`nlang/vm/`路径——镜像与加载器名字改指`nlang/core/`。

**nlang_vm**：执行面全部保留（`VmExecutor*`、`VmScheduler*`、`GcWorld*`、GC执行、intrinsics实现、native host/`NativeLibraryLoader`、`DebugSessionController`、`IoPoller*`、`CrashReporter`、`InputLineSource`/`TokenView`、`VmExecutorSer*`序列化面、wepoll/win32面）。执行契约头留在vm侧且现状位置不动：`NativeHost.h`/`NativeHostHelpers.h`是vm公开树（现共8个头）在格式头迁出后的仅剩成员；`IDebugHooks.h`/`IHostFunctions.h`/`IHostIo.h`/`VmExecutor.h`等本就是vm私有头，仓内消费者（embed、ndb、tests族）经PRIVATE include目录直达的现状保持——它们都是宿主↔执行器契约，编译器从不消费。vm不再include任何编译器头（现状已如此，拆分后由构建固化）。

**消费者链形**：
- `ncc`、`nlang`：`nlang_compiler`+`nlang_vm`（+`nlang_langservice`），core经PUBLIC传递。`ncc/main.cpp`还借目录以裸名消费`NcuLoader.h`/`NcuLinker.h`（迁core）与`VmExecutor.h`/`NativeLibraryLoader.h`/`TestNatives.h`/`CrashReporter.h`（留vm私有），拆后借用目录从`src/vm`一个改为`src/core`+`src/vm`两个；此处例举非穷尽，改写清单以实施计划的全量盘点为准。
- `nvm`、`ndb`：`nlang_vm`(+`nlang_langservice`)。
- `ndisasm`：仅`nlang_core`（外加INTERFACE库`nlang_version`，无归档无实质影响；现状链整个vm，拆分即瘦身）。
- `nlang_embed`：`SourceCompiler.cpp`消费compiler面、`InterpreterImpl`消费执行与格式面，三库PRIVATE链形保持；其PRIVATE借用`src/vm`目录直达私有头（`VmExecutor.h`/`NcuLoader.h`/`NcuLinker.h`/`IHost*.h`）的现状随头的归属改为借core+vm两目录。
- `nide`各组件按实际用途；`tests/`各套件链名表同步补core。

**公开头树**：`include/nlang/vm/`中的格式面头（`CompiledModule.h`、`CompiledModuleRecords.h`、`TypeDesc.h`、`ExprDesc.h`、`Intrinsics.h`、`NcuPackage.h`）迁至`include/nlang/core/`；现vm私有但被跨库include的头（`ModuleLoader.h`、`BytecodeReader.h`等按准入判据）升为core公开头；`TypeArgSpelling.h`自compiler树迁入。不做兼容别名（无向下兼容原则），直接改include路径。`include/nlang/embed/NLang.h`同步。

## 六、边界固化

拆分若只靠「记得别写回去」维持，会重蹈include注释手工维护的覆辙（`src/vm/CMakeLists.txt:174-177`的"Reverse precedent"注释即是现状补丁的自供）。验收面加机械断言（实施计划落为tools/守卫脚本+ctest条目，先RED后GREEN）：
1. `src/vm/`全树不得出现`nlang/compiler/`或`builder/`include；
2. `nlang_core`源树不得include AST/builder/调度器面；
3. `src/compiler/`不得出现`nlang/vm/`（允许`nlang/core/`）；
4. CMake两库不再互加对方的源码include目录（构建系统层面的同一条断言）；
5. 符号面：`nlang_core`与`nlang_vm`不得引用`nlang_compiler`符号——机械形态是`nvm`/`ndisasm`的链名表不含`nlang_compiler`且实测链接通过。include面断言挡不住符号穿边（`CanonicalArgSpelling`即现状实证，§二），此断言与之互补。

以上include类断言的匹配一律锚定`#include`指令行，注释与字符串中的库名不算命中（如`VmBackend.h:61-63`注释含库名）。

## 七、用户可见面与文档

语言面、标准库面、诊断文案、退出码、ABI五类用户可见面全部不变（其中ABI不变涵盖`.ncu`/`npkg`格式这层字节码与模块交付契约）；`include/`树与库不随安装包发布，嵌入者从源码构建、随仓更新。**版本裁定（用户，2026-11-25）**：本重构作为独立补丁版发布，版本号`0.13.12a`——`VERSION`提升为`0.13.12a`（根`CMakeLists.txt:14`的格式校验本就接受可选字母后缀，`:21`剥离数字段供CMake工程版本）并在`CHANGELOG.md`与`CHANGELOG.zh-CN.md`成对登记（结构性重构条目）。「新增或变更功能须同步手册语言规格」条款不适用（无语言面变化，手册不动）；手册同步面仍走既有纪律：核对人对基线以来每个提交逐条判定「不属五类」，随后以`tools/manual_sync_check.py --update`推进基线（发布门禁要求该工具漂移清零；判定归核对人，工具只列漂移清单）。`docs/dev/README.md`索引收录本文档（其当前有未提交改动归属他线，实施计划核对合并）。同步面：`AGENTS.reference.md`模块边界节按本形态改写（本地独占）；`src/vm/CMakeLists.txt:174-177`的反向先例注释随迁删除，`src/compiler/CMakeLists.txt:194`的借目录行整行移除（compiler侧无注释可删）；README.md未提及库内部名，无需改。

## 八、测试与门禁（概要）

- 纯结构重构，行为零变化：现有`test_vm`/`test_compiler`/`test_tools`族的断言一字不改，预期仅include与链名表适配。测试对编译器私有头的header-only引用不构成库边界（如`test_nculinker`经include目录引用生产侧`builder/SymbolSlots.hpp`的占位槽契约常量，链名表不因此补compiler）。基线对拍用字节级差分（`.ncu`产物前后逐字节一致，配方见`tools/`既有用例），任何输出漂移即回归。
- TDD防线：第六节分层守卫测试先行——在现状树上必红（vm确有compiler include），拆分后转绿。
- 门禁按`tests/README.md`影响映射：构建结构变化波及全部库→影响层取保守Debug全量单测+受影响e2e族；收口批照例一次IDE树全量ctest（编译器面变更规则）与Release/慢测试同点。

## 九、风险与开放问题（归实施计划）

1. 码生成57文件的目录组织与命名（`src/compiler/codegen/`布局）；`WriteModuleArtifact`对saver的调用序细节。
2. `ICodeBackend`退役的具体形态（`OnModuleCreate`钩子的消费位点在`ModuleBuilder.cpp:192-193`——`BuildEnvironment`只是持有者——内化方式归实施计划）。
3. 公开头迁移的全量include路径改写面（实施计划先盘点改写点清单，再分批提交）。
4. 泛型实例化运行期面（`GenericInstDescriptor`）迁入core后，其拼写消费与`ModuleLoader`记录解析的配对关系测试要随文件走。
5. LLVM面现状盘点（第1轮评审更正：并不存在独立的LlvmBackend文件/类）：`NLANG_ENABLE_LLVM`只剩`src/compiler/CMakeLists.txt:207-211`的选项管线与`src/compiler/BuildEnvironment.cpp:15-16,112-118`的`llvm::Module`条件编译钩子；「LlvmBackend」一词仅存于`include/nlang/compiler/ICodeBackend.h:5`的过期注释（随该头退役同批修正）。存量钩子全部在compiler内，未来后端复活亦随compiler，本任务对其零迁移面。
6. 预置包`stdlib.npkg`重打包链不动（格式未变）。

## 十、评审记录

（循环完善流程逐轮追加）

**第1轮（第三方独立评审，锚定59aad4ed初稿）**：High×1、Medium×1、Low×7、Info×5。全部发现经主会话逐条独立核实属实；裁决主干（P3采纳、P1/P2/P3b/P3c否决）经评审逐文件include闭包与对抗性攻击确认独立成立。处置：
- High（§9.5把`ICodeBackend.h:5`过期注释里的幻影`LlvmBackend`当现状引用）→§9.5重写为LLVM面真实现状盘点，并登记该注释随退役修正。教训：过期注释不可作现状采信。
- Medium（`CanonicalArgSpelling`符号穿边未裁定：只迁头则core/vm对compiler仍有符号级反向边，nvm/ndisasm在新链形下undefined symbol，而§六四条include/CMake断言全部照样绿）→§五裁定`TypeArgSpelling.cpp`函数级拆家（AST无关半边随头入core），§二补符号穿边事实条目，§六补第5条符号面断言。机理注记：现状之所以静默，是CMake把nlang_vm的PRIVATE静态依赖以link-only形态自动附加到消费者链名表；新拓扑删掉该声明后兜底消失。
- Low×7（55/59计数口径与CMake重复条目、Sn系198处、`GenericInstDescriptor.cpp`include不全、`ModuleLoader.h`持有者集合错列Units.cpp、静态断言范围越界、install范围越宽、ncc借目录漏项）与Info×5（§9.2钩子消费者、ndisasm+version、格式入compiler变体行、注释仅vm侧、守卫锚`#include`行）→逐条修入§二/§四/§五/§六/§七/§九。
- 版本裁定变更（用户，2026-11-25）：§七原「不提升VERSION」裁定作废，本重构独立发布为`0.13.12a`。

**第2轮（定点复审，修订稿）**：Critical 0/High 0/Medium 0/Low 2/Info 2。两条Medium+的处置均判定充分（拆家裁定三处联动到位、`CanonicalArgSpelling`函数体确为纯AST无关可入core、符号面断言机械可执行）；修订新写入的全部事实句与新坐标（含CMake link-only自动附加机理句）逐条重新核实相符；第1轮12条Low/Info对表全部修入到位；回归扫描无矛盾新句。新发现处置：Low（§七漏`CHANGELOG.zh-CN.md`成对登记、五类句列举6名词致口径错位→格式归ABI子项）与Info（「不属五类」判定归因由工具改回核对人＋补基线推进归置、ncc例举补`TestNatives.h`并注明例举非穷尽）修入§五/§七。Medium+清零，spec循环退出，进入实施计划。
