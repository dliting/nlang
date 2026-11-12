# 位运算符语言设计(Spec)

v0.13.7语言面批次：补上二元位运算符`& | ^`、一元按位取补`~`与复合赋值形`&= |= ^=`。用户裁定（2026-11-12）：语言面缺口应当合理补上而非以算术技巧绕行，改进语言、编译器和VM本身是任务的目标之一，且无需向下兼容。本批次与正则表达式库（v0.13.7路线图「实现正则表达式库」条目，见`docs/dev/regex_library_design.md`）同版交付：正则引擎的标志集与状态编码天然要用位运算，可读的代码是项目目标；但本批次的成立理由独立完备，不以正则库为必要条件。

## 一、目标与范围

### v1做

- 二元`&`（按位与）、`^`（按位异或）、`|`（按位或）与一元前缀`~`（按位取补），操作数为整型（`byte`至`ulong`）；enum成员字面量按其底层int32参与，enum类型变量被整型门拒绝——两条都与位移族行为一致（实测`E e; e << 1`报`operator '<<' requires integer operands, got "enum E".`，`temp/bitwise_probe/enum_shift.n`，2026-11-13）。
- 复合赋值形`&=` `|=` `^=`，规则全随`+=`/`<<=`族（左值集合、按声明类型存回）。
- 新运算符进常量表达式子集：凡已接受`<<`常量折叠的位置（跨模块默认参数、数组维度等）同步接受。
- 手册「语言规格」运算符章更新（中英成对）。

### v1不做（各有独立理由，非「已知限制」式递延）

- **`~=`复合形**：C/C++/Java/C#/Rust/Go均无此运算符——取补不是结合运算，`x ~= y`没有`x = x ~ y`之外的语义锚点，也没有任何主流用户群携带这一直觉。它的半成品词法脚手架（`nlang.l:339`已在发射`OT_AEQ`，见事实2）随本版死脚手架扫荡一并拆除（用户裁定2026-11-12），不保留。
- **位运算符的用户类型重载**：语言整体没有运算符重载机制，位移族同样不重载；重载是独立语言子项目。
- **bool操作数的位运算**：布尔面已由`&& || !`覆盖；位运算符仅关整型（各参照语言一致——C#甚至刻意让bool上的`&`不短路但仍是独立规则，nlang无短路需求场景，不开这个面）。
- **char操作数**：char是字符类型不是整型，`<<`已同样排除char（手册`05-expressions/operators.md:35`），位运算取相同口径保持族内一致。char参与需显式`as`转换（既有规则）。

## 二、关键事实（勘察结论）

