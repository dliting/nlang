# 门禁写目录互斥（tools/gate.sh）概要设计

## 一、引言

v0.13.8收官批的一次门禁误红是这样的：两条`--shared`腿（同一棵build-dev的Debug与Release重建）并发起跑，两条CPack配置写序程抢`build-dev/CPackConfig.cmake.tmp`，报`Permission denied`、rc=127，build-dev于是留着0.13.8的旧二进制，连带5条`version_*`单测假失败（台账`.superpowers/sdd/2026-11-15-json-library/progress.md:141`，原始日志`temp/gate_json_t12_rebuild_release.err:1-3`）。当时的处置只改了批次编排——重建腿跨树配对（同一条目:142），机制本身没动，也就是说下一次还会有人踩同一个坑。

用户裁定（2026-11-15）：这类问题要从机制上改进，让`tools/gate.sh`自己拦下「两条构建门禁写同一个目录」，而不是靠人记住配对规则；排在门禁绿、第三方评审与v0.13.11收尾提交之后做（同一条目:147）。

本文是概要设计（Spec）：钉死目标范围、机制裁定与行为面；逐任务细化留给实施计划。文风与结构仿`docs/dev/json_library_design.md`。

## 二、目标与范围

### 2.1 本版做

- 给`--shared`（只构建、不测量）腿加一道「写目录锁」：目录从该腿命令行里cmake/ctest/nlang_docs显式命名的目录导出，写同一目录的两条构建腿串行，写不同目录的照常并行；
- 持有者信息、陈旧接管、等待与获锁日志行，全部沿用既有机器锁的格式与纪律（`LOCK `前缀、判决行原样）；
- 新增自测`tools/gate_selftest.sh`：以事件先后（不是时长）断言「同目录串行、异目录并行」，并注册进ctest；
- 同步`tools/gate.sh`头部注释，并把仓内全部按行号引用`gate.sh`的地方改成按函数引用（四处，分布在两个文件，见九）。

### 2.2 本版不做

- 不改机器锁（exclusive/shared）的既有语义，也不改`--shared`标志的含义；
- 不做目录前缀包含判定：父目录与子目录互不排除（理由见D-9）；
- 不为「读另一个目录里的构建产物」（如docs腿读build-dev里的`ncc.exe`）加锁：那是文件级读写竞态，与本次要根治的目录级写写竞态不同源（D-9）；
- 不要求每条`--shared`腿都能导出目录（D-7）；
- 不重构构建树布局（每配置一棵树）——见方案比较C；
- 不提VERSION、不写CHANGELOG、不动用户手册：五类用户可见面均未触及（见九）。

### 2.3 落点

纯贡献者工具改动，占dev线一次独立提交，不占roadmap版本号；排在v0.13.11收尾之后、v0.13.12（WEB框架）开工之前。

## 三、关键事实（勘察所得）

1. `tools/gate.sh:28-32`已把`--shared`描述成「只写自己的构建树、可与另一条`--shared`腿并存」。契约里「自己的」这一条从来没有被检查过——本次补的就是这个enforcement，而不是新添语义。
2. 机器锁的现有实现：独占认领靠`$lock_dir/exclusive/`的原子`mkdir`（第167行），构建腿是`$lock_dir/shared/`下每个holder一个`.info`文件（156-157行）；`write_holder`（83-92行）写`name`/`pid`/`started`/`dir`/`mode`/`cmd`六行，`holder_age_s`（104-111行）读`started=`，`sweep_stale`（116-132行）按`LOCK_STALE_S`（默认10800s）接管并点名被顶替者；等待行与获锁行（137-143、158、169行）一律带`LOCK `前缀；锁根可由`NLANG_GATE_LOCK_DIR`改址（第60行）。认领循环每轮都先重建锁根（148-151行注释写明：临时清理器可能在等待期间把根目录删掉，缺这一步，原子认领会永久失败）。
3. 本机Git Bash（bash 4.4.23，MSYS）里`flock`不存在（`command -v flock`无输出），`cygpath`、`realpath`、`sha256sum`、`mktemp`都在`/usr/bin`。
4. 目录旗标的拼法实测：
   - `cmake --help`只列空格分开的`--build <dir>`与`-B <path-to-build>`；`cmake --build=DIR`实测报`Ignoring extra path from command line`。
   - `ctest --test-dir <dir>`存在；`ctest --test-dir=DIR`更糟——没有任何警告，直接改用CWD并回rc=0（实测：`Test project E:/cases/nlang/dev`＋`No tests were found!!!`＋rc=0）。
   - `nlang_docs`的目录旗标是`build`/`check`的`--site-dir`与`snippets`的`--workdir`（`tools/nlang-docs/src/nlang_docs/cli.py:85,107,121`），由argparse解析，因而`--site-dir=DIR`是其**合法**拼法（实测输出里就打出该目录）。
