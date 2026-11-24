# dev并入master：双线合并设计（2026-11-16）

## 一、性质与完成判据

这不是一次新功能，是把两条各自已经停靠的开发线并回一条：

- **dev线**：v0.13.11的`json`库（纯NLang实现，已成版，本地提交未推送）、json暴露的两项语言面事项与两个缺陷修复、门禁写目录锁（`tools/gate.sh`的写目录认领，`docs/dev/gate_write_dir_lock_design.md`）。
- **master线**：v0.13.8收官面（类型级与函数级访问控制、描述符拼写改走包限定链接身份）、v0.13.9第②期的记录稳定化在途工作（`OP_CopyStructInPlace`、字段/元素/局部变量原位写、随后撤掉`StructCopy`记录锁改按cell粒度）、门禁分层工具链（`tools/gate_tier.sh`＋`tools/gate_tiers.txt`＋`tools/test_impact.py`＋`tools/e2e_shard.py`＋`tools/tier_coverage_check.py`＋CONTRIBUTING的三档说明）。

分叉点`aaa82bb5`，master领先28条提交，dev领先若干（含v0.13.11成版与写目录锁8条）。

**完成判据**（缺一不算合完）：合并树三树重建与ctest全绿、e2e串行全绿、手册五类漂移清零并刷新同步基线、双线roadmap收口成一份、`VERSION`与CHANGELOG中英成对自洽、两套门禁工具在同一棵树上都能跑出判决行。合并只落本地提交，推送与制品仍留到发布时点。

## 二、合并前置

1. dev工作树的未提交面先清干净：用户对`docs/roadmap.md`的未暂存两行（v0.13.9那条末句改判为「并发原位拷贝不保证单原子存储操作」，与master撤记录锁的走向一致；v0.13.13+标题加「可以根据工作量拆分成多个版本」）先落成一次`docs(roadmap)`提交。带着未暂存编辑合并会被git拒绝，而这两行是裁决正文，必须进历史。**已落地**（`f3ff2230`），工作树现在除本地独占路径外干净。
2. 本批（写目录锁）先按自己的计划收尾：第三方评审改到无Medium及以上新问题、七条腿门禁批全绿、手册同步判定入台账。合并不能和收尾混在一次提交里，否则门禁结果无法归因。
3. 泄漏扫描已通过（实测）：`aaa82bb5..master`的28条里带进版本库的文档只有`docs/dev/`下三份设计文档（`gate_tiering_design.md`、`generic_todo.md`、`value_type_reference_design.md`），没有`CLAUDE.md`，没有`docs/superpowers/`。因此不需要`filter-branch`摘除，直接并。

## 三、冲突面（git 2.27的`merge-tree`预演结果，不是估计）

预演命令`git merge-tree aaa82bb5 HEAD master`（HEAD为写目录锁收尾批之后的dev尖端；早先对`1800f6d6`跑的那一次已被`97cca124`作废，本节是重跑结果）给出**15个文件带冲突标记**；两侧都改过、但能自动干净合并的另有16个（含`tests/CMakeLists.txt`、`src/vm/VmBackend.h`、`src/vm/VmExecutor.h`、`src/vm/backend/Register.cpp`、`src/vm/backend/WalkersStmt.cpp`与九份手册页）。

判定按预演转储的段落边界机读，不按`<<<<<<<`肉眼扫：那种扫法会把标记行归到窗口里恰好相邻的另一个文件上（`docs/dev/value_type_reference_design.md`就被这样误报过一份，它其实干净）。下面第一类是机读出的冲突清单，第二类是git不报冲突但合并后会静默丢内容的段落边界情形。

冲突文件的逐类处置：