1. 语言现有位运算只有位移：`nlang.y:2271-2272`是仅有的位移二元产生式；一元产生式（`nlang.y:2277-2279`）只有`+ - !`。`&`、`|`、`^`、`~`没有自己的词法规则，只落进词法器的兜底错误分支（`nlang.l:477`），故当前是词法错误；多字符运算符规则里`nlang.l`只有`&&`（:340）与`||`（:341），另有一个未接线的`~=`（:339→`OT_AEQ`，见事实2）。
2. 语法脚手架已半铺：优先级线已按C族次序预留——`%left '|'`、`%left '^'`、`%left '&'`（`nlang.y:477-479`）位于`&&`与`==`之间，`~`在单目右联线（`nlang.y:485`）与`!`同级；词法器已识别`&=`（`nlang.l:343`）与`|=`（`nlang.l:348`）并映射到已声明的token（`nlang.y:461,466`），但没有任何产生式消费它们；同样状态的还有`~=`（:339→`OT_AEQ`，`nlang.y:457`声明、挂在相等优先级线`:480`——是比较位置而非复合形，残留形态暗示曾按「不等于」候选规划）与`++`/`--`（`nlang.l:292-293`→`OT_INC`/`OT_DEC`，`nlang.y:447-448`、单目线`:485`）——共五个死token，手册`01-lexical.md:109`（en`:152-155`）正是用一句话列举这五个序列并宣布它们按语法错误拒绝。本批次接通位运算族三个（`&=`/`|=`接入产生式、新增`^=`），消除这部分半成品；`~=`/`++`/`--`不是本批次运算符（见「v1不做」），随本版全仓死脚手架扫荡与死关键字`elseif`/`state`一并处置（用户裁定2026-11-12）。
3. 位移族的完整管线就是直接模板，站点清单已钉死：`nlang.l`；`nlang.y`（二元产生式+复合形产生式四位点：IdentifierExpr `:1115,1118`、MemberExpr `:1136,1139`、SubscriptExpr `:1157,1160`、for-update `:1312,1315`）；`include/nlang/compiler/SnExpressions.h`运算符枚举（`:800-805`）；`src/compiler/builder/ExprResolverBinary.cpp`类型规则（`ResolveShiftBinary:333`）；`src/compiler/builder/StatementResolverCompound.cpp:43-68`复合形；`include/nlang/vm/ExprDesc.h`与`src/compiler/builder/CompiledModuleDefaultSupport.hpp`常量子集；`src/vm/backend/EmitExprBinary.cpp:205`codegen；`src/vm/BytecodeOps.h:153`opcode与`:255`kind表；`src/vm/VmExecutorOps.cpp:206`执行；`src/vm/VmExecutorOpsPrim.cpp`逐行序言表；`src/vm/OpCodeTable.cpp`助记名表（位置表，`static_assert(size == OP_Count)`钉死，`:107-108`——新opcode必须加行）；`src/vm/Disassembler.cpp`反汇编。运算符枚举的新成员须按线序追加在`Shr`之后：`EDB_*`注明「in SnBinaryExpr::Operator order」（`ExprDesc.h:32`），加载器`kOps`按位置解码——中间插入会静默漂移线码。
4. 格式面有两处，本批次都触及。(a) 指令集：25-ncu章§25.1明文opcode值与指令形态不是本规范的一部分（图像内的指令数组不透明），但把指令集钉在格式版本上——「2.13的映像携带2.13工具链的指令集」（zh :13）；历史上新增opcode即升版本次号——`CompiledModule.h:37-44`版本史注释：v1.8新增函数值opcode族并升，注明"Older VMs cannot execute these opcodes; refuse outright"。(b) 默认表达式语法（§25.6）本身就是手册面：节点tag `0x20`–`0x23`与二元运算符字节枚举（`0 +,1 -,2 *,3 /,4 %,5 <<,6 >>`）逐字写出（zh `25-ncu.md:215-223`、en `:329-337`），对应常量为`include/nlang/vm/ExprDesc.h:28-39`（`EDB_Add`..`EDB_Shr`），加载器硬拒越界运算符字节（`CompiledModuleDefaultSupport.hpp:268-285`，含`kOps` static_assert）。D4把位运算送进跨模块默认参数⇒语法扩展须同步修订§25.6（中英），且格式次号须2.13→2.14（`NCU_FORMAT_MINOR`，`CompiledModule.h:154`；§25.1「没有迁移，也没有部分接受」，旧映像一律拒绝）。27-abi只辖native模块调用约定，本批次零新增native；`.ncu`/`.npkg`同树重编，无向下兼容负担。
5. 手册锚点：运算符章的位移族（`docs/user_manual/zh/specification/05-expressions/operators.md:32-41`及en对应）逐段承载「类型规则/运行期行为/优先级/复合形」四段式写法，是本文措辞模板；运算符总表在`05-expressions/index.md`；复合形左值集合引用`06-statements`复合赋值节。
6. e2e先例：行为用`all_int_ops`/`logical_ops`钉（`tests/e2e/manifest.txt:15,20`），诊断用compile_error行钉（`manifest.txt:523-525`的`operator '&&' requires bool operands`形态）；常量子集单测模板在`tests/test_compiler/test_default_exprdesc.cpp`。
7. 优先级对照主流参照：C/C++/Java/C#一致——按位与＞异或＞或，三者皆松于相等比较、严于逻辑运算；`~`与`!`同级。Rust与Go是例外：Rust刻意把`& ^ |`排在比较运算符更紧（Rust Reference「Operator precedence」表，对C经典坑`x & 1 == 0`的修复——按`(x & 1) == 0`读；Rust亦无`~`，按位取补用`!`），Go则`&`与乘法同级、`| ^`与加法同级。nlang的既有运算符总表（`&& ||`、比较、位移次序）是C族血统，且语法优先级线已按C族预留（事实2），取C族次序；Rust的`& > ^ > |`相对次序与「严于`&&`/`||`」两半与裁定相同，仅「与`==`的相对位置」一说不作支持证据。
8. 类型规则锚点：`ResolveShiftBinary`（`ExprResolverBinary.cpp:333-365`）已定族内规则——整型检查（`PrimKindIsInteger`：标量登记表无enum行，enum类型变量被拒；成员字面量按int32定型故通过，实测见「v1做」节）、null操作数编译期拒绝、左行rank<3单目提升为int32（`:356-360`，即byte+byte规则施于单操作数）；算术二元混型的共同类型由Phase 8e-8对称提升机制给出（该轮修复了`1+2.5`不对称；rank<3提升发生在共同类型步`:134-139`，rank≥3行保持自身、含`uint32`）。