5. 今天所有`--shared`腿的命令形态（全仓`--shared`调用点汇总：`temp/*.sh`、`tools/http_perf/probe_pair_ab.sh:49`、`docs/dev/http_perf_baseline.md`配方）只有三类：`cmake --build <tree> --config … [--target …]`、`python -m nlang_docs build|check --site-dir <dir>`，以及一条不带任何目录旗标的docs片段pytest腿。后一类今天就存在：它写的是每次一换的mkdtemp暂存目录（`cli.py:213,258`、`snippets.py:352`），与别人不撞；仓库根的`.pytest_cache`虽是共享写入，但写入良性，且这类腿今天本就串行编排。
6. 构建树根被所有配置共用：错误日志点名的正是树根的`build-dev/CPackConfig.cmake.tmp`（`CPackSourceConfig.cmake`同理，另有`CMakeCache.txt`），不是每配置子目录各一份。这就是「同一棵树两个配置并发」会撞的物理原因。构建腿的POST_BUILD拷贝都落在`$<TARGET_FILE_DIR:…>`里（`CMakeLists.txt:151-167,182-193`），即各自树内，不跨树。
7. 路径归一化实测：`cygpath -m build-dev`原样吐回相对路径，要`cygpath -ma build-dev`才得`E:/cases/nlang/dev/build-dev`；`realpath -m build-dev`得`/e/cases/nlang/dev/build-dev`。
8. 全仓按行号引用`gate.sh`的地方有四处两个文件（`git grep -n 'gate\.sh:[0-9]'`）：`docs/dev/http_perf_baseline.md:710、798`与`tools/http_perf/doc_template.md:637、725`。后者是基线页再生成的模板，漏改就会把旧行号再灌回前者。

## 四、调研结论与出处

- shell级互斥的两种标准做法是`flock(1)`与原子`mkdir`。POSIX文件系统上`mkdir()`是原子的，认领必须用不带`-p`的单次`mkdir`（`mkdir -p`不保证「已存在即失败」），释放用`rmdir`；网络文件系统上`mkdir()`可能不原子（unix/SE答复，镜像`cloud.tencent.com/developer/ask/sof/116584365`、`cloud.tencent.com/developer/ask/sof/105941096`）。本机锁根在`%TEMP%`本地盘，不在网络盘上。既有机器锁已经是这个形状，目录锁沿用同一机制，一个脚本里只有一种锁法。
- 「一棵构建树同一时刻只有一个构建」是CMake社区的常规前提：多配置的正路是多棵构建树，同一文件系统位置并发构建会撞（Xcode生成器的`build.db: database is locked — Possibly there are two concurrent builds running in the same filesystem location`，`cloud.tencent.com/developer/ask/sof/107865466`；多配置构建问答`cloud.tencent.cn/developer/ask/sof/102367408`）。本仓库选了多配置共用一棵树（省磁盘、省配置时间），代价就是要由门禁保证同树串行——这正是本次要做的事。

## 五、方案比较

- **A 按写目录互斥（选定）**：目录从命令行导出，目录级互斥，跨树并行度完整保留。代价：目录得从argv导出，导不出时退回现状（且日志可见）。
- **B 构建腿一律串行**：零新机制，但把三棵独立树的并行重建也交出去了——机器锁当初正是为这个并行度设计的（`tools/gate.sh:19-26`）。收缩功能需要独立理由，这里没有。
- **C 每个配置一棵树**：能根治「共用树根CPack文件」，但要改构建树布局、门禁编排、e2e的二进制路径与全盘脚本，影响面远大于本次。门禁之外的入口这一项是C的真实优势：手工跑的构建与IDE后台构建根本不经过`gate.sh`，目录锁拦不到它们，而C的「每配置一棵树」在文件系统层面就消除了跨配置并发（各配置写各自目录），只有同配置并发两案相同。本方案不掩饰这个缺口，选A的理由与之无关——天天相撞的场景发生在门禁批里（本次事故就是），而`gate.sh`的职责范围也正是门禁批；门禁之外怎么摆构建树属于贡献者约定，不该由脚本代管，代价只是手工/IDE的跨配置并发仍靠习惯避免。
- **D 只加一个必填旗标让作者声明写目录**：实现最省，但失败模式恰恰是「人忘了声明」，本次要修的正是这个。导出为主、环境变量可覆盖（D-3）比「必须记得」稳。

