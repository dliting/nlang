# 已知限制调研：用户角度优先级 × 可解决性

> 2026-10-16。范围：`docs/user_manual/zh/specification/reference/known-limitations.md`（44 条）
> 与 `docs/user_manual/zh/vm-architecture/known-limitations.md`（5 条）。
> 评估维度：**用户影响**（踩坑频率 × 有无干净变通 × 静默错误还是明确报错）
> 与**可解决性**（工程量、设计路径清晰度、是否与架构冲突）。
> 本文是可复用的调研排序（与 `roadmap.md` 同类的开发侧文档，不进用户手册），
> 每完成一项后更新其裁定与状态记录。
>
> 2026-11-01：条目编号 #NN 指合并前旧页顺序；2026-10-31 该页已并入规范参考页并按 en 章节序重组。

## 评估原则

1. **静默错误 > 明确报错**：明确报错只是不便（有变通即可忍受）；静默算错数是正确性
   灾难（用户无从察觉程序已错）。静默类限制无论频率都应最高优先。
2. **变通干净度**：变通是「换一种写法」（可忍）还是「写一堆样板」（不可忍）。
3. **设计决策不是缺陷**：有意规格缩窄（如 string→数值 `as` 禁止）不进解决清单。
4. **架构级改动单独归类**：需 VM 布局/序列化格式/泛型系统的是长期项，不与小修复混排。

## 第一梯队：静默正确性风险，可单点修复

### P1 覆写签名不匹配无诊断（language-spec #33）——✅ 已解决（2026-10-17）

子类方法与父类虚方法**同名但参数或返回类型不同**时，不产生任何编译错误或警告——
同名方法仍占据虚分派槽位：调用点按父类签名编译并传参，运行期分派进子类方法，
实参与返回值**按位错读**。

- 用户角度：**全手册唯一一条静默数据错读级限制**。面向对象用户必踩（拼写父类
  签名时差一个参数/类型即触发），且程序继续运行输出错误结果，无从排查。
- 可解决性：**高**。同名方法与虚槽位分配对 resolver 可见；加一条「子类方法与父类
  虚方法同名但签名不同」的编译期检查即可。纯诊断，不改任何运行期语义。
- 既有事实：手册明载「覆写必须严格同名、同参数、同返回类型」——即语义契约已在，
  缺的只是机器强制。用户已裁定**不需要向下兼容**（可直接定为编译错误）。
- **状态（2026-10-17）**：已实现并合入 dev（VERSION 0.9.4，提交链 2a5aded..04d1c40
  共 15 提交，未 push）。规则升级为同名集双射（契约名下禁重载）＋接口按签名校验＋
  Object 协议硬编码表；34 单测＋27 e2e 条目；终局审查 APPROVED。原 #33 已从
  known-limitations 删除。**未 push、未同步 master、未发布**。

### P2 嵌套下标接收者写求值覆写（language-spec #27）——✅ 不复现，条目已移除（2026-10-17）

`matrix[i][0].x = v`（数组的数组的 struct）中，接收者内层下标索引的求值可能覆写
右值临时槽位。单层 `arr[i].field = v` 正常。

- 用户角度：静默算错；单层形态正常使用户误以为嵌套也正常。
- 可解决性：**高**。temp 槽覆写家族已有成熟解法先例：嵌套二元表达式 temp 冲突
  （Phase 8e-1 扩 4 槽修复）、callParamBase 覆写（EvalAreaClaim RAII）、
  observedPeakCursor tripwire。本条是同一族的赋值语句接收者路径残余。
- **状态（2026-10-17）**：**六形态探针全部不复现**（`List<Point[]>` 链／数组 of
  List 链／struct 数组字段链／整元素链写／调用型索引＋调用型右值＋嵌套字段最大
  temp 压力形态；真 `T[][]` 文法不可表达——直接与别名形态均被拒）。条目已从两树
  known-limitations 移除（a1cc65e，探针 temp/p2_probe_*.n）。推断被 temp 槽
  claim 化（PickTempSlot 删除）系统性修复时顺带消除。

## 第二梯队：高频日常卡坑，设计路径清晰

### P3 构造调用不适用默认参数值（language-spec #18）——已结清（2026-10-18，v0.9.6）

~~`Vec(int a, int b = 3)` 下 `Vec(5)` 报实参数目错误。~~

构造调用与 `super()` 已应用形参默认值（`BuildCtorBindings` + `EmitBinding`
共享下发；struct 实参顺带统一为深拷贝；arity 诊断改区间制）。随附结清：
language-spec 已知限制 #18 条目、class.md 同源条目已删。测试
ctor_default / ctor_default_formal_ref / ctor_default_nested /
ctor_struct_arg_copy / super_default / ctor_default_missing_required。

### ~~P4 右移/左移运算符未实现（language-spec #12）~~

0.9.9 已实现 `<<`/`>>`（左操作数定结果、计数须隐式可转换到载体（Java/C#
规则，宽/跨符号计数编译拒绝）、计数运行期范围检查、enum 走 int32 载体、
跨模块默认参数子集内）。既有 e2e 钉 `rshift_not_generic` 反转为正向钉
（depth-0 `>>`＝位移）。随附结清：known-limitations.md 双树 #12 条目已删、
operators.md 双树加「位移」节、bytecode-instructions 双树加 OP_Shl/OP_Shr。
测试 shift_basic / shift_types / shift_count_error_throws /
shift_count_negative_throws / shift_precedence /
shift_reject_{float,double,char,bool,string,null} /
shift_reject_count_{long,uint} / shift_generic_regression /
cross_mod_default_shift。

## 第三梯队：推断角落收敛——✅ 已解决（2026-10-22，0.10.8）

一族共享同一变通（「先把字面量绑定到带类型局部」）的推断限制，已一批收窄
（0.10.8 D1–D4）：