## 三、调研结论与出处

| 议题 | 结论 | 出处 |
|---|---|---|
| 运算符集 | `& ^ \| ~`加复合`&= \|= ^=`，无`~=` | `& ^ \|`与`&= \|= ^=`六家（C/C++/Java/C#/Rust/Go）通用；按位取补五家写`~`（Rust写`!`）；`~=`六家皆无 |
| 优先级 | C族次序（`& ^ \|`介于相等与逻辑之间，`~`与`!`同级） | C++ operator precedence表、Java LS §15.22、C#运算符表同序；Rust Reference与Go spec为例外形态（事实7，不作支持证据）；语法预留线（事实2） |
| 类型规则 | 仅整型（enum成员字面量参与、enum变量被拒——同位移门实测），char/bool/浮点拒绝；结果走算术对称提升 | 本仓`ResolveShiftBinary`族内规则（事实8）+Phase 8e-8机制；C#的`int & long → long`同效 |
| 格式面 | 新增opcode与扩默认表达式语法都触及`.ncu`次号与25-ncu章（§25.6为手册明文面） | `CompiledModule.h:34-49,154`版本史与常量单一真源；`25-ncu.md` zh :9/:215-223、en :329-337；`ExprDesc.h:28-39`；`CompiledModuleDefaultSupport.hpp:268-285` |
| 复合形 | 随`+=`/`<<=`族：声明类型存回、隐式窄化、不单目提升 | 手册`operators.md:41`的`<<=`规则（同族既有裁定） |
| 常量子集 | 随位移入子集 | 0.9.9位移入子集先例（e2e `cross_mod_default_shift`、`manifest.txt:670-671`） |

## 四、架构裁定

**D1 运算符集与语法形态。** 二元`& ^ |`左结合、一元`~`前缀右结合、复合`&= |= ^=`右结合，各放在语法已预留的优先级位置（事实2），不新增优先级线。`^=`需新增词法规则与token（`OT_BXORS`），`&=`/`|=`复用既有死token接入产生式。flex最长匹配保证`&&`/`||`与单字符形无歧义；`&&`操作数为bool（0.7.5已取消原始int隐式转bool，`CheckLogicalBoolOperands`），e2e以`((1 & 3) != 0) && ((2 | 1) != 0)`混合式钉住——`&&`若被拆成两个`&`则解析当场失败。

