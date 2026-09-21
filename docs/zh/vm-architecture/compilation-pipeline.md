# 编译管线

编译器前端产出 AST 后，`VmBackend` 把它翻译为 `CompiledModule`：先
注册 struct、class 与函数，再为所有函数生成字节码。本页按顺序列出
各阶段，并解释其中一趟（`ResolveStructClassRefs`）为何必须单独执行。

```text
AST → VmBackend → CompiledModule (.nmod)
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout
```

### 编译阶段（GenerateStatements）

1. **RegisterStructs** — 注册 struct 类型，解析 fieldStructIndices
2. **RegisterClasses** — 注册 class 类型，解析 superClassIdx、
   fieldClassIndices、fieldStructIndices
3. **ResolveStructClassRefs** — 解析 struct 的 fieldClassIndices
   （之所以推迟，是因为 RegisterStructs 阶段 class 尚未注册）
4. **RegisterFunctions** — 注册函数签名
5. **PopulateClassMethods** — 将类方法映射到函数索引
6. **GenerateAllBytecode** — 为所有函数生成字节码

### ResolveStructClassRefs 为何单独一趟

struct 可以包含 class 类型的字段，但 `RegisterStructs` 先于
`RegisterClasses` 执行，此时 `FindClass` 返回 -1，因为 class 还没有
注册。解决办法：RegisterStructs 阶段先把类型名存入
`m_structFieldTypeNames`，在 RegisterClasses 之后的单独一趟里再解析
class 索引。