- 裸集合字面量不能作 foreach 源（#22）——D3：花括号形式源按循环变量类型
  推断 `List<T>`（方括号形式源此前已支持）
- 数组 return 位裸表被拒绝（#21）——实际 0.9.13 已落地（探针 p03/p04
  佐证），账本此前未更新
- 裸初始化列表作命名实参不推断（#13）——实际 0.9.13 已落地（探针 p05/p06
  佐证），账本此前未更新
- 非推断位上的裸 `{...}`（#10）——D2：无目标位的裸方括号字面量按条目推断
  元素类型；裸花括号仍拒绝，逃生通道为锚定形式（D1）
- 泛型实例化没有锚定头（#19）——D1：锚定泛型字面量（`List<int>{1, 2}`）；
  嵌套泛型声明（`Channel<Channel<int>>`）经权威判定谓词单源化同时解锁
- 裸 `[]` 空初始化、花括号内嵌裸花括号（#11/#20）——D4：键值条目的花括号
  值接受；无目标空 `[]` 仍拒绝（无元素类型可推断，已入文档）

LALR 冲突台账 27/4 → 25/3（`OT_TypeLt` 专用记号退役旧 `'<'` 冲突族）。
本批次探针新浮现潜伏缺陷 p23（见「新 surfaced 候选」节）。

## 声明符语法角落——✅ 已解决（2026-10-22，0.10.7）

`int a, b[3]`（#14）、`Vec a, b(2)`（#15）、混合声明符继承数组类型（#16）、
定长声明花括号形式（#17）——「首声明符专属臂」的既有文法决策残余已在
0.10.7 消除：续接声明符接受首声明符全部形态（D1）、按声明符独立定类型
（D2）、定长声明花括号形式改具名诊断（D3）。LALR 冲突台账 23/4 → 27/4
（新增冲突均为预期新臂）。

## 新 surfaced 候选（2026-10-22 0.10.8 批次副产物，待用户裁定是否立项/排序）

- **p23 集合初始化器条目键形态与目标种类不匹配被静默接受**（0.10.8 批次
  探针浮现；非本批引入——0.10.8 提交链未触碰条目处理路径）。四种形态均
  编译运行成功、零诊断：
  - `List` 目标＋键值条目：`List<int> l = {x: 1, y: 2};` 列表实收元素
    `1`/`2`（探针 p23c 退出码 12），键被静默丢弃；字符串键（`{"a": 1}`，
    探针 fam9 退出码 21）与锚定/foreach/命名实参形态同族。
  - `Dict` 目标＋无键条目：`Dict<string, int> d = {1, 2};` 条目在代码
    生成处被静默跳过（探针 fam5：`count()` 为 0）；混合 `{"a":1, 2}` 只
    收首条目（`count()` 为 1）。
  - `Dict` 目标＋标识符键：`Dict<string, int> d = {a: 1};` 键被静默当作
    拼写字符串 `"a"`——手册与代码生成注释本已约定仅允许字符串字面量键。
  - struct 目标＋字符串字面量键：`S s = {"a": 1};` 键被忽略、按字段声明
    序位置填充（探针 fam8 退出码 12）。
  静默正确性类。解决方向：目标判定处对与目标种类不匹配的条目形态报具名
  诊断拒绝。
  > **→立项＋已解决（2026-10-23，0.10.9，用户 standing 指令「按同样流程
  > 自主处理下一批已知缺陷」）**：解析器单点门 `RejectInitListEntryKeyForm`
  > （`PrepareInitListShape` 形态门组内调用；`!bIsArray` 守卫排除数组目标
  > ——元素类型为 `Dict`/`List` 的数组其条目形态由数组自身决定，正向钉
  > `init_dict_element_array`）；`Dict` 键类型门同批前移至该处（键类型
  > 诊断比形态诊断更根本——给标识符键加引号不能挽救非 string 键类型）；
  > 代码生成侧同步收口（`EmitInitListEntrySet` 的键检查依解析器保证收敛，
  > 注释具名）。四类不匹配各报具名诊断（文案见手册 collection-initializers
  > 双语页新增「条目形式必须与目标种类匹配」节）；11 条 e2e（9 拒绝＋
  > struct 混合条目与数组 of Dict 两条正向钉）。门位于
  > `src/compiler/builder/ExprResolverInitListForm.cpp`（source-size 拆分
  > 自 `ExprResolverInitList.cpp`）。spec 见
  > docs/superpowers/specs/2026-10-22-init-list-entry-key-form-design.md。

## 长期项（架构级，不与上述混排）