**D2 类型规则。** 操作数限整型；enum成员字面量（按int32定型）参与、enum类型变量被整型门拒绝（同位移口径，事实8）；char、bool、float、double、string与null操作数一律编译期拒绝，诊断点名运算符与坏类型（`operator '&' requires integer operands, got "double".`；null沿用`null is not a valid bitwise operand.`句式）。二元结果类型：先过整型门，再走算术族对称提升（Phase 8e-8机制）取共同类型——`int & long`是long、`byte & byte`单目提升为int；无隐式共同类型的组合（如`int`×`ulong`）与算术同形编译期报错（实测`a + b`即报`no implicit common type for "Int32" and "ULong".`，族继承边界，见五的诊断钉）。位运算没有「计数」语义，不走位移的计数门：右操作数与左操作数对等参与提升（`1 << long计数`报错而`1 & long`合法——与位移的本质区别，手册写成正面规则）。`~`结果类型：操作数施以与二元结果同款的单目提升——rank<3的行提升为int32，rank≥3的行保持自身（事实8）。不能写成「与一元负号同规则」：一元负号原样携带操作数行（`ExprResolverBinary.cpp:246-253`，`-ulong`是`ulong`），照它做byte取补仍是byte，与同族`byte & byte`是int相矛盾。运行期无新增异常路径：位运算无溢出概念、无计数范围检查，结果为二进制补码位模式的天然定义。

**D3 复合形。** `&= |= ^=`全随复合赋值族：左值=局部变量/字段/下标元素（与`+=`同集合），按左值声明类型存回、隐式窄化、不做单目提升（`byte b &= 200`按byte回绕），右操作数按普通隐式转换进入声明类型。enum左值与右值规则继承族内现状，实施计划按`+=`现状核实钉数。

**D4 常量表达式子集。** 四个新运算符随位移同例入常量折叠子集（跨模块默认参数、数组维度等已接受`<<`的位置同步接受，事实6的`cross_mod_default_shift`先例）；折叠实现为共享常量折叠器加四个case，不新建机制。线格式随0.9.9位移入子集先例扩展：`ED_Binary`运算符字节追加`EDB_BitAnd=7`、`EDB_BitOr=8`、`EDB_BitXor=9`（续接枚举），取补用新节点tag`ED_BitNot=0x24`（仿`ED_Neg`单孩子形态）；加载器`kOps`表加三行、`static_assert`随扩；折叠结果落在子集外仍记`0xFC`；版本门槛由次号2.14承担（事实4），opcode表无需跨版本双解。

**D5 字节码。** 新增opcode `OP_BitAnd` `OP_BitOr` `OP_BitXor` `OP_BitNot`，kind编码随算术族——**逐行表**：如`kShlTable`（`VmExecutorOpsPrim.cpp:135-142`）由X-macro `SCALAR_PRIMITIVE_DECL`为每个标量行生成一行，八个整型行（`byte`..`ulong`）各用自身原生承载（`uint32`行独立、不折进int32），enum在编译期已被整型门挡住、从不以enum身份到VM；非整型行的条目同`Shl`惯例存在但永不发射。无新运行期错误路径。`.ncu`格式次号bump 2.13→2.14——新增opcode（v1.8先例）与默认表达式语法扩展（事实4）都已落地，旧映像按「重新编译」诊断拒绝；native ABI版本不动。

