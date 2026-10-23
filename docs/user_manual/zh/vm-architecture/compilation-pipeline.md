# 编译管线

编译器前端产出抽象语法树（AST，abstract syntax tree）后，`VmBackend`按翻译单元把它翻译为独立的单元映像（`CompiledModule`）：先注册类型与函数，再为所有函数生成字节码。每个源文件一幅映像，跨单元引用留在占位槽加导入记录里，由加载期链接器解析。本页按顺序列出各阶段，并解释其中一趟（`ResolveStructClassRefs`）为何必须单独执行。

```text
AST → VmBackend::GenerateUnits → 单元映像 (.ncu)，每个翻译单元一幅
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout

执行期：加载器发现并装载闭包（.ncu / .npkg）→ 链接器按限定名
合并 → 唯一的运行期模块
```

### 全树预检查

任何映像开建之前有两项跨单元的预检查——它们横跨多个翻译单元，在逐单元循环内部无法判定：可执行单元集中出现两个`main`候选会被拒绝，包名跨多段的native声明也会被拒绝（宿主动态链接库（DLL，Dynamic Linking Library）按第一个点分段选定）。任一拒绝都会记录日志并使构建失败。

### 逐单元阶段（GenerateUnits）

1. **RegisterBuiltinClasses** — 为内建引用种类建立根类记录
2. **RegisterStructs** — 注册struct类型，解析fieldStructIndices
3. **RegisterClasses** — 注册class类型，解析superClassIdx、fieldClassIndices、fieldStructIndices
4. **ResolveStructClassRefs** — 解析struct的fieldClassIndices与字段类型描述符（RegisterStructs运行时class尚未注册，故在此解析）
5. **RegisterArrayTypes** — 发现并注册本单元用到的数组类型
6. **RegisterEnums** — 收集每个enum的值名表
7. **RegisterFunctions** — 注册函数签名
8. **PopulateClassMethods** — 将类方法与构造器映射到函数索引
9. **PopulateStructMethods** — struct构造器的对应趟
10. **RegisterInterfaces** — 建立接口方法表；表索引的是刚创建的函数记录，因此必须先于字节码生成完成——函数体发射可能追加占位函数记录，表索引必须留在本单元区段内
11. **GenerateAllBytecode** — 为所有函数生成字节码

循环结束后，入口收集把程序入口路由到唯一持有它的单元映像。

### ResolveStructClassRefs为何单独一趟

struct可以包含class类型的字段，但`RegisterStructs`先于`RegisterClasses`执行，class索引此时还不可分配。解决办法：RegisterStructs阶段把字段的类型语法树节点按平行列表存入`m_structFieldTypes`，RegisterClasses之后的单独一趟用`ClassSlotFor`解析class类型字段——它同时为跨单元字段类型分配占位记录——并在struct与class两张表都齐备后为每个字段构建类型描述符。
