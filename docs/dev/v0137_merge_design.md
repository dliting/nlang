# v0.13.7 master/dev合并概要设计

## 一、背景与目标

仓库自0.12.9发布（8bc79ee9，11-07）起双线并行：

- **master线**：泛型完善系列（0.13.1a静态引用T全面化＋游离函数泛型、0.13.1b结构体值相等批次、0.13.1c抽象函数＋泛型接口），共77个提交未进dev。语言面还包括：拼写歧义具名指向探针、数值宽度别名全家族、试验保留字`elseif`/`state`移除、单进程编译运行宿主的描述符驻留池根治。线格式NCU v2.14→v2.19（泛型类尾、接口尾、抽象旗标）。VERSION=0.13.1c。
- **dev线**：0.13.4日期时间并入、0.13.5 HTTP库、0.13.6 net/http性能优化、0.13.7进行中（位运算`& ^ | ~`＋复合赋值已落，regex库Task 3-12已落、13-18待做）。线格式停留在v2.14。VERSION=0.13.7。

目标：master并入dev后单线承载0.13.7交付，例行四门禁全绿，到达发布时点流程（终检、制品、推送、本地部署）。合并是roadmap 0.13.7条目第3项，不在dev侧计划内，归master会话执行。

## 二、事实盘点

- 冲突面：master相对祖先改336个文件，dev改437个，交集78个。热点家族：语法（nlang.y/nlang.l）、VM后端（BytecodeOps.h、OpCodeTable.cpp、Disassembler.cpp、EmitCall.cpp、EmitExprBinary.cpp、VmExecutor*）、手册specification/vm-architecture的en/zh成对页、测试（test_nculoader、test_generic_decl、test_vm、manifest.txt、CMakeLists）、CHANGELOG/VERSION/roadmap/SYNC_BASELINE。
- opcode：master追加7件（OP_AllocArrayN、OP_AssembleArrayN、OP_CheckCast、OP_CallStaticOut、OP_Eq_struct、OP_Ne_struct、OP_CheckArrayType）并修改OP_CallStatic与OP_New的操作数布局（追加descIdx）。dev追加位运算族4件（OP_BitAnd/BitOr/BitXor/BitNot）。两侧都在枚举尾部追加，直接合并必然编号碰撞。
- 线格式：master 14→19；dev未动。合并取19。
- 版本序列交错：CHANGELOG按版本号降序排列后，master的0.13.1c/b/a三条目插在0.13.4与0.12.9之间，条目内日期保留真实时间线。
- 先例：dev侧已有四次「Merge master into dev」（11-06/11-07共四笔），方向与机制成熟。

## 三、方向与时机

- **方向**：在dev树执行`git merge master`。理由：dev是0.13.7交付线，历史上release都从dev侧tag；master在发布后快进到发布提交即可收敛，两线归一。
- **取数**：master的提交全部未推送，dev须从本地路径取（`git fetch E:/cases/nlang/master master`后合并FETCH_HEAD），不经过远端。
- **时机门**：dev计划Task 18（收尾）落盘且台账关闭、dev工作树干净之后才进入dev树。dev会话活跃期间绝不触碰dev树（今天dev仍在提交regex任务）。
- **执行者**：master会话（本会话）。

## 四、冲突面分类与裁决

1. **语法**（nlang.y/nlang.l）：并集。master侧：保留字移除、数值宽度别名、泛型implements专用产生式ImplementsName、泛型拼写与rescue机制；dev侧：位运算token（注意`&`/`|`与`&&`/`||`的最大 munch）与优先级行、复合赋值。重建后bison/flex的冲突告警必须与合并前dev线的基线一致（零新增）。
2. **解析层**（ExprResolver.h/ExprResolverBinary.cpp/StatementResolver*.h/cpp/SnExpressions*.h/cpp/include头）：两侧改的是不同函数族（master＝泛型mint与边替换、值相等；dev＝位运算解析），按函数级并集；同函数双改的冲突块逐个人工判读，以语义完整一侧为基并入另一侧语义。
3. **opcode表**（BytecodeOps.h/OpCodeTable.cpp/Disassembler.cpp/VM switch/EmitCall.cpp/EmitExprBinary.cpp）：以dev现有编号为基（0.13.7是当前交付线），master新增的7件接排在dev之后统一重排；OP_CallStatic/OP_New的操作数布局以master为准（descIdx贯穿），dev侧所有emit位点适配新布局。不向下兼容，无跨版本产物兼容负担，全部产物重建后由e2e全量兜底；Disassembler与ndisasm同步。
4. **线格式**（CompiledModule.h/test_nculoader.cpp）：`NCU_FORMAT_MINOR=19`。test_nculoader以master的v2.19值绑定pin为基，并入dev对该文件的新增测试。
5. **手册**（specification与vm-architecture的en/zh成对页）：逐页并集。operators.md＝master修改＋dev位运算节；lexical/grammar页＝master关键词变化＋dev运算符；bytecode-instructions页＝两侧新增指令。合并后SYNC_BASELINE重锚（合并提交本身进基线计算）。
6. **CHANGELOG/VERSION**：VERSION=0.13.7；CHANGELOG以dev结构为基（无Unreleased空壳），master三条目按版本号降序插入，条目内容原样保留；dev的[0.13.7]条目补一行合并说明。
7. **测试面**：manifest.txt并集；各CMakeLists并集；e2e/embed-host runner脚本并集；两侧新增测试全保留（master的泛型/抽象/值相等族＋dev的位运算/regex族）。
8. **stdlib**：io.n注释级差异并集；dev独有的regex.n/http/gzip包原样；master无stdlib新增。合并落地后std库包随构建重打包。
9. **文档与工具**：roadmap并集（dev的0.13.7进行态＋master的0.13.1c交付条目与递延并入项）；probe_run.py、source_size_guard allowlist并集；nlang-docs两侧修改合并。

## 五、验证与收尾

- 合并树全量重建（Debug、ASan、IDE三树＋e2e工具与stdlib包）后按例行门禁跑四腿（Debug＋ASan＋IDE ctest，e2e串行），判决只认tools/gate.sh判决行。
- 任一腿红＝修复后整批作废重扫（既有纪律）。
- 四腿全绿后：SYNC_BASELINE重锚、roadmap登记合并完成态。
- 发布时点流程随后执行（0.13.7的发布点即合并完成点）：Release构建＋Release e2e终检兼制品生产、推送dev与tag、master快进到发布提交、本地部署d:/dev/nlang并哈希对拍。
- 合并后即可启动的两项既有设计实现（并发导入边界重设计、Exception不可变＋cause链），二者规格早已备好且都动builtins.n，此前被「dev落地前不碰stdlib」的裁定押后，合并清障即解押。

## 六、风险与回退

- 合并中途失败：`git merge --abort`回到dev tip，重跑无副作用。
- opcode重排的隐性错位：四树ctest＋e2e全量兜底；重建时要求看到真实编译行（测试名≠目标名的假绿教训）。
- 手册逐页并集遗漏：manual_sync_check以基线漂移清零为发布门禁兜底。
- 与dev会话并发：时机门是唯一防线，进dev树前`git status`必须干净、台账必须关闭。