**D6 版本与手册落点。** 随v0.13.7发布，属语言面变更：CHANGELOG「Added」一条（中英成对）；VERSION在首个用户可见变更落地时升0.13.7（早于正则库收尾）；手册更新`05-expressions/operators.md`（中英）新增四段式小节、`05-expressions/index.md`运算符总表同步、`06-statements/index.md`（中英）两处枚举同步（`:62`的「`op`是`+ - * / % << >>`之一」与`:74`的示例代码行）；`01-lexical.md`§1.6（zh`:105-109`、en`:147-155`）两段同步重写——「出现在语法中的多字符运算符」清单（`:106-107`）增`&= |= ^=`——单字符`& | ^ ~`不进该清单：清单只列多字符运算符，单字符面由「单字符标点自成一个记号」句（`:109`）覆盖、其合法位置由语法定义，用户面说明由`05-expressions`运算符小节承担；「词法器也会产生`++ -- &= |= ~=`但都不在任何产生式中」句在本批后仅`~= ++ --`仍成立（其词法脚手架随本版死脚手架扫荡拆除，拆除后该句直接删除——`++ -- ~=`不再构成单记号，无词法事实可述），手册两段随两处改动分两次同步；`formats/25-ncu.md`（中英）§25.6默认表达式表（运算符字节枚举延长+`0x24`新行）与全章版本提数同步2.13→2.14（实测「2.13」在zh :9/:13/:258及en对应处，实施时grep全章核对无遗漏）；`CompiledModule.h`版本史注释补一条。本变更属`manual_sync_check.py`五类中「语言面」类（§25.6属格式面、同属用户可见同步），落在本版手册同步批次内；`~= ++ --`脚手架拆除不改变诊断口径：实测拆除前（`temp/bitwise_probe/former_inc.n`、`former_aeq.n`）与以拆除后的单字符记号流预演（`sim_post_inc.n`，把`i++`写成`i + + ;`即拆除后同一记号序列）报告同一`syntax error`+`Invalid statement.`诊断（2026-11-13），仅报错定位移位；故诊断文案类在本批无独立同步项，`elseif`/`state`由保留字恢复为可用标识符属语言面类。

## 五、测试策略

- **行为钉**（e2e，均`0`退出）：`bitwise_ops`——AND/XOR/OR/取补值表含负数与`long`高位、窄型提升、enum成员字面量参与、补码钉（`~5 == -6`、`~0 == -1`）、窄型取补提升钉（byte的`~`结果按int钉，钉住D2的单目提升线）；优先级钉`(3 ^ 1 & 2) == 3`（`&`紧于`^`）、`1 | 2 ^ 3`按`1 | (2 ^ 3)`读（`^`紧于`|`）、`~`与`!`同级混用、`((1 & 3) != 0) && ((2 | 1) != 0)`词法最长匹配钉（bool操作数口径，见D1）；混型提升钉`-1 & 1099511627776`结果为long且bit40保留（符号扩展让高位进入结果——`1 & 2^40`这类结果为0的式子区分不了承载体，两种承载体都得0）、`byte & byte`为int。`bitwise_compound`——三运算符复合形、按声明类型窄化存回、声明类型不参与单目提升。
- **诊断钉**（compile_error行）：`bitwise_operand_float`（`operator '&' requires integer operands`）、bool/char/string变体各一行、`bitwise_operand_enum_var`（enum类型变量`e & 1`，钉`got "enum E"`一行，锁死enum变量口径）、`bitwise_not_operand_bool`（`~`同族诊断）、null操作数一行、`bitwise_signed_unsigned`（`int`与`ulong`混用钉`no implicit common type for "Int32" and "ULong".`一行——D2对称提升继承算术族既有边界，实测算术同形：`a + b`同报此错，`temp/bitwise_probe/uint_mix.n`，2026-11-13）；对`==`的优先级钉落在诊断上：`bitwise_precedence_eq`——`5 & 3 == 1`按`5 & (3 == 1)`读、`3 == 1`是bool故报错；若`&`紧于`==`该表达式类型合法不会报错，报错本身即优先级证据。
- **常量子集**：`cross_mod_default_bitwise`随`cross_mod_default_shift`模板；`test_default_exprdesc.cpp`补位运算默认参数断言（往返覆盖运算符字节`7/8/9`与tag`0x24`）；`test_nculoader.cpp:470`的格式次号常量钉13→14；加载器对越界运算符字节的malformed拒绝用例（事实4b）在新码扩展后仍全过。
- **回归**：既有`all_int_ops`/`logical_ops`/`cross_mod_default_shift`全绿；bison冲突计数不高于现状（`nlang.y:1938-1939`注释台账记录现值30 shift/reduce、3 reduce/reduce；新增产生式落在既有优先级线上，冲突增量若现以`%prec`处置）。
- **门禁**：例行三门（Debug断言+ASan+IDE树ctest）；无新增慢测试项。

