# v0.14.7后 master同步合并概要设计

## 一、背景与目标

master线在tensor-simd基座（7e5ea0ba）之后演进了310个提交（+81778/−8619行），核心是**引用形参线**：`&`形参全面替换并退役out参数（resolver迁移诊断→out溢出后端机械拆除→out操作码移除→NCU v2.22→手册"reference semantics across the specification"全规格重写），外加门禁工具链升级（test_impact.py、tier_coverage_check.py、守卫自测）与K7冻结测试的结构化改造。

tensor-simd线同期交付0.14.1-0.14.7（106提交）：设备执行栈六版+稳定排序+索引散布补全。

切片左值（下一特性）的实现恰好落在master重写过的赋值降低面（StatementResolverAssign/EmitStmtStore），在旧基座上实现等于保证冲突返工。目标：master并入tensor-simd，单线承载后续特性，例行门禁全绿。

## 二、事实盘点

- 总面：master改1145文件，tensor-simd改……交集296文件；compiler/vm/include交集约100文件（全resolver/emitter面、语法、BytecodeOps/OpCodeTable/Disassembler、ModuleSaver/Loader）。
- opcode：master删out三元组（OP_CallStaticOut/OP_CallMethodDirectOut/OP_CallDelegateOut）、增位运算族（基座7e5ea0ba无位运算——master经dev吸收线引入）；tensor-simd增OP_Box_TypeArg/OP_Unbox_TypeArg（泛型装箱）。两侧均改动OP_Count附近的枚举尾部。
- 线格式：master NCU v2.19→**22**；tensor-simd停在19。
- NativeHost.h：两侧双改（master的宿主ABI演进 vs tensor-simd的v14设备钩子）——**最关键ABI文件**，逐块并集。
- 语法（nlang.y +196/−31）：tensor-simd的tensor赋值语句产生式（0.14.2花式索引赋值）vs master的引用形参产生式+out产生式删除。
- stdlib：master完成out→&迁移（io.n等）；tensor.n为分支独有文件（master无），原生注册C++ ABI无out形态——张量面无out依赖。
- 手册：master"reference semantics across the specification"全规格重写（18-tensor.md在master有276行差异）vs tensor-simd的0.14.x条目编辑；en/zh成对。
- 测试：master的manifest增量+refbind族+迁移后的语言语料；tensor-simd的0.14.x钉与manifest增量。stdlib_net_tcp_concurrent_writer双线共有。
- CHANGELOG/VERSION/roadmap/SYNC_BASELINE：序列交错。
- 工具：守卫（master加自测+改动 vs tensor-simd的5行声明窗口）、probe_run.py（分支的--backend）、test_impact/tier_coverage（master新增）。

## 三、方向与时机

- **方向**：tensor-simd树执行`git fetch E:/cases/nlang/master master && git merge FETCH_HEAD`。先例：v0137的"交付线并入master线"方向；本地路径取数，不触碰master树。
- **时机**：tensor-simd工作树干净（0.14.7已收口，554b987c+2e6ee82e落地）✓。
- **执行者**：本会话（用户裁决"本会话先合并master"）。

## 四、冲突面分类与裁决

1. **opcode表**（BytecodeOps.h/OpCodeTable.cpp/Disassembler.cpp/VM switch）：以master为基（out退役是不可逆语言裁决，位运算族在位），tensor-simd的Box/Unbox_TypeArg接排在master现有尾部之后；NCU取**22**（循v0137"取高值"裁决；分支+2操作码不单独立版本）。OpCodeTable/Disassembler/ndisasm同步，e2e全量兜底。
2. **语法**（nlang.y/nlang.l）：并集。master＝引用形参产生式+out产生式删除；tensor-simd＝tensor赋值语句产生式。bison冲突告警与合并前master基线一致（零新增）。
3. **resolver/emitter**：master重写调用与store面（out退役）；tensor-simd的StatementResolverTensorAssign.cpp为新增文件（无冲突），EmitStmtStore/WalkersStmt/WalkersMaxArgs的修改逐文件人工判读——master语义完整一侧为基，tensor-simid的tensor赋值语义并入。
4. **NativeHost.h**：逐块并集——tensor-simd的v14设备钩子族（TensorDeviceStatus/TensorDeviceHooks/kSortMaxLine时代的新槽）+master的宿主ABI演进；两侧增量互不覆盖。
5. **手册**：specification页以master重写为基（引用语义是语言裁决），tensor-simd的18-tensor等页增量并集；en/zh成对；合并提交后SYNC_BASELINE重锚。
6. **CHANGELOG/VERSION**：master序列为基，tensor-simd的0.14.1-0.14.7条目按版本号降序插入原样保留；VERSION取0.14.7（分支交付线）。
7. **stdlib**：master的迁移基；tensor.n原样；构建重打包。
8. **测试面**：manifest并集；两侧新增测试全保留；master的门禁工具链（test_impact/tier_coverage/refbind族）为准；tensor-simd的钉族在新格式下全量重验。
9. **工具**：守卫=master自测版+tensor-simd的5行窗口（语义并集）；probe_run.py保留分支的--backend；tier工具链原样。
10. **ROADMAP**：master条目+tensor-simd的0.14.x交付条目并集。

## 五、验证与收尾

- 合并后全净重建三树（bench/dbg/asan——**两DLL同批**）。
- 门禁四腿+mnist护栏（master口径的工具链）；source_size_guard与符号pin重数（master迁移后的stdlib签名数可能变化——重数为准）。
- 0.14.x钉族全量复验（新语法/新格式下）。
- 收尾提交（合并提交本身）；SYNC_BASELINE重锚；roadmap/记忆更新。
