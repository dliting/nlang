# 测试套件使用说明

本仓库测试套件的简要使用说明:目录构成、各种跑法、以及新增测试时要遵守的约定。分层门禁的设计动机见`docs/dev/gate_tiering_design.md`,相对计时预算的设计见`docs/dev/gate_timing_design.md`;本文只讲日常使用。

## 目录构成

| 路径 | 说明 |
| --- | --- |
| `test_*/` | C++单元测试源码,每个测试可执行文件一个子目录(`test_nide`为Qt/QTest的IDE测试,`test_support`为共享测试工具) |
| `e2e/` | 端到端(end-to-end,下称e2e)测试:`run_e2e_tests.py`运行器、`manifest.txt`测试清单、`examples_manifest.txt`示例清单、`env_gates.txt`逐条目环境开关,其余为`.n`语料与跨模块包目录 |
| `check_*.py` | 仓库级检查脚本(字节码确定性、工具链行为、GC压力等),同样注册为ctest条目 |
| `bench/`、`packaging/`、`fixtures/` | 性能测试、打包检查与共享测试资源 |
| `CMakeLists.txt` | ctest唯一注册点:每个条目自动获得专属临时目录与构建树令牌 |
| `gate_time_reference.txt` | 分构型参考计时表,ctest/e2e的看门狗超时与预算帽都从它派生 |
| `gate_time_keys.cmake` | 计时表键名的生成helper,CMake与Python两侧共用 |

门禁工具在`tools/`下:`gate.sh`(单命令门禁包装)、`gate_tier.sh`(T0/T1/T2批次跑批)、`gate_time_check.py`(计时表守卫与校准)、`gate_tiers.txt`(每层包含哪些腿)、`test_families.txt`(T0影响映射)、`test_impact.py`(映射消费者)。

## 构建测试构型树

环境为C++17、CMake 3.16+、VS2022、Flex 2.6+/Bison 3.0+。configure时打开`NLANG_BUILD_TESTS`,Flex/Bison路径找不到时用`-DFLEX_EXE=`/`-DBISON_EXE=`指定:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" \
  -DNLANG_BUILD_TESTS=ON -DFLEX_EXE=... -DBISON_EXE=...
