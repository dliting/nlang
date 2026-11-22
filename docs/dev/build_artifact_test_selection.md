# 构建产物依赖图与按产物选测试

性质：分析记录，不是概要设计。日期2026-11-22，工作树`dev`分支`bff964d9`。
问题来自用户提出的一条选测原则：业务代码改动后，CMake能判定哪些构建产物需要重建，
测试只需重跑依赖这些产物的那些，集成测试同理（例如`net`这个包相关的代码改了，
`vm`的测试不必重跑，发布前也一样）。本文用实测回答三件事：这张依赖图长什么样、
按产物选测在哪些地方真能省下时间、要让它在集成测试层成立还缺什么。

探针与原始输出：`temp/dag_probe.py`（读全部`CMakeLists.txt`，抽目标定义与
`target_link_libraries`/`add_dependencies`边，算传递闭包与扇入）、
`temp/dag_entries.py`（再把ctest条目名接到可执行目标、把可执行目标接到其闭包、
把条目接到`tests/gate_time_reference.txt`的实测秒数）。
完整输出见`temp/dag_report.txt`与`temp/dag_entry_report.txt`。

## 一、结论摘要

1. 图是真实的、可机读的：136个CMake目标、成品清单明确、每个ctest条目都能接到它链接的库集合。
2. 在已有边界处按产物选测很划算：IDE族（15条目141.5秒，占`ctest-IDE`12.8%）、
   `nlang_embed`（4条目3.4秒）、`nlang_tensor_kernels`（3条目1.5秒）、
   七个原生模块DLL、`stdlib.npkg`——这些改动都能把选测集缩到百分之几。
3. 在核心四库内部几乎省不下来：可由链接图定界的126条目共323.3秒里，
   含`nlang_langservice`的139条目占320.4秒（99.1%）。改`src/vm/**`就是改几乎所有单测，
   这不是选测机制的缺陷，而是编译—运行时—VM—语言服务四者确实互相依赖。
   现行`tools/test_families.txt`故意把这几片留成未映射、由rc=3保守回退跑全量Debug单测，
   与图上的事实一致。
4. `net`这个例子恰好说明链接图的边界：net的原生代码在独立DLL里（v0.13.0 net task 6移出`nlang_vm`），
   没有任何单测链接它——它与测试之间是「运行期装载」关系，CMake的链接图里没有那条边，
   只有`tests/CMakeLists.txt:1409`一条手工`add_dependencies`。所以「改net只跑net的用例」
   这个正确结论，不能靠读链接图得到，必须补声明或实测记录。
5. 集成测试层目前登记的粒度不支持这条原则：`tests/e2e/`有2344个`.n`用例，
   参考表里e2e只有套件级行（`default`一条）。要按产物裁剪集成测试，缺的是
   「逐用例→它测哪个包/哪个模块」的映射，而不是CMake的靶子新鲜度。
6. 「发布前也照此裁剪」有一个前提冲突：发布门禁是净室全新构建树，CMake在那里判定
   所有产物都需要重建。要让发布时的裁剪有意义，选测的依据必须是产物的内容指纹，
   而不是目标的陈旧状态；否则该规则在发布点退化成全量（即现状）。

## 二、事实基座