## 六、风险与开放问题

1. **LALR冲突变动**：优先级线早已就位，产生式增量与既有状态正交；实施时以bison报告复测为准（见五）。
2. **优先级惊讶**：`&`松于`==`是C族历史坑（`x & 1 == 0`是`x & (1 == 0)`），本批次循惯例不改，手册优先级段显式钉死（镜像位移族`operators.md:39`写法）。
3. **与正则类面的交叠**：模式字符串里的`& | ^`是字符串内容，语言运算符化后无任何交叠面（唯一相关处是类内`&`/`~`字面量裁定，见正则spec D3，两者互不涉入）。

## 修订记录

| 日期 | 修订 |
|---|---|
| 2026-11-12 | 初稿：用户裁定「语言面缺口合理即补」入册；D1-D6裁定；站点清单与族范模板以实测行号钉死（`ResolveShiftBinary`、复合形四位点、语法预留线、死token） |
| 2026-11-13 | 第三方spec审核发现经独立核实后落实：事实4拆成两处格式面（opcode版本钉扎、§25.6语法）并裁定格式次号2.13→2.14；D5承载从「int32/int64两种」改为逐行表（实证`kShlTable`族）；D2的`~`改自洽单目提升规则（「与一元负号同规则」被`ExprResolverBinary.cpp:246-253`证伪）；enum口径精确化（成员字面量参与、类型变量被拒，实测`<<`）；对`==`的优先级钉从行为移入诊断（`5 & 3 == 1`是编译错误），行为钉补`(3 ^ 1 & 2)==3` |
| 2026-11-13 | 第二轮审核落实（各项均独立复核）：死token从两个改判五个——`~=`（`nlang.l:339`→`OT_AEQ`）与`++`/`--`（`:292-293`→`OT_INC`/`OT_DEC`）实测同样是「发射但无产生式消费」，处置裁定为随本版死脚手架扫荡拆除；手册落点按§1.6实况校正：`01-lexical`§1.6多字符运算符清单只增`&= |= ^=`（单字符面由「单字符标点自成一个记号」句覆盖，不进清单）、拆除段直接删拒绝句，`06-statements`登记两处枚举；实测`~= ++ --`拆除前后报同一语法错误（`sim_post_inc.n`预演拆除后记号流），原「变为未知字符词法错误」说法校正、诊断文案类无独立同步项；站点表补`OpCodeTable.cpp`助记名表并写明线序追加纪律（D4）；Rust的`& ^ \|`实测紧于比较（Rust Reference），从「一致」名单移入例外（事实7、调研表）；for-update站点行号`:1312,1315`修正；bison冲突基线修正30/3（`nlang.y:1938-1939`台账）；路线图交叉引用从条目号改为条目名（条目号随路线图编辑漂移）；D2补「无隐式共同类型」边界（`CommonNumericType`返null，`ExprResolverBinary.cpp:115,155`）并新增`bitwise_signed_unsigned`诊断钉（实测`temp/bitwise_probe/uint_mix.n`） |
| 2026-11-13 | 实施计划审核落实（各项均独立复核）：`&&`混合钉改写为`((1 & 3) != 0) && ((2 | 1) != 0)`——`&&`操作数要求bool（0.7.5已取消原始int隐式转bool，`ExprResolverTypeGates.cpp:24-52`实测），原`1 & 3 && 2 | 1`不能类型检查、根本编不过；混型提升钉从`1 & 2L`改为`-1 & 1099511627776`（字面量自动加宽，符号扩展让bit40进入结果——结果为0的式子在int或long任一承载体下都得0，区分不了承载体） |