| 限制 | 阻挡点 |
|------|--------|
| 用户自定义泛型（#4/语言 #7） | 泛型系统整体设计；路线图远期已有规划 |
| lambda/闭包（#38） | 函数值捕获语义；路线图远期 |
| 跨模块默认参数可折叠限制 + 具名实参不支持（#29/#30） | ~~`.ncu` 签名格式扩展（现不携带形参名/复杂默认值），跨版本兼容大工程~~ ✅ **已解决（2026-10-17，缺陷②批次）**：`.ncu` 2.1→2.3（2.2 携带形参名与 `func` 签名，2.3 携带默认值表达式），函数值/具名实参/复杂默认参数（序列化表达式子集，逐调用点求值）全部对齐源码直读路径；子集外默认形态（转换/成员访问/嵌套调用）保留消费侧拒绝（边界文档移至 common-errors）。残留新条目见文末 surfaced 候选（类桩成员不可达） |
| 锯齿数组 `T[][]`（#9） | VM 无多维数组布局；`List<List<T>>` 变通干净 |
| `List<struct>` 装箱引用语义（#26） | 装箱语义重设计；行为与变通已文档化（known-limitations 双语 71e5e1db、§12.1/§4.7.4）。**2026-11-07 用户裁定：确认为缺陷，待修复**。七探针实证（temp/struct-alias-probe/，d:/dev/nlang 部署二进制）：struct 元素进出容器全深拷贝（无 COW，探针 1/2/3 全独立），而 `contains`/`indexOf`/Dict 键按记录身份比较——值相等元素永远匹配不上（探针 4）；强形态：`contains(pts[0])` 容器自吐句柄亦不匹配（实参在形参位同被深拷贝，探针 6），即 struct-T 容器上不存在任何可成功的匹配写法；类元素同形态正常（对照探针 5，exit 1）；`Dict<struct,V>` 值等键 `containsKey`=false（探针 7，`DictKeysEqual` RTK_Struct 身份臂）。修复方向＝struct 元素/键改字段深值比较（与 string 内容比较、标量值位比较同哲学；深比较沿「值字段深比较、类字段按引用比较」——与拷贝语义对偶），同步 `DictKeysEqual` 的 RTK_Struct 臂与 `indexOf`/`contains`/`containsKey` 三入口 |
| `List<int>` 装箱开销（vm #4） | 优化项非限制修复；`IntList` 特化已给变通 |
| `List<func>.contains` 恒等比较（#39） | 需先定义函数值相等语义（设计决策） |
| 可变静态字段／模块级状态（非既有条目，2026-10-18 用户问询后记档） | **route 外，未立项**：路线图远期特性无此条目，`state` 关键字保留未用。若实施需独立设计——单元产物静态数据段（`.ncu` 格式版本）、链接器槽位重映射、加载期初始化顺序（每单元一次）、持有堆引用的静态槽进入 GC 根集、调试器展示、继承/隐藏查找规则。设计取向建议：以「命名空间级模块状态」为本原、类静态字段为语法糖。**零存储子集已另行覆盖**：接口 `static const` 编译期常量（0.9.10 Task 11），同一折叠机制可低成本平移到类常量，不涉及运行期静态存储 |

## 有意设计，不解决

| 限制 | 依据 |
|------|------|
| string→数值 `as` 被禁止（#5） | 规格缩窄非缺陷；string 方法族（toInt 等）覆盖 |
| struct 没有方法（vm #2） | struct＝纯数据的设计定位；要行为用 class |
| 退出码 8 位观察（vm #1） | 平台限制文档化；测试约定 ≤255 |
| 64 参数上限（#34） | 合理性上限，有理有据 |
| finally 无控制流（#35-36） | 语义保守正确（不得吞在途控制流/异常） |
| 接口默认参数/默认值不继承/super 只达父类（#31/#32/#37） | 语义简化决策，变通清晰。**事实修正（2026-10-17 实测）**：#31 原文「接口默认值不解析、须提供全部实参」有误——接口声明默认值会按调用点静态类型生效填充（实现方法默认值不跨接口边界）；known-limitations 措辞已在 0.9.4 批中改写为验证过的规则 |

## 裁定记录

- 2026-10-16：用户裁定从 **P1（覆写签名诊断）** 开始设计实现（superpowers 流程）；
  **不需要向下兼容**（诊断可直接定为编译错误）。
- 2026-10-17：**P1 完成**（dev 15 提交，0.9.4，门禁全绿，终局审查通过；未 push/
  未同步 master/未发布）。同日 **P2 探针裁定不复现**（条目移除 a1cc65e）。
- 2026-10-17：**用户指令（离开期间自主执行）**：superpowers 标准流程解决两个手册
  缺陷——①成员链 `s.length().toString()` 致 ncc 崩溃；②跨模块函数值／复杂默认
  参数／具名实参被拒（#29/#30 及函数值跨模块）。
- 2026-10-17：**缺陷①核实为陈旧条目并移除（8d81a3d）**：崩溃在 0.8.4 已修复
  （changelog 明载 `a.length().toString()` 例；错误路径孪生形态 0.8.5 修复），
  FAQ 条目写于 0.8.3 时代未随修复撤销。当前 dev＋0.9.3 双二进制探针九形态
  （深链/左值链/语句位/条件位/错误路径）全部编译通过或具名诊断。
- 2026-10-17：**缺陷②批次完成（dev 10 提交 6582f55..83877cd，VERSION 0.9.5，
  门禁全绿）**：Task 1 具名实参（6582f55，桩真名）→ Task 2 函数值（38a185c，
  TypeDesc 携带 `func` 签名，`.ncu` 2.2）→ Task 3 复杂默认参数（34bb6e3，
  序列化表达式子集，`.ncu` 2.3，逐调用点求值保持）→ 测试加固（7beadc1/7ba6b65/
  0d527ad）→ 根修三连（14aa3ca UniquePtrCollection 移动移交删除器、13ade9d
  导入门策略单点化 ImportGateAllows、8f270e1 ExprDesc 防漂移守卫）→ Task 4
  文档收口（83877cd：known-limitations 双树删三条＋function-types 删跨模块
  bullet＋FAQ 删陈旧节＋common-errors 改写为子集边界＋module-serialization
  版本陈述修正＋CHANGELOG 双语＋VERSION）。known-limitations 新增「导入类成员
  不可达」条目（本批实测确认的真实残留，见文末 surfaced 候选）。门禁：构建
  零告警、ctest 107/107、e2e 1206/0（3 既有 warning）、docs 严格绿。
  未 push、未同步 master、未发布。
- 2026-10-20：**缺陷 #1（嵌套 struct 局部乘法「244」）裁定为 bash 8 位退出码
  观测假象，无代码缺陷**：Python 32 位全值复测五探针全对（`p30m9_mul_only` /
  `p30q_local_then_mul` / `p30p_field_order`＝500、`p30r_add_const`＝105、
  `p30s_local_copy`＝5），反汇编 `mul<int> 0 48` 操作数槽正确、`OpMul` 无 struct
  相关分支；原「已钉边界」四形态（5／105／5／244）恰因 5 与 105 落 8 位、500
  越界被误读。对应 surfaced 候选条目已标 ✅ 观测假象，无需代码修复。