| 编号 | 事实 | 证据 |
|---|---|---|
| F-1 | 成品清单：七个可执行`ncc`、`nvm`、`ndb`、`ndisasm`、`nlang`、`nide`、`embed_host`；字节码制品`stdlib.npkg`由自定义目标`nlang_stdlib_package`产出（`:298`用`ncc build -p stdlib.nproj`）；原生模块七个DLL，`set(NLANG_NATIVE_MODULES math io fs tensor net system gzip)` | `temp/dag_report.txt`「target definitions」段；`CMakeLists.txt:275-304`；`src/native/CMakeLists.txt:12` |
| F-2 | 四个核心库互相闭环并位于所有成品之下：`ncc`闭包为`nlang_compiler`＋`nlang_langservice`＋`nlang_runtime`＋`nlang_version`＋`nlang_vm`＋`tinyxml2`（`nlang_langservice`反向被`nlang_compiler`链接，见`src/compiler/CMakeLists.txt:191`）；`nvm`/`ndb`/`ndisasm`/`nlang`同集合少`tinyxml2`；`nide`再加九个`nide_*`库、`vterm_static`以及`ncc`/`nvm`本身 | `temp/dag_report.txt`「product closures」段 |
| F-3 | 全部ctest条目在`ctest-Debug`构型登记154条、实测合计796.9秒；`ctest-IDE`185条1103.7秒、`ctest-ASan`179条2550.4秒、`e2e-Debug`8行254.4秒 | `tests/gate_time_reference.txt`（`temp/dag_entries.py`汇总） |
| F-4 | 单测可执行对核心库的接入面：77个链接`nlang_compiler`、78个`nlang_langservice`、77个`nlang_runtime`、71个`nlang_vm` | `temp/dag_report.txt`「fan-in」段 |
| F-5 | 在条目名能经可执行目标定界的126条目（323.3秒）中，含`nlang_langservice`的139条目耗320.4秒（99.1%）、含`nlang_runtime`与`nlang_compiler`的各319.2秒（98.7%）、含`nlang_vm`的319.0秒（98.6%）、含`nlang_version`的64条目63.9秒（19.8%） | `temp/dag_entry_report.txt` |
| F-6 | 定界不到的28条目473.5秒，其COMMAND不是单测可执行而是成品或脚本：`e2e_runner_timing_gate`一条409.77秒（占整个Debug登记量的51%）、`nlang_gate_selftest`30.59秒、`nlang_docs_pytest`16.09秒，其余为`nvm_*`/`ncc_*`/`ndb_*`/`ndisasm_*`/`version_*`命令行冒烟。它们按成品定界，闭包同样是核心四库 | `temp/dag_entries.py`未匹配行列举；`tests/CMakeLists.txt`相应`add_test` |
| F-7 | 已有边界上的产物确实小：`nlang_embed`4条目3.4秒、`nlang_tensor_kernels`3条目1.5秒、`tinyxml2`47条目8.6秒（占Debug实测2.7%）；IDE侧九个`nide_*`库合计15条目141.5秒（占`ctest-IDE`的12.8%），其中`mainwindow_tests`独占115.60秒 | `temp/dag_entry_report.txt` |
| F-8 | 原生模块DLL与测试之间没有链接边：模块只依赖C ABI头（`src/native/CMakeLists.txt`文件头自陈"it does NOT link nlang_vm"）。`tests/CMakeLists.txt`里与模块相关的边只有四类形态——`:1409`唯一一条`add_dependencies(test_module_state nlang_net)`；`:2558`把`-DNATIVE_DLL_DIR=$<TARGET_FILE_DIR:nlang_math>`编进测试；`:3428-3441`（`nlang_embed_deploy`）与`:3468-3476`两个自定义目标把DLL拷进部署目录当fixture；`:266`把`nlang_io`链给加载器测试当样本 | 两处`CMakeLists.txt`原文 |
| F-9 | net的六条单测在Debug实测合计2.0秒（占该构型0.3%）：`test_net_platform`与`test_net_slots`除自身外不链任何库，另四条链核心四库 | `tests/gate_time_reference.txt`；`temp/dag_report.txt` |
| F-10 | 集成语料与登记粒度不匹配：`tests/e2e/`下2344个`.n`用例，参考表的e2e行按套件级登记（`default`、`stdlib_regex_size`加六条定点探针），无逐用例秒数 | `tests/e2e/`计数；`tests/gate_time_reference.txt`的e2e行 |
| F-11 | 两处会让「CMake判定谁需要重建」失真：`nlang_stdlib_package`是`add_custom_target(... ALL ...)`，CMake每轮都视其需要重跑（制品内容只在`stdlib/*.n`变时才真变）；`stdlib/lang.n`与`stdlib/containers.n`是prelude，编译期内嵌进编译器本身（`:277-279`注释与`:285-287`的排除），改这两个文件等同改核心 | `CMakeLists.txt:275-304` |
| F-12 | `nlang_version`是INTERFACE库，由生成的`nlang_version.h`驱动，被八个成品与若干条目引用 | `CMakeLists.txt:53-56`；`temp/dag_report.txt` |

## 三、改动位置到选测集的映射（按实测）

| 改动位置 | CMake需重建 | 该重跑的ctest条目（Debug实测） |
|---|---|---|
| `src/vm/**`、`src/runtime/**`、`src/compiler/**`、`src/langservice/**` | 四个核心库＋全部成品＋71～78个单测可执行 | 几乎全部：139条目320.4秒单测＋成品驱动的28条目473.5秒冒烟＋全部集成用例 |
| `src/native/<模块>/**`（含`net`） | 仅该模块的DLL | 该模块n侧用例：net的六条单测2.0秒＋引用该包的集成用例。核心单测不受影响 |
| `stdlib/*.n`（prelude除外） | 仅`stdlib.npkg` | 引用该包的集成用例＋内置面覆盖条目`test_builtin_surface_coverage`、`test_prelude_faces` |
| `stdlib/lang.n`、`stdlib/containers.n` | 编译器及其全部下游 | 与改`src/compiler/**`同级 |
| `src/tools/nide/**`（九个`nide_*`库） | `nide_*`＋`nide` | 15条目141.5秒IDE族；Debug族不受影响 |
| `src/embed/**` | `nlang_embed`＋`embed_host` | 4条目3.4秒 |
| `src/native/tensor/kernels*` | `nlang_tensor_kernels` | 3条目1.5秒 |
| `src/3rdparty/tinyxml2` | `tinyxml2`＋`ncc`＋`nide` | 47条目8.6秒；另因`ncc`重链，成品冒烟一并失效 |
| `cmake/nlang_version.h.in` | `nlang_version`及其八个成品 | 64条目63.9秒 |

