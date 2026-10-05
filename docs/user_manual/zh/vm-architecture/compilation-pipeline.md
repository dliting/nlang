# 编译管线

编译器前端产出抽象语法树（AST，abstract syntax tree）后，`VmBackend`按翻译单元把它翻译为独立的单元映像（`CompiledModule`）：先注册struct、class与函数，再为所有函数生成字节码。每个源文件一幅映像，跨单元引用留在导入槽里，由加载期链接器解析。本页按顺序列出各阶段，并解释其中一趟（`ResolveStructClassRefs`）为何必须单独执行。

```text
AST → VmBackend → 单元映像 (.ncu)，每个翻译单元一幅
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout

执行期：加载器发现并装载闭包（.ncu / .npkg）→ 链接器按限定名
合并 → 唯一的运行期模块
```

### 编译阶段（GenerateStatements）

1. **RegisterStructs** — 注册struct类型，解析fieldStructIndices
2. **RegisterClasses** — 注册class类型，解析superClassIdx、fieldClassIndices、fieldStructIndices
3. **ResolveStructClassRefs** — 解析struct的fieldClassIndices（RegisterStructs运行时class尚未注册，故在此解析）
4. **RegisterFunctions** — 注册函数签名
5. **PopulateClassMethods** — 将类方法映射到函数索引
6. **GenerateAllBytecode** — 为所有函数生成字节码

### ResolveStructClassRefs为何单独一趟

struct可以包含class类型的字段，但`RegisterStructs`先于`RegisterClasses`执行，此时`FindClass`返回 -1，因为class还没有注册。解决办法：RegisterStructs阶段先把类型名存入`m_structFieldTypeNames`，在RegisterClasses之后的单独一趟里再解析class索引。