- 2026-10-20：**0.9.11 类型初始化现代化计划完成（dev 线）**：T1–T9 已提交
  （class 构造器去伪返回类型、struct 构造器、class/struct 字段初始化器、
  数组 `[...]` 收敛、重复 main 专用诊断、构造器重载、外部 .ncu 构造器＋NCU
  v2.6），T10 全量验证通过（ctest 109、e2e 1361/0、bison 23 s/r＋4 r/r、
  docs 严格绿、打包 0.9.11 全 PASS）。master 已裁定不同步（master 正开发
  并发功能，未完成、改动面大）。
- 2026-10-21：**P0 销账＋0.9.12 立项（superpowers 设计进行中）**：
  ①「混合布局同名静态/实例对错链返回垃圾 231」经 Python 全值复测裁定为
  bash 8 位退出码观测假象（全值＝999，实例方法正确返回；声明顺序与 virtual
  修饰无关），缺陷不存在——手册「混合导入布局」条目错链描述失实待修正，
  补正向回归钉。②密封布局事实矩阵复测钉死：实例字段读/实例方法调用编译期
  不可达（`imported_class_field_read`/`imported_class_method_call` 两钉成立）；
  消费者子类化导入基类已可编译可运行（名称派发），错误覆写签名与导入接口
  错误实现均静默通过（契约检查空转，危害前提已随 0.9.11 外部构造失效）。
  ③新发现：模块限定静态调用 `lib.Type.f()` 两种布局均编译失败（裸名正常），
  未文档化。④探针方法修正：密封探针须把生产方 `.n` 移出 `-I` 顶层目录，
  否则源码内联掩盖桩路径（本批初版假绿教训）。裁定：0.9.12 范围＝P1a（导入
  类实例字段/方法可达）＋P1b（跨模块基类/接口契约检查激活）＋模块限定静态
  调用＋P0 销账，设计走 superpowers 全流程。
- 2026-10-21：**0.9.12 实施完成（跨模块实例成员＋成员访问现代化）**：五提交
  a415f86（字段读取改走 VmExecutor 持有的类布局视图）＋d64b5d8（NCU v2.7：
  类记录上线携带完整成员面——自有字段、实例方法、构造器、接口实例签名；
  saver/loader 严格对称＋旧版本精确拒绝）＋6851a0e（成员访问现代化：struct/
  enum 成员默认 public、struct 成员仅可写 `public` 修饰符、非 public 构造器
  类外不可调用、可见性规则跨模块一致）＋740deaa（消费侧解析导入类实例字段/
  方法：自有＋继承字段、默认参数、重载、虚分派；模块限定静态调用 `lib.Type.f()`
  修复，模块提示由前缀匹配改精确匹配）＋ca44f13（跨模块 override/implements
  契约检查激活，导入桩与本地属主同等参与契约遍历）。P1a、P1b、模块限定静态
  调用、P0 销账（bash 8 位退出码观测假象，补 999 全值正向钉）全部完成；
  原密封布局两拒绝钉 `imported_class_field_read`/`imported_class_method_call`
  转为正向运行钉（21/42）。门禁：ctest 110、e2e 1407/0（3 条既有 WARN）。
  残余限制已写入用户手册 known-limitations 双语：导入 struct 实例字段不可
  访问、枚举不能跨模块导入、跨模块同参数数重载链接期报显式歧义。T6 文档/
  VERSION 与 T7 最终全量验证（全量构建＋bison 台账＋打包）进行中。
  master 不同步裁定不变。
- 2026-10-24：**0.10.14 跨模块导入完善完成（0.9.12 三条残余边界全部消除）**：
  ①导入 struct 实例字段跨模块可读写（含锚定花括号按名填充 `lib.Vec v =
  {x: 1, y: 2};`）；②enum 成为完整模块成员——类型、枚举值、成员方法、
  `toString` 经包名全可达（`lib.Color.Red`、`lib.Color.Blue.rank()`），
  消费者经 `CreateEnumStub` 铸造枚举桩（值成员＋方法成员，走既有
  `FindModuleType` 通道绑定）；③跨模块同名同参数数不同参数类型重载按类型
  精确链接（调用点类型选择传到链接器，每处调用连被选中重载自己的记录），
  链接期显式歧义消除。统一机制＝「类型记录携带自有成员函数索引」的第四个
  实例（class methodIndices / struct constructorIndices / interface
  staticMethodIndices / enum methodIndices）。`.ncu` 小版本 2.8 → 2.9
  （enum 成员方法下标＋访问级字节两张平行表，`ModuleSaver`/`ModuleLoader`
  枚举段追加、`NcuLinker` 合并重映射同趟纳入），全部 `.ncu` 须随版本重编。
  新增链接诊断 `imported function '...' does not match its record in the
  owning unit`（生产方重编致表位移动而导入方产物陈旧时报告而非错链，处置
  重编导入方）。门禁：构建零告警、ctest 113/113、e2e 1521/0、
  source_size_guard 绿、docs 严格绿。提交链 07ccb7ad 起（Task 1–5）。
  用户手册双语同步：known-limitations 删三条、class/enum/struct/
  overload-resolution/packages/common-errors 补规则、module-serialization
  版本陈述修正 2.8→2.9。未 push、未同步 master、未发布。