- **`CHANGELOG.md`与`CHANGELOG.zh-CN.md`**：dev在顶部新开`[0.13.11]`段，master改写过的正是它下面那段`[0.13.8]`的正文（日期改成2026-11-15、补泛型术语统一条目）。取「dev的新段＋master改写过的`[0.13.8]`正文」，中英两份同判。两侧内容互不覆盖，文本上像冲突只是因为都动在文件顶部。
- **`CONTRIBUTING.md`**：现在两侧都改过——dev的`97cca124`在门禁一节补了ICMP用例的主机前提，master在同一片加了门禁三档说明，两处撞在同一区域。取两侧并集，再按第五节补一句写目录按键认领与具名资源claim的关系（即`CONTRIBUTING.md`门禁一节现行的claim措辞）。（早前「只有master改过、直接取master侧」的判断已被`97cca124`推翻。）
- **`docs/roadmap.md`**：以master侧为骨架——它已经做过一次双线并账（`b4f9a7f2`）并把0.13.8未交付的条6/7/9挪到v0.13.13+（`d29a08ee`），结构比dev侧新；再并入dev侧的v0.13.11已交付措辞、v0.13.12 WEB框架条目与用户那两行判词。逐行核对，不靠自动合并的结果。
- **`docs/user_manual/SYNC_BASELINE`**：三份各不相同（分叉点`6164bfe5`、dev`6fb94de3`、master`460f7e1d`）。合并提交记不下自己的哈希，所以基线只能由合并之后的下一次手册提交承载；合并提交里先保留dev值（较新的一侧），随后在合并批内对基线以来的每一条提交逐个判定——master侧那28条里的手册面、诊断文案、退出码、ABI变化此时都会重新出现在待判定清单上，这是正常且必要的，必须判到清零才能`--update`。
- **五份手册页**（`en`与`zh`的`03-program-structure/index.md`、`07-functions/index.md`，加`zh`的`10-concurrency/index.md`）：两侧都在同一节加过内容——dev侧是json与预置包/包结构面，master侧是访问控制与描述符限定身份面。按「用户能依赖什么」的并集重写，而不是把两段文本拼起来；中英必须成对落地。
- **四处解析器源文件**（`src/compiler/builder/ExprResolverNewCtor.cpp`、`ExprResolverPackageAdmission.cpp`、`ExprResolverStdLib.cpp`、`StatementResolverSuper.cpp`）：这是合并里唯一有真语义风险的一处。master侧把构造器匹配与流参数扫描统一进了helpers（`9db58be1`）并加了裸类型名自身单元优先、描述符限定身份（`01420017`、`79e9e8e2`、`1c7fe352`）；dev侧改的是json需要的构造器实参隐式加宽、跨模块可见性相关判定与两个缺陷。两侧都在同一函数体内动过，必须读懂两侧意图后按语义并，禁止「取一边」。并完先跑`test_perunit`、`test_module_import`、`test_library_index`与`ctor_arg_implicit_widen`、`imported_*`族e2e，再进全批。
- **`tests/e2e/manifest.txt`**：两侧都追加了条目。取名字并集、去重、行序稳定，条目数按「非注释非空行」实数核，等于两侧并集数才算对。

**第二类：git不报冲突、合并后会静默丢内容的两处**

- **`docs/user_manual/{en,zh}/specification/formats/26-ncu.md`**：dev为了插json章把格式章26/27/28整体重编号为27/28/29（`e3341760`），master的`8997e920`则在旧路径`26-ncu.md`上补写了ncu 2.20的类型级与函数级访问字节和加载器拒收规则。预演把它标成`removed in local`：git视dev的重命名为删除，master的编辑随之静默丢弃，合并既不报冲突也不报错。已按内容比对两版正文（剥掉章号位移后剩十处差异）：master补写的事实dev的`27-ncu.md`全都已经写了，dev侧还多出类记录的泛型声明尾部。唯一的实质分歧是enum记录里类型访问字节的位置——master写它在方法表之前，dev写它在最后。**代码站在dev一侧**：`src/vm/ModuleSaverRecordsEnums.cpp`两条线一字不差，而它先写方法行、最后才写`enumAccess`。因此合并取dev的`27-ncu.md`，master那句enum顺序不搬。
- **`docs/user_manual/{en,zh}/specification/formats/`的其余页**：master只动过ncu一章（`git log aaa82bb5..master -- 'docs/user_manual/*/specification/formats/*'`只有`8997e920`一条），npkg与abi两章的改名不带风险。