读法：表的左列就是`git diff --name-only`能给出的位置，右列是选测集。
核心四库那一行没有裁剪价值，其余每一行都有。

## 四、net案例

用户的判断成立：改`src/native/net/**`，`vm`的测试不必重跑。成立的原因不是选测聪明，
而是v0.13.0 net task 6把net源码从`nlang_vm`搬进了独立DLL（`tests/CMakeLists.txt:1239`注释），
边界先存在，选测才可能窄。但把这条原则落地时要注意两点只有实测才能发现的事实：

1. 链接图给不出答案。`nlang_net.dll`不被任何测试链接，net的六条单测测的是
   VM侧的调度/槽位/平台层（`test_net_scheduler`等仍链核心四库），
   真正把net跑起来的是集成用例经`NativeHost`运行期装载DLL、
   以及经`NLANG_TEST_STDLIB_DIR`读取`stdlib/net.n`所在的npkg（`tests/CMakeLists.txt:1394`注释、
   `:3428-3441`部署目标）。这类「运行期消费」边在CMake里目前只有`:1409`一条手写声明。
2. net的n侧改动落在`stdlib.npkg`里，而那个目标每轮都被CMake判为需要重跑（F-11）。
   按目标陈旧度选测会把所有npkg消费者误判为失效；按制品指纹选测才不误判。

## 五、要让原则在集成测试层成立，缺的三层

1. **补声明缺失的边。** 给每个ctest条目声明它消费的产物清单（ctests属性，
   如`NLANG_ARTIFACTS=nlang_net.dll;stdlib.npkg`），由表驱动、`tools/gate_time_check.py emit-cmake`
   发射——与本计划Task 11把`RESOURCE_LOCK`从`tools/gate_tiers.txt`发射是同一套做法。
   原生模块DLL、npkg、部署fixture目录三类边都属此列，手写`add_dependencies`那种
   一一条一条补的方式不可维护。
2. **改动集按内容指纹取，不按目标陈旧度取。** 在`cmake --build`前后各存一份
   产物指纹（按构建树各存一份），差集即「真变了的产物」。这一步同时解掉F-11的两处失真。
3. **集成测试需要逐用例的需求声明。** 粒度必须比产物更细：2344个用例都消费`ncc`，
   按产物裁剪等于不裁。可行做法二选一——在每个用例目录的清单里写`requires:`
   （测哪个包/哪个原生模块），或让跑器在每用例记录它实际装载了哪些原生模块与包
   （沿用参考表已有的「实测→入仓→守卫」套路，`tests/e2e/run_e2e_tests.py`已经逐用例打点）。
   未声明的用例按未映射处理，走既有的rc=3保守回退跑全量。

## 六、与现行门禁的关系与建议

现行T0已经实现了这条原则的粗版：`tools/test_impact.py`＋`tools/test_families.txt`
把改动文件映射到窄族条目，映射未命中则保守回退全量Debug单测。本文的数字解释了
它为什么把核心四片留成未映射——那里确实没有可裁的边界。

本计划的产物级扩展建议单独排期，不塞进当前12任务计划（计划已到Task 8–12，
加面会把已过的评审循环重开）。建议顺序：先做第五节第2条（指纹差集，最便宜、
且解掉两处已知失真），再做第1条（属性化声明，覆盖原生模块与npkg），
最后做第3条（逐用例需求声明，收益最大但需要跑器改造与一轮全量实测来生成初表）。

发布前这一条要单独裁决：发布门禁的前提是净室全新构建树（用来抓构建图里漏声明的依赖），
在那里CMake判定所有产物都要重建，按产物差集裁剪退化成全量。两个出路——
或者发布点保留全量矩阵、把裁剪只用于每轮与每子任务的验证（本文的实测支持这个选择），
或者让发布点改用增量树＋内容指纹，代价是失去净室构建抓漏声明的能力。
这个取舍留给spec裁决，本文只把冲突与代价说清。