- 2026-11-07：**#26（`List<struct>` 装箱引用语义）经用户问询-实验确认为缺陷，
  裁定在案、待修复**：博客文稿核校引发的核查链（泛型实参双轨 → struct 值表示
  → 容器边界语义）以七探针钉死（temp/struct-alias-probe/，部署二进制）：
  struct 值在帧槽/容器存储里均为堆记录句柄，值语义靠每个变量绑定/赋值/传参
  边界的 `OP_CopyStruct` 深拷贝实现（无 COW）；`add(p)` 后改源不变元素、
  通过容器改元素不泄漏回源、读出副本改之不变元素（探针 1/2/3，全独立）；
  `contains(p)` 在 `add(p)` 后返回 false（探针 4）——身份比较（
  `DictKeysEqual` RTK_Struct 臂＝句柄恒等）×边界深拷贝的组合，使值相等元素
  永远匹配不上；强形态 `contains(pts[0])` 亦不匹配（探针 6：实参在形参位同被
  深拷贝，任何用户可写表达式都无法命中）；类元素同形态 `contains` 正常
  （探针 5 对照，exit 1——类不拷贝，句柄即对象）；`Dict<struct,V>` 值等键
  `containsKey`=false（探针 7）。行为现状已于 known-limitations 双语（zh:28 /
  en:80，71e5e1db 校正）与 §12.1/§4.7.4 文档化，本轮把实证与用户裁定回填
  长期项表 #26 行；修复方向（struct 元素/键字段深值比较，三入口同步）一并
  记档。探针留存供复核。

## 新 surfaced 候选（2026-10-17 P1 批次副产物，待用户裁定是否立项/排序）

- **含虚方法同名族的静默错分派**：族{`int f(string)`, `int f(int)`（虚）}中 `f(5)`
  按声明序落到 `f(string)`——错体＋实参按位错读＋零诊断（双二进制实测钉死，
  0.9.3 起既有）。属本文件「静默正确性」最高优先级类；解法方向＝解析器把精确匹配
  提为无条件优先（或含虚族禁隐式转换绑定）。已文档化于 known-limitations。
  > **立项（2026-10-18，0.9.7）→已解决（2026-10-18，v0.9.7，提交 77db937＋723f06f）**：
  > CheckVirtualNameUniformity 声明点拒绝（同类内含虚名字一名字一签名）；
  > 六 e2e（4 拒绝形态＋2 合法回归钉）；用户手册双树 known-limitations 条目
  > 已删，common-errors/class.md 双树已补规则。以下根因勘误保留：
  > 九探针＋代码研读证明解析器排序本来就
  > 正确（p3/p4/p9），原「解法方向」基于解析器塌缩假设不成立。真根因＝虚分派通道
  > 按名键控：`EmitMemberMethodDispatch` 对虚成员只发射方法名，运行期
  > `FindMethodByName` 取首个同名声明，签名丢弃。解法改为**声明点拒绝**（含虚名字
  > 类内一名字一签名，扩展 0.9.4 契约检查）；spec 见
  > docs/superpowers/specs/2026-10-18-virtual-name-overload-diagnostic-design.md。
- **导入类实例成员跨模块不可达／混合布局同名错链（缺陷②批次副产物，2026-10-17；2026-10-19 探针实测刷新）**：
  **纯`.ncu`密封布局**：桩不携带实例成员——字段读取（`w.w`）与实例方法调用（`w.doubled()`）编译期报「不存在/无法解析」；经桩构造（`Mix()`）报后端内部错误（"constructing imported class ... not wired in per-unit mode yet"）；0.9.10 起桩携带 public 静态方法与接口常量（Task 10/11），类名可作类型使用、生产方函数返回的实例可接收。**明确报错类**，已文档化于 known-limitations 双语。
  **混合布局（源码与`.ncu`并存于导入路径）**：实例方法调用与字段读取经源码符号编译通过且**运行期正常**（探针：`Factory.make().f()`＝7、`m.v`＝42）——唯**同名静态/实例方法对**例外：类同时声明 `static int f(int)` 与 `int f()` 时，消费端实例调用运行期错链返回垃圾 231（lib4 探针，与 0.9.5 基线二进制逐位一致＝既有缺陷，非 0.9.10 引入）。**静默正确性类（最高优先级类）**，候选立项：`.ncu` 链接层实例方法键控缺静态性维度（或消费端源可见符号发射了错误目标）。
  **探针纪律修正**：原条目「同目录 lib.n 会被内联为源码绕过桩路径」不准确——混合布局下编译期经源码解析、运行期仍加载`.ncu`（源-only 导入报 Module not found，ncc 从不自动编译源导入）；「源码在场不受影响」在同名对场景被推翻。密封布局＝e2e runner 做法（`_imports_tmp` 只放 `.ncu`）属实。**测试钉（2026-10-20 销账）**：密封布局两存活拒绝形态已补 compile_error e2e——字段读 `imported_class_field_read`、方法调用 `imported_class_method_call`；第三形态「桩构造」经 0.9.11 T5 已不再是拒绝形态（外部 `.ncu` 构造成功，正向钉 `cross_mod_class_ctor`）。混合布局同名错链仍待钉。
  > **刷新（2026-10-21 实测裁定）**：「混合布局同名静态/实例对运行期错链返回垃圾 231」**不成立，为观测假象**——Python 32 位全值复测＝**999**（实例方法正确返回值），231＝999 截低 8 位（bash `$?`，与「244」案例同族）；声明顺序、virtual 修饰均不影响。缺陷不存在，用户手册「混合导入布局」限制条目的错链描述失实待修正，改补正向回归钉。**同批新发现**：模块限定静态调用 `lib.Type.f()` 在密封与混合两种布局下均编译失败（`Cannot resolve the field: lib.`＋误导性模块提示，即便 `import lib;` 已在场），裸名 `Type.f()` 正常（e2e `cross_mod_static_class`/`cross_mod_interface_static` 钉），该缺口未见于文档。**探针方法修正**：密封探针必须把生产方 `.n` 移出 `-I` 顶层目录（e2e runner 将其暂存到子目录），否则 ncc 源码内联掩盖 `.ncu` 桩路径——本批复测初版曾因 `.ncu` 与 `lib.n` 同目录而假绿。裁定：P1a＋P1b＋模块限定静态调用＋本条销账并入 **0.9.12**（superpowers 设计进行中）。
  > **→已解决（2026-10-21，0.9.12，提交 a415f86..ca44f13）**：NCU v2.7 类记录携带
  > 完整成员面，导入类的实例字段读取与实例方法调用编译期可解析、运行期正确执行
  > （含自有＋继承字段、默认参数、重载、虚分派，e2e `imported_class_field_read`/
  > `imported_class_method_call`/`imported_method_defaults`/`imported_method_overloads`/
  > `gc_imported_inherited_strings`）；模块限定静态调用 `lib.Type.f()` 同批修复
  > （D4：限定类型引用与裸名走同一记录绑定；模块提示由前缀匹配改精确匹配），
  > 正向钉 `imported_qualified_static`/`imported_bare_static`（999 全值退出码，
  > 同时钉死 bash 8 位截断假象）；混合布局同名错链经 2026-10-21 复测已裁定为
  > 观测假象（见上）。残余边界（导入 struct 实例字段、枚举跨模块、跨模块同参数数
  > 重载）已文档化于用户手册 known-limitations 双语。