## 四、版本号裁决

`VERSION`：分叉点是`0.13.8`，dev已抬到`0.13.11`（json成版），master仍是`0.13.8`（它的新工作是v0.13.9②的在途改动，自己也没抬版本、没开CHANGELOG段）。**合并树取`0.13.11`**，master侧的记录稳定化改动作为v0.13.9第②期的在途工作留在树上，暂不开`[0.13.9]`段，等四期收齐一次写。理由与现状无关：roadmap的版本号已经分配完毕（0.13.9值类型引用、0.13.10 lambda、0.13.11 json已交付、0.13.12 WEB框架、0.13.13+泛型完善可拆多版），dev侧json已按0.13.11成版且手册`26-json.md`与之配套；把一个已成版的号回退去迁就在途工作，等于让未交付面改写已交付面的身份。这条改动roadmap上的版本叙事（0.13.9排在0.13.11之后交付），已请用户裁决，见第七节。

## 五、两套门禁工具并存

- 文件层面无冲突：写目录锁只动`tools/gate.sh`，master新增的是`tools/gate_tier.sh`与几张清单/守卫脚本。
- 行为层面已核对：`gate_tier.sh`的`leg()`每条腿都经`gate.sh`起跑且不起后台，全部串行由具名资源claim与e2e分片标签互斥序化（D-10、D-11），所以合并不会改变T0/T1/T2的排程，也不会牵动那条线校准过的计时帽。
- 一个真交叉要当场验：`gate_tier.sh`拼的腿命令带`env NLANG_E2E_MANIFEST=…`前缀，正好落在dev新增的「只认行内前缀的env通道」解析路径上。合并批里要实跑一条这样的腿，确认判决行与目录锁行为都对，而不是只看`--dry-run`的装配输出。
- 文档面补一处：CONTRIBUTING的三档门禁说明来自master，写目录锁的使用口径在dev的`gate.sh`头部注释里；合并批要在CONTRIBUTING里把两者都写全并补一句它们的关系，让贡献者文档与`gate.sh`头部注释口径一致。CONTRIBUTING因此是冲突文件（见第三节），不是「取master侧」能了事的。

## 六、合并批门禁

合并树同时含VM/编译器改动、标准库`.n`改动与IDE树注册面改动，因此按门禁扫尾顺序跑例行全套：三树（`build-dev` Debug、`build-ide` Release、`build-asan` Release）重新configure＋重建可并行，三树ctest串行，e2e串行跑Debug档一次（合并正是为了验两侧语义是否仍自洽，e2e不能省）；断言构建冒烟腿必含。Release构建与Release档e2e仍留发布时点。任一条腿红，整批作废重扫。

合并批还要单独跑一次master带来的四份工具单元测试：`tools/e2e_shard_test.py`、`tools/test_impact_test.py`、`tools/tier_coverage_check_test.py`、`tools/source_size_guard/check_source_size_test.py`。实测两条线的注册面都没有跑它们的位置——ctest里只有守卫本体`check_source_size.py`（master的`tests/CMakeLists.txt:2386`）与docs管线pytest（`nlang_docs_pytest`），`gate_tier.sh`只把`test_impact.py`本体用作影响面映射。三树ctest因此全绿也不覆盖这四份；它们跟master一起进来时是「有测试、无运行」的状态，合并批必须亲手跑一次拿结果，并把「要不要挂进某条腿」记成master线的待办而不是就此当作已覆盖。