## 六、架构裁定

### D-1 互斥资源＝命令自己声明的目录，读写一视同仁

docs的`check --site-dir`是读者、`build --site-dir`是写者，同目录并发同样会读到半成品站点。区分读写要两套锁语义，换来的只有「两个check可并行」，而docs腿本来就串行编排（计时帽是机器级的）。故一把锁，读写都算。

### D-2 只有`--shared`腿取目录锁

独占腿起跑前要等`shared/`清空、起跑中独占整机，因此任何不是「shared对shared」的配对都已被机器锁全序化，再加目录锁是空转。此裁定有独立理由，不是「现状如此」：目录锁要防的是两本脚本各自合法（都带`--shared`）却写同一目录。

### D-3 目录集合的导出规则

按argv顺序扫描目录旗标，两种拼法都认：长旗标的空格式与`=`连接式（`--build DIR`/`--build=DIR`、`--test-dir DIR`/`--test-dir=DIR`、`--site-dir DIR`/`--site-dir=DIR`、`--workdir DIR`/`--workdir=DIR`），短旗标`-B DIR`与`-B<DIR>`。环境变量`NLANG_GATE_WRITE_DIR`指定的目录并入集合（为argv看不见的工具预备，如手工跑的msbuild/ninja）。

为什么`=`连接必须支持：对cmake/ctest它本来就是坏调用（三.4：cmake警告、ctest静默改用CWD），拦下总比漏过强；对`nlang_docs`它是argparse认的合法拼法，若不支持，`--site-dir=temp/en`这条腿跑得好好儿的却一把锁都不取，与同目录的另一半并不串行——正是方案D被否决的那个失败模式。多一个分支，不多一处坑。

`NLANG_GATE_WRITE_DIR`的使用纪律：只用逐次前缀形式（`NLANG_GATE_WRITE_DIR=<dir> tools/gate.sh …`），**不得export**。批脚本里的export会随环境继承给后面每条腿，让写别的树的腿也白等一把无关的锁；获锁行会把取到的每个目录都打出来，这种白等看得见，但仍要避免。

### D-4 路径规范化

Windows下用`cygpath -ma`（实测`-m`不做绝对化，`-ma`才得`E:/…`正斜杠绝对形）再转小写——Windows大小写不敏感，`build-dev`与`BUILD-DEV`是同一棵树。无`cygpath`时（Linux移植预备）用`realpath -m`：`-m`不要求路径存在（`cmake -B`正是要新建目录），且**不转小写**——POSIX大小写敏感，`/Build-Dev`与`/build-dev`是两棵树，折叠成一把键就是假串行。相对路径按gate.sh的CWD解析——gate.sh不chdir，其CWD就是调用者的CWD。

### D-5 一把腿可持多把目录锁，全局同序获取

对集合内去重后的每个目录都上锁，按锁键字典序依次获取（全局同序是无死锁的充分条件），释放逆序。获取次序全脚本一致：机器锁在先、目录锁在后。于是一条腿同时写两棵树（例如`cmake --build A`后再`ctest --test-dir B`）也被完整覆盖，无需「只认第一个旗标」这种隐性收缩。今天的腿还没有这种形状，故这条机制的覆盖面必须由八.2的两树断言与突变核查兜住，不能只靠论述。

### D-6 锁文件布局与陈旧接管

`$lock_dir/wdir/`为根（随`NLANG_GATE_LOCK_DIR`一起改址，自测因此可以整体搬进私有目录）；每个目录一把锁＝原子`mkdir "$lock_dir/wdir/<key>"`（不带`-p`，理由见四），其内`holder.info`沿用`write_holder`的六行格式并多写一行`path=<真实目录>`。每轮认领前先`mkdir -p "$lock_dir/wdir"`——与`tools/gate.sh:148-151`同一个理由：临时清理器可能在等待期间把父目录删掉，缺这一步，原子认领会永久停在ENOENT上，正是那段注释防的挂死。陈旧判定与接管复用`holder_age_s`与`LOCK_STALE_S`。