- ~~**静态方法函数引用被拒**~~——✅ **已解决（0.9.14 D-new-1，2026-10-22）**：静态方法现经类型名绑定为函数值（含继承静态、接口静态、native 静态、所属类内裸名形态，D-new-2/4/8）；经对象接收者仍拒（"the static method must be called through the type name"）。已文档化于 function-types 限制节双树。
- ~~**接口字段零标志/缺初始化式形态报裸 syntax error**~~——✅ **已解决（0.10.7 D4，2026-10-22）**：InterfaceMember 补三个部件缺失的字段形态文法变体（零标志＋`=`、零标志无初始化式、多标志无`=`），声明契约诊断（"must be static const"、"must be a constant expression"）取代裸语法错误；`iface_field_*` 三条 compile_error 钉住。
- ~~**含嵌套 struct 字段的局部变量所在函数中乘法静默算错**~~——✅ **观测假象，无缺陷（2026-10-20 裁定）**：
  0.9.11 T2 探针批次以 bash `$?` 观测 `return o.t * 100;` 得 244（期望 500），误判为「该局部
  struct 存在时乘法运算读错操作数槽」，244 一度被疑为嵌套 Inner 的堆句柄。**真因＝bash 8 位
  退出码截断**（见「有意设计」vm #1）：`500 = 0x1F4`，`$?` 仅保低 8 位＝`0xF4`＝244。经
  Python（32 位全值，`cmd/PS/Python 见全值`）复测五探针**全部正确**：`p30m9_mul_only` /
  `p30q_local_then_mul` / `p30p_field_order`＝**500**、`p30r_add_const`＝105、`p30s_local_copy`＝5；
  反汇编 `mul<int> 0 48` 操作数槽正确（槽 0＝`o.t` 经 `load_field` 读回＝5、槽 48＝`const_i32
  100`），`OpMul` 实现 `*dst *= *src` 无 struct 相关分支。原「已钉边界」四形态（5／105／5／
  244）恰因 5 与 105 落 8 位、500 越界而被 8 位观测误读。**差分基线闭环**：以
  0.7.9 发布二进制（`release/NLang-0.7.9-win64.zip` 解出 `ncc.exe`/`nvm.exe`）经
  Python 复跑同一探针＝**500**，与现行 0.9.11 二进制一致——原「0.7.4 差分基线同样
  复现＝既有缺陷」系对 bash 8 位观测的误读（新旧二进制经 Python 全值皆 500，bash
  皆截 244）。探针保留 `temp/p0911_init/p30m*.n` 供复核；结论＝退出码 ≤255 约定
  （「有意设计」vm #1）之外的观测假象，**无需代码修复**。
- ~~**协议名方法默认访问级别即可外部调用**~~——✅ **已解决（0.10.7 D5，2026-10-22）**：裁定为文档化规则——协议派发按名字查找、不检查可见性，`equals`/`getHashCode`/`toString` 不受访问修饰符限制、始终可外部调用（行为不变、零代码改动）；已写入 object.md 双语树，`proto_default_access` 钉住。
- **跨模块基类契约检查空转**：导入类桩为空，`class X : 导入基类` 的契约检查跳过
  （空转无静默危害——桩上调用编不过）。已在 known-limitations 文档化，属桩机制
  深层工程，单独立项价值待议。
  > **立项（2026-10-21，0.9.12）**：2026-10-21 实测刷新——「空转无静默危害」的前提
  > 已随 0.9.11 外部构造失效：消费者子类化导入基类现已可编译可运行（名称派发），
  > 错误覆写签名（`f(int)` 对基类 `f()`）静默通过（探针 vt），导入接口的
  > `implements` 契约同样静默通过（探针 vq）。与 P1a 同根因（桩不携带实例成员），
  > 并入 0.9.12 一并解决。
  > **→已解决（2026-10-21，0.9.12，提交 ca44f13）**：桩经 NCU v2.7 携带基类方法
  > 签名与接口实例签名，消费侧 override/implements 契约检查激活——导入桩与本地
  > 属主同等参与契约遍历（访问级别无关，运行期分派仅按名）；错误覆写签名、漏实现、
  > 错误实现、静态同名占虚槽、私有虚方法契约各报编译错误（e2e
  > `err_cross_mod_override_mismatch`/`err_implements_imported_interface_missing`/
  > `err_implements_imported_interface_mismatch`/`err_cross_mod_static_vs_virtual`/
  > `err_cross_mod_private_virtual_claim`），正向钉 `cross_mod_override_ok`/
  > `cross_mod_private_virtual_ok` 钉住继承调用方与基类型形参两条分派路径。

## 记档延后（低优先，后续批次顺手销账）