cmake --build build --config Debug
cmake --build build --config Release
```

跑门禁批次按约定用三棵构建树:

- `build/`:主树,承载Debug与Release两个构型
- `build-ide/`:额外打开`-DNLANG_BUILD_IDE=ON`(需要Qt5),跑IDE相关测试
- `build-asan/`:AddressSanitizer插桩树,跑ASan族单元测试与e2e子集

VS生成器是多构型的,**所有ctest调用必须带`-C <构型>`**;不带时测试清单为空却返回0,是假绿。

## 跑单元测试

```bash
ctest --test-dir build -C Debug                    # 全量
ctest --test-dir build -C Debug -R <名称正则>      # 单个条目族
```

每个条目的临时目录(`nlang_test_scratch_<构建树令牌>`目录,令牌由构建树路径的MD5派生)和`TMP`/`TEMP`、`NLANG_TEST_TREE_TOKEN`环境变量都由CMake注册注入;手动运行测试可执行文件时需要自己带上。另有两点注意:

- ctest条目名与exe目标名不一定相同,用`ctest --test-dir build -C Debug -N`查询实际条目,按目标名猜测会跑到陈旧二进制。
- 本机QTest条目的stdout恒为空,测试可执行文件的退出码等于失败的测试函数数。

## 跑e2e测试

```bash
D:/dev/miniconda3/envs/py313/python.exe tests/e2e/run_e2e_tests.py <ncc> <nvm> <ndb> [选项]
```

三个二进制路径省略时默认取`build/`树的Release布局;测其他构型树必须显式传入。常用选项:

- `--config=Debug|ASan|Release`:计时预算评估取参考表的哪个构型列,省略时按工具路径推断
- `--timeout=SEC`:没有参考行的条目的兜底看门狗帽(默认90秒);有参考行的条目用表内派生帽
- `--record-out <文件>`:逐条目输出计时行,供回填参考表

环境变量:`NLANG_E2E_MANIFEST=<文件>`只跑指定清单(分层门禁的e2e腿就是这样选子集的);`NLANG_E2E_SCRATCH_TAG=<名称>`给并行分片隔离scratch命名空间;`NLANG_TEST_WORK_FACTOR`按比例放大计时帽。

运行器有两道「制品比输入陈旧」守卫,命中时一条测试都不跑直接以rc=2退出:

1. 陈旧工具守卫:ncc/nvm/ndb任一比`src/`、`include/`下的源文件旧
2. 陈旧包守卫:`<构建树>/stdlib/stdlib.npkg`比ncc或`stdlib/`下最新文件旧

两种情况的解法都是重建(标准库包重建`nlang_stdlib_package`目标),不要用篡改时间戳的方式绕守卫。

## 门禁:长时间测试的标准跑法

凡耗时较长的测试一律经`tools/gate.sh`跑,不要自己管道包装:

```bash
tools/gate.sh <名称> <日志前缀> <命令...>
```

**判决只认`GATE <名称>: PASS/FAIL rc=<n>`判决行与退出码**;门禁输出经`| tail`这类管道查看会丢掉判决行造成假绿。

日常收尾按层跑`tools/gate_tier.sh`:

```bash
tools/gate_tier.sh T0 --master-tree build --git <基线提交> [--paths ...]
tools/gate_tier.sh T1 --master-tree build --asan-tree build-asan --ide-tree build-ide [--docs]
```

- T0(几分钟):按`tools/test_families.txt`的影响映射选出受影响的Debug单测族与e2e族;改动没命中映射时保守回退Debug全量单测。
- T1(约1至1.5小时):Debug全量单测、Debug全量e2e、IDE树全量ctest、ASan单测、ASan e2e子集。`--docs`追加手册五腿(手册工具pytest与中英双语站点的构建加检查,吃`--master-tree`树的Release二进制)。
- T2:版本收口门禁,在此之上加全量ASan e2e、Release构建与e2e、perf条目。

批次规则:不论前面是否变红都跑完全部腿,但只要有一条红,整批作废,修复后全批重扫。`--dry-run`只打印计划不落盘;日志与scratch默认在`temp/tier/`,用`NLANG_GATE_TIER_WORK`改位置。

计时体系:带计时的腿开跑前自动做机器规模校准(对参考表的哨兵条目实测取比率中位数,落在带外则拒绝该腿并要求重新记录参考值)。跳过采样用`NLANG_GATE_MACHINE_SCALE=<倍率>`,`tools/gate_time_check.py`提供`calibrate`、`record`与`check-complete`(表缺行)、`check-orphans`(表多行)、`check-budget`、`check-constants`、`check-timeouts`子命令。

## 新增测试

- 走测试驱动开发,用真实组件:编译器真实编译`.n`,虚拟机真实执行字节码,不用mock。
- 单元测试:源码放进对应的`test_*/`子目录,在`CMakeLists.txt`注册条目;然后给`gate_time_reference.txt`补上该条目在每个构型的参考行——先真实跑一遍,再用`tools/gate_time_check.py record --config <构型> --from-junit <junit文件>`回填,`check-complete`守卫要求每个条目都有行。演示验收类条目(如`mnist_mlp_*`)在`哨兵`列标`samples`——按性质退出每轮判定腿,收口批承接,详见`docs/dev/gate_timing_design.md`的D-20。
- e2e测试:语料`.n`放进`tests/e2e/`,在`manifest.txt`加一行`<名称> <预期退出码>`;每条清单行都必须至少被一个分层选择器选中(`tools/gate_tiers.txt`的覆盖守卫会检查)。
- 断言不得依赖挂钟时间;确定性结论归单元测试,e2e只断言无条件成立的性质。
- 单项超过30秒的慢测试只在提交前的门禁批次里跑,开发轮次用`ctest -E <名称正则>`排除。