### D-7 无目录可导出的`--shared`腿：警告并继续

打印一行说明「未从命令行导出写目录、未取目录锁」后照常执行。错误化会打断今天就合法的腿（三.5），而那类腿根本没有跨腿冲突可防（各自的mkdtemp暂存不共享；仓库根的pytest缓存写入良性，且这类腿今天串行）。

### D-8 锁键＝规范化路径的sha256前16位十六进制

键要进文件系统当目录名：Windows有路径长度上限，路径里还有空格与`:`。日志与holder一律打真实路径，人读得懂；哈希只当目录名用。

### D-9 不做前缀包含判定，也不锁跨目录的文件读

`build-dev`与`build-dev/src`互不排除，因为没有腿把写目录指进另一棵写树里面（今天全部形态里，docs两站是`temp/en`与`temp/zh`这样的兄弟目录）；做包含判定要在多种拼法间比父链，复杂度换不到现实收益。holder已记`path=`，日后真要加规则不必改格式。同理，「docs腿读build-dev里的`ncc.exe`」这类跨目录读写竞态不在本次范围：它是文件级的，且`--ncc`是文件不是目录——锁它的父目录也与`--build build-dev`的锁键不同，锁了也拦不住。

### D-10 自测只断言事件先后，不断言时长

按确定性测试设计裁定（2026-11-14）：不给时间赌局加时间护栏。自测用私有`NLANG_GATE_LOCK_DIR`（绝不排在真门禁后面排队）、`NLANG_GATE_POLL_S=1`；桩命令是一个只认`--build DIR`与`--site-dir DIR`两式并记录事件的shell脚本，由`bash`直接调用，起跑时在事件文件记一行、直到GO文件出现才收工——不篡改PATH也不假冒cmake可执行文件（Windows下PATH桩受PATHEXT与无扩展名执行限制，代价不可控）。桩与被测的导出逻辑共用同一套旗标拼法，所以**桩用到的那两式**导出规则确被检验，其余旗标由八.3的突变核查兜底。驱动脚本一律条件轮询（等期望日志行、等holder文件），断言取自日志事实与事件文件里的先后次序。整体挂120s哨兵（常量命名），哨兵触发即判失败并报出当时的holder。

### D-11 注册进ctest，找不到bash就响亮跳过

`tests/CMakeLists.txt`里`find_program(BASH_PROGRAM bash)`，命中才注册`add_test(NAME nlang_gate_selftest COMMAND ${BASH_PROGRAM} ${PROJECT_SOURCE_DIR}/tools/gate_selftest.sh)`，配`WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}`与`TIMEOUT 180`；未命中时`message(STATUS "… skipped: bash not found")`，与`nlang_docs_pytest`缺依赖时的处理同形（`tests/CMakeLists.txt:2796`注册、`:2805`的`TIMEOUT 180`、`:2807`的skipped消息）。180s是这类脚本腿的房值，也是AGENTS「测试超时设短（最长3min）」的上限本身；超过它的少数条目（`:669`的900s、`:2763`的300s、`:2778-2779`的420s）都是经用户裁定确立的例外，那几处上方的注释记着裁定与实测数据，本条目不属于例外，故取180s。这是第一条以bash注册的ctest条目；缺席只是少一条门禁条目，不是构建失败。

### D-12 判决行一字不改

`GATE <name>: PASS|FAIL rc=`原样保留，目录锁的所有行都用`LOCK `前缀，「只认判决行」的纪律不受影响。

## 七、行为面

新增/改变的只有日志行，格式为：

- `LOCK <name>: write-directory lock acquired after <n>s (<dir>)`
- `LOCK <name>: waiting for the write-directory lock on <dir> (<holder>)`（首轮），其后每`LOCK_REPORT_EVERY_S`一行`LOCK <name>: still waiting <n>s (<holder>)`
- `LOCK <name>: taking over the write-directory lock on <dir> held <age>s by <holder>`
- `LOCK <name>: no write directory derived from the command line, no directory lock taken`