- **导入类成员三形态无 e2e 钉**（2026-10-17 发布后审查 Minor 2，见上条展开）。
  > **→已解决（2026-10-20，0.9.11 dev）**：密封布局两存活拒绝形态补 compile_error
  > e2e——`imported_class_field_read`（字段读报「Cannot resolve the field」）、
  > `imported_class_method_call`（方法调用报「does not exist」）；第三形态「桩构造」
  > 经 0.9.11 T5 已转为成功形态（外部 `.ncu` 构造可用），由正向钉 `cross_mod_class_ctor`
  > 覆盖，不再是拒绝形态。
- **构造调用循环默认无专属 e2e**（2026-10-18 规格审查 Info ②）：ctor/super 侧循环
  默认经共享 DefaultEmitGuard 与函数/方法路径同一实现（该路径有既有测试），规格
  测试计划未列；后续批次补一钉（形如 ctor 形参默认值引用自身）即可销账。
  > **→已解决（2026-10-20，0.9.11 dev）**：实测修正原前提——DefaultEmitGuard 的报错
  > 此前**无任何测试钉**（「该路径有既有测试」不成立）；且建议的 ctor 形参默认引用
  > 自身形态（`a = a`）被「默认值只能引用先前形参」规则在声明位拒绝，根本到不了
  > guard，ctor/super 侧循环默认路径在默认表达式文法下**不可达**。改钉两条可达路径：
  > `err_default_recursive_fn`（自由函数默认调用自身）、`err_default_recursive_method`
  > （方法默认在接收者上调用自身，即不可达 ctor 路径的接收者绑定类比），均编死
  > 「recursive default parameter in call to ...」。
- **MaxArgs walker 不下钻默认表达式内嵌调用的 callParamBase 需求**（既有缺口，
  observedPeakCursor tripwire 兜底；0.9.7 批次如再触 walker 面可顺带评估）。
  > **升格实证（2026-10-18 质量审查 Critical-1＋Info-8a）**：P3 构造默认参数把该缺口
  > 在 ctor 三臂（MaxArgsSuperCallStmt/MaxArgsCtorInvoke/MaxArgsNewExpr）**激活为
  > 静默实参破坏**（嵌套调用 bulk-copy 越过 callParamBase 打掉外层 claim，探针
  > p8：Vec(7) 实收 6，零诊断）；方法/自由函数臂同型破坏 Phase 9c 起即可复现
  > （探针 p4b："struct field store out of bounds"）。**ctor 三臂已修（2026-10-18，
  > f527dd0：CtorDefaultMaxArgs 成员守卫集下钻默认表达式子树；e2e
  > ctor_default_bigcall 钉回归）**；**方法/自由函数臂（MaxArgsInvokeExpr 等）
  > 残留为独立缺陷**，待单独立项（探针 p4b 仍复现）。
  > **→已解决（2026-10-18，v0.9.8）**：`CalleeDefaultMaxArgs`（原
  > `CtorDefaultMaxArgs` 机制泛化更名）在 `MaxArgsInvokeExpr` 对 callee
  > 非空的调用下钻默认值子树，四个调用发射臂全覆盖；e2e
  > `fn_default_bigcall`（修前退出码 127 静默算错）＋
  > `method_default_bigcall`（修前 struct field store out of bounds）双钉。
  > spec 见 docs/superpowers/specs/2026-10-18-maxargs-callee-defaults-design.md。
- **解析侧与代码生成侧的构造函数过滤规则不一致**（质量审查 Info-8b，既有）：
  FindCtorParamRange（ExprResolverNew.cpp:75-107）不滤无 body/native 成员，而
  codegen 侧 FindCtorFunction/CtorSlotFor 要求 body-or-native——无 body 非 native
  的 ctor 成员会被 arity 检查接受后静默 alloc-only。低频形态，待顺手批次收口。
  > **→已解决（2026-10-20，0.9.11 dev）**：注册判定谓词上提到 AST 层
  > `SnFunction::HasImplementation()`（有 body / native 标志 / imported 标志三态
  > 合一），解析侧重载候选收集 `CollectCtorCandidates` 与代码生成侧
  > `VmBackend::IsRegisteredCtor` 共用同一谓词，两层不再分叉。无 body、非 native、
  > 非 imported 的构造器成员不再进入候选集：仅能匹配它的调用报明确的无匹配错误
  > （复用既有 `no constructor of class/struct "..." matches the argument list` 与
  > `class/struct "..." has no constructor; ...() cannot take arguments` 文案），
  > 不再绑定一个只会分配对象、静默丢弃实参的构造器；零参数调用仍合法分配
  > （抽象形态，与无构造器类型同形）。e2e 四钉：`ctor_bodyless_arg_no_match`、
  > `ctor_bodyless_only_args`、`ctor_bodyless_arg_no_match_struct`、
  > `ctor_bodyless_zero_arg_alloc`。
- **EmitStmtTry.cpp:262-263 陈旧注释**（2026-10-18 修复批复审 Info）：cf4e125 后
  父类 ctor 默认引用 this 在声明位已不可能（父类自己的声明同样过
  ResolveFunctionFormals 检查），注释描述的回退语义不可达；thisSlot=0 布线本身
  仍正确需要。下次触碰该文件时顺手改写。
  > **→已解决（2026-10-20，0.9.11 dev）**：注释改写——删去「父类 ctor 默认可引用
  > this」的过期回退描述，改为说明 thisSlot=0 的语义（接收者暂存于 claim[0]，`new`
  > 调用则传 UINT16_MAX）＋父类 ctor 默认引用 this 已在声明位被拒；`0` 实参不变，
  > 函数仍 50 行合规。

## 新 surfaced 候选（2026-10-25 ndarray 阶段2 终审副产物）

