# Phase 3a 详细设计 — stdlib/*.n 成为签名权威，kStdLibTable 精简为实现表

## 目标数据流

```
stdlib/*.n (权威源: 签名 + 文档)
   └─ SymbolIndex (langservice)
         └─ TypeKind 化的签名
               ├─ compiler ExprResolverStdLib: 参数类型检查 / 结果绑定
               └─ vm backend EmitStdLibCall: coerce / 返回赋值

kStdLibTable (精简: ns, name, intrinsicId)
   └─ vm backend: native 实现分发 (OP_CallIntrinsic)
```

## TDD 步骤

- A: langservice 增加 enum TypeKind {Unknown,Any,Int,Float,String,ListString}
  + 转换函数；ParamInfo/SymbolInfo 带 TypeKind。测试: print 参数 Any、
  sqrt 返回 Float、listFiles 返回 ListString。
- B: BuildParams 加 m_sStdLibDir；BuildEnvironment 持有并加载 SymbolIndex；
  compiler 链接 langservice。测试: 加载真实 stdlib=38；空路径降级=0。
- C: ExprResolverStdLib 用 SymbolInfo 签名（TypeKind→NodeKind），intrinsicId
  仍查表。测试: 真实编译，正确通过/错误被拒。
- D: EmitStdLibCall 用 SymbolInfo（Any=coerce、returnKind 决定赋值）+
  表的 intrinsicId。测试: 真实编译+运行（e2e）。
- E: StdLibEntry 只留 ns/name/intrinsicId；更新 static_assert；删除 Phase 1
  生成器，把 stdlib_generation 测试改写为".n native 函数都有 intrinsicId"。
  全量 ctest。

## 降级与边界

- stdlib 目录由 ncc/nide 用 FindStdLibDir(applicationDirPath) 定位后经
  BuildParams 传入；compiler 不自己定位（可测试、职责清晰）。
- 找不到 stdlib 且源码引用了库函数: 明确编译报错（不静默回退），方向对齐
  "stdlib 就是普通模块"。
- string 方法表 kStringMethodTable 本阶段不动（receiver-dispatched）。