退出码语义不变（仍是被包命令的rc），`GATE`判决行不变，既有`LOCK`机器锁行不变。用法行与`--shared`的说明随之更新。

## 八、测试策略

1. **TDD顺序**：先写`tools/gate_selftest.sh`，在当前`tools/gate.sh`上跑到RED（缺等待行、事件文件显示重叠），再实现目录锁转GREEN。
2. **自测的断言面**：
   - 同树两拼法（`--build <abs>`对`--build <rel>`）→ 第二条腿必出现等待行，事件文件里第二条的起跑晚于第一条的收工；
   - 同树大小写差（仅Windows生效；Linux大小写敏感，这条在POSIX下自动跳过，见D-4）→ 同上；
   - 异树（T1对T2）→ 第二条腿无等待行，事件文件里两条起跑重叠；
   - 两树对单树（一条腿同时写T1与T2，另一条只写T2）→ 必串行，且获锁行里两把锁的先后严格按锁键字典序——这一条是D-5的覆盖面。为使该断言能区分「按锁键排序获锁」与「按argv顺序获锁」，驱动先把两目录按锁键排序、再以**逆序**写进argv（argv顺序正是全局排序要防的那种交叉形态）；锁键次序从holder文件配对读出，不复制哈希配方（目录名即锁键、`path=`行即目录，见D-6），但映射必须在上一条异树用例的重叠窗口里建立——那时T1与T2两个holder同时在场，列一次`wdir/*/holder.info`就够了；不能改用两树腿自己持锁期间的那次listing，因为argv在起跑前就得定，那次listing只能用来核对获锁行的先后。两树腿写作`--build <目录> --site-dir <目录>`：真实命令里一条腿涉及两类目录旗标就是这一种形态，重复两次`--build`不是合法的cmake命令。启动顺序固定为先起两树腿（它的起跑事件在两把锁都持住之后才落，见D-10）、再起单树腿并断言其等待行、最后放GO；反序启动会让两树腿被单树腿挡在T2上，获锁行根本打不出来；
   - `NLANG_GATE_WRITE_DIR`覆盖 → 与命令行目录同键时照样串行；
   - 无目录可导出的`--shared`腿 → 必出现「no write directory derived」行且腿照常PASS。
3. **突变核查（temp/探针，不入仓）**：从旗标集合里删掉`--build`、再删`--site-dir`，两轮自测都必须转红（证明旗标集合每个成员都真被检验）；把`cygpath -ma`退回`-m`（绝对化失效），同树两拼法那条必须转红。
4. **陈旧接管**：把`wdir/<key>/holder.info`的`started=`改老再起跑，必出现接管行——由temp/探针验证并记台账，不进自测（它会额外等一轮）。
5. **门禁口径**：`tests/CMakeLists.txt`变了，三树都要重新configure。Debug（断言）树与IDE树各跑全量ctest（含新条目）：改动落在测试注册本身，只有全量跑才能证明它没碰坏同一棵树里其他条目的注册与超时属性。ASan树按例行口径重建并跑全量`ctest_asan`（含新条目）——它是例行门禁三树之一，跑全量本就是这棵树的例行默认，省掉才是要另加理由的偏离；而「CMake文件变更」正是已获认可的窄复扫先例（`.superpowers/sdd/2026-11-15-json-library/progress.md:132-134`）明列的ctest腿失效条件：那批省掉ctest腿是因为它没动CMake文件，这批动了，不能照抄那份省略。e2e不跑：本批无`.n`、二进制或产物变更，按用户裁定（2026-11-08）e2e只在发布时点执行一次。门禁范围属于用户口径，本条随本spec一并请用户确认；其中唯一值得商酌的是ASan全量（本批最贵的一条腿）：若同意省掉，只留新条目单跑，代价是「注册改动碰坏ASan树其他条目」这类信号一并消失。

## 九、集成点与文档同步