- **M1 getArrayView 共享步长缓冲并发竞态** ✅ 已解决（0.11.3，提交
  2e9a34e7，host ABI v7）：步长内联进 `NativeArrayView` 自身
  （`stridesInline[kMaxArrayRank]`，`strides` 自指），共享 scratch 由构造
  消灭；`ArgArray` 改输出参数（含自指指针的视图不能按值传递）。
  **同形递延**：`stringScratch`/`lineScratch`（getString/readLine/
  readToken 的 `const char*` 暂存）仍为每执行器共享缓冲——树内零生产
  消费方，暂不构成缺陷；重开条件＝首个阻塞 native 采用字符串参数。
- **声明位多维尺寸 `int a[2,3]` 为裸语法错误** ✅ 已解决（0.11.3，提交
  0fe85b97）：文法六臂先解析后具名拒绝，诊断指向 `int[2, 3]` 创建式（裸/
  `= 表达式`/花括号初始化三形态，局部声明与续接声明符）；类字段位仍为
  语法错误，已文档化为边界。
- **零元素多维数组格式化为 `[]`** ✅ 已解决（0.11.3，提交 44c3cd3e）：
  `FormatArray` 空数组早退移到 rank≥2 分派之后；`int[3,0]`→`[[], [], []]`、
  `int[0,3]`→`[]`、`int[2,0,3]`→`[[], []]`；一维空数组不变。
- **tensor 阶段3 矩阵内核**（matmul/transpose/reshape，spec 已定义，复制
  实现）：能力缺口非缺陷，按需立项。
- ~~**构建树 native DLL 多副本根治**~~——✅ **已解决（0.11.4，2026-10-26）**：
  原状＝同一 native DLL 在构建树复制 5+ 处（3 个工具 Release 目录、
  tests/Release、e2e 夹具目录），各归不同 CMake 目标刷新，ABI 升级后定向
  构建易留陈旧副本。现＝单一权威目录 `<build>/native/<config>` +
  `NativeLibraryLoader` 构建树回退搜索（可执行文件目录 → 显式目录 → 回退，
  嵌入宿主与安装包契约不变）；spec/计划见
  docs/superpowers/{specs,plans}/2026-10-26-0114-batch-*。存量构建树需手动
  清理工具目录陈旧副本（CHANGELOG 已注明）。
- 阶段2 内部递延（M2 EmitGateRankOf 死代码、BytecodeOps 注释重复、
  test_stride_table ND 钉）：零用户感知，维护顺手清，不单独立项。
  0.11.3 终审第二轮备案：~~`ArrayPayloadBase` 截断检查只覆盖固定头
  （size<4）不覆盖 dims 段；`NativeGetArrayView` 校验前读 record[1]/[3]~~
  ——✅ **已解决（0.11.4，2026-10-26，commit 41fcd04e）**：dims 段校验并入
  `ArrayPayloadBase`，`NativeGetArrayView` 校验前置于全部头部读取（含
  arrayTypes 界检查），白盒测试（截断记录/下标覆写）突变验证过。
- ~~**ABI 校验晚于模块 init**~~——✅ **已解决（0.11.4 追加批，2026-10-26，commit
  111231a1）**：原状＝`EnsureLoaded` 先调 `nlang_native_init`（注册直穿宿主回调）再校验
  返回的版本号，不兼容模块的注册（指向随即卸载的库的函数指针）在拒绝前已进入宿主注册表。
  现＝契约新增必需导出 `nlang_abi_version` 探针（`NLANG_DEFINE_NATIVE_INIT` 宏自动附带，
  树内模块零源码改动），加载器在任何模块代码运行前解析并校验；init 返回值降为信息性。
  无探针模块以重编译提示拒绝（不向下兼容，用户裁定）。夹具三件套（badabi 手写坏探针/
  noentry 补正确探针/noprobe 无探针）＋突变验证钉住。
- ~~**单配置生成器 native 目录回退失配**~~——✅ **已解决（0.11.4 追加批，2026-10-26，
  commit 1db2de48）**：原状＝`native/$<CONFIG>` 无条件输出，Ninja/Makefiles 下 exe 目录
  叶子名是工具名，非空 `CMAKE_BUILD_TYPE` 时模块落在 `native/<config>` 而回退两候选
  （`native/<叶子>`/`native`）全部落空。现＝按 `CMAKE_CONFIGURATION_TYPES` 分派输出
  目录。（验证更新 2026-10-26：cmd AutoRun 注册表经用户授权修复后，单配置 Ninja
  树的真实编译链接已通过——nlang_math/io 真实构建、DLL 直落 `native/` 根。）
  （连带根治：docs 片段审计竞态——`nlang_docs` 缺工具/模块依赖边，契约变更时审计先于
  模块重建执行，即 0.11.3/0.11.4 两次「MSB8066 瞬时假失败」的真身；commit 2e6ff22d。）
- **注册回调抛错时 EnsureLoaded 泄漏 DLL 句柄**（0.11.4 追加批终审备案，Info，
  既有）：init 期间注册回调抛异常（如文档化的重复 ns.name 装载错误）时，句柄既未关闭
  也未入 m_loaded，进程存活期内泄漏一份库引用。本批未触碰该路径，且删除了旧的
  init-返回值拒绝路径（窗口缩小）。低频（宿主回调抛错即装载失败，进程通常随后退出）。
  修法＝EnsureLoaded 全路径 try/catch 收口句柄所有权（或 RAII 句柄守卫）。
- **NSIS 安装形态的 stdlib 探针无自动化覆盖**（0.11.4 终审第二轮，Low）：安装包形态为
  `prefix/bin`＋`prefix/stdlib` 相邻（FindStdLibDir 1 级向上探针命中）；自动化部署检查
  （nide_deploy_check）物化的是可执行文件旁 `stdlib` 形态（兜底探针），NSIS 的 1 级向上
  探针仅由人工安装冒烟覆盖。补法＝暂存目录做 exe 置于 bin/ 子目录、stdlib 与 bin/ 同级的
  变体；未立项。