e2e腿还带一条主机前提：ICMP的ping用例要主机真能收发echo。本机现况是不行——Oray VPN的过滤profile在场时，对环回与默认网关的ping都100%丢包（`CONTRIBUTING.md`的门禁一节已记这条前提）。合并批若在这台机器上跑，那几条ping用例会红，而这是环境缺能力、不是回归：按「门禁判决先看失败项身份」的既有做法先单独复现确认，再决定是等能力恢复后重跑该腿，还是把这条腿标成受环境限制并当场记录。不得为了让腿变绿去改测试预期（测试断言不依赖时间的裁定同样适用于这里）。

本轮实测印证了这条前提：15:16–15:37那条e2e腿只有两条ping用例红（`stdlib_net_ping_loopback`、`stdlib_net_ping_stress`），stderr都是`TimeoutException: net: ping 127.0.0.1 timed out`，即3000ms与4000ms的期限内没有echo应答；同一条腿里不需要echo应答的两条对照用例反倒全绿（`stdlib_net_icmp_raw`只冒烟raw socket绑定，`stdlib_net_ping_error_text`只看解析失败的措辞）。隔两分钟系统`ping 127.0.0.1`丢包100%，再隔两分钟用同一批二进制复跑那四条用例五轮全绿、系统ping三次全应答：能力在场与否随分钟级翻转，红腿的判决跟着能力走而不是跟着代码走。合并提交本身没碰net原生模块（`git show --stat a8b966eb`里没有任何net路径的文件），单元测试面按`test_net_platform.cpp`钉住判决集合而不要求应答，所以三树ctest全绿与此并行不悖。「等能力恢复后重跑该腿」这条处置试过而不成立：复跑整腿（约20分钟）仍在那两条ping用例红，红腿跨越了分钟级的翻转窗，等待不能把窗口挪到腿外。终局处置是把master线当日（2026-11-16）15:06刚落地的环境门控接进本线——`tests/e2e/env_gates.txt`登记「条目→能力键」，运行器每轮以带外OS先知采一次，能力缺席即打印可见的SKIP行并计入skipped。该提交（`e4b252cb`）比合并所取的master侧更新，所以合并树本来就没有它，本次按移植接入而非合并hunk；理由与先知新鲜度前提记在`docs/dev/gate_tiering_design.md`的环境门控一节。单测面沿用本线既有的纯契约写法（`test_net_platform.cpp`钉住出口集合、不要求应答），未取master侧的先知写法。两条ping用例的测试预期一路未改。

## 七、用户裁决（2026-11-16）

1. 版本号按第四节执行：合并树取dev的`0.13.11`，master侧v0.13.9②的记录稳定化改动不在合并树里成版，也不另开一个号。
2. **master线继续在master树上走v0.13.9**，不因合并而停在v0.13.8收官。两线因此按设计再次分叉：合并后的dev线做v0.13.12 WEB框架，master线做v0.13.9的②③④期。下一次合并的代价由这段间隔里两侧各自的改动量决定，合并越晚代价越大——这不是要收缩或递延功能的理由，只是排期时要算进去的成本。

两条裁决对合并批的直接影响：合并提交里`VERSION`写`0.13.11`、`CHANGELOG`中英两份都不开`[0.13.9]`段；`docs/roadmap.md`要把0.13.9写清「在master线上进行中、尚未成版」，免得dev线把它当成可以开工的版本重做一遍。

## 后记（2026-11-23，合并执行时）

本文写就时master尚在0.13.8收官面；实际合并发生在2026-11-23，此前master已在自身树上交付v0.13.9（记录稳定化）与v0.13.10（lambda与闭包），dev侧也在0.13.11成版后推进了v0.13.12的先行面（web框架Tasks 1–7与D-29名称解析修复随尖端一并入树）。因此第二、三节的预演清单与冲突判定按合并当日的实际分叉重新执行，不以本文所载为准；第七节裁决2所述的两线再分叉按原样成立，只是分叉时点的版本号改为「master带着0.13.11合并树、dev继续v0.13.12」。本文其余部分作为合并方法的历史记录保留。