- `tools/gate.sh`头部注释补目录锁一段，把`--shared`的说明从「只写自己的树」升级为「只写自己的树，且脚本会按目录拦下两条写同树的腿」；env清单补`NLANG_GATE_WRITE_DIR`与其「只逐次前缀、不得export」的纪律。
- 行号引用一次性根治而不是逐处更正：`docs/dev/http_perf_baseline.md:710、798`与`tools/http_perf/doc_template.md:637、725`这四处（后者是前者再生成的模板源）改成按代码特征引用而不是按行号——「`tools/gate.sh`的`acquire()`机器锁认领段」（今天落在`:148-176`）与「`$stem.out`/`$stem.err`按第二个参数命名处」（今天落在`:198`的`"$@" >"$stem.out" 2>"$stem.err"`）——从此脚本增减行不再制造漂移。第二处的原文说的是门禁产物按stem命名，不是`GATE`判决行的打印点，锚点必须照此写，否则再生成的基线页会把引用挪错地方。改完用`git grep -n 'gate\.sh:[0-9]'`复核为空。
- 记忆条目`nlang-gate-paths`刷新（新增锁键来源与自测位置）。
- 不提VERSION、不写CHANGELOG、不动用户手册与`SYNC_BASELINE`：本次只改贡献者工具，语言面/标准库/诊断文案/退出码/ABI五类均未变。先例上`tools/source_size_guard`曾以ctest条目形式进过CHANGELOG（0.7.4，`CHANGELOG.md:3197-3203`），差别在于那一版同时发布手册页与CONTRIBUTING指南，用户确实能感知；本批用户拿不到任何新东西。代价：发布说明少一条贡献者条目，真需要时在v0.13.12段补一行即可。

## 修订记录

- 2026-11-15 v1：初稿（自主开发流程的Spec阶段）。
- 2026-11-15 v2：落实第一轮第三方评审（0 Critical / 4 Important / 6 Minor）。四处Important：`=`连接拼法必须支持（否则docs腿静默无锁；cmake/ctest的`=`实测各自坏法已补进三.4）、D-5多目录锁补上两树断言与`--site-dir`突变（八.2、八.3）、方案C否决理由里那句写反的「更普适防护」改为实话（五）、行号引用面补上`tools/http_perf/doc_template.md`并改为按函数引用（三.8、九）。六处Minor：`cygpath`改`-ma`且大小写折叠限定Windows（实测，三.7、D-4）、`NLANG_GATE_WRITE_DIR`禁export（D-3）、门禁窄范围标为待用户确认并引先例（八.5）、`wdir/`每轮认领前重建父目录（D-6）、三.5「本无冲突」改为准确表述（补`.pytest_cache`）、`nlang_docs_pytest`先例的引用行号更正为2796/2805/2807。
- 2026-11-15 v3：落实第二轮第三方评审（0 Critical / 3 Important / 1 Minor，另有一处引用标签写错），四处均由我另行实测复核。八.5门禁口径同一条里既写「全套e2e」又写「e2e不涉及」，且把ASan例行口径列错（例行批实为重建＋全量`ctest_asan`＋`e2e_asan`，见`progress.md:145`），改为逐腿列举并各自给出跑或不跑的理由；八.2的两树断言补上「argv按锁键逆序传参」与固定启动顺序，否则按argv顺序获锁的实现也能过、且反序启动时获锁行根本打不出来；五的方案C那句被改坏的「两说都不成立也不成立」重写，承认「每配置一棵树」对门禁之外入口的结构性优势，再给A的独立理由。Minor：D-11「均在AGENTS的3分钟上限内」不实（实测`tests/CMakeLists.txt:2763`为300s、`:2778-2779`为420s、另有`:669`为900s），改为「180s就是上限本身，超限条目是经用户裁定确立的例外」。九的第二处锚点由「判决行打印处」更正为`.out`/`.err`按第二个参数命名处（引用站点原文即如此）。
- 2026-11-15 v4：落实第三轮第三方评审（0 Critical / 0 Important / 1 Medium）。该Medium是我在v3新写的八.2里造出来的循环依赖：锁键次序号称从两树腿持锁期间的holder listing读出，可argv在起跑前就定了，同一次listing不能既定argv又核次序；改为映射在前一条异树用例的重叠窗口建立（T1、T2两个holder同时在场），持锁期间那次只用于核对获锁行先后，并顺手钉死两树腿的旗标拼法为`--build`＋`--site-dir`（重复`--build`不是合法cmake命令）。八.5的ASan全量裁定获评审方独立复核并补了一条更硬的理由：全量本是该树例行默认，省掉才是需裁定的偏离。第一轮遗留的三处Minor（m8/m9/m10）经核全部关闭。
