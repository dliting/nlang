门禁分层设计（T0/T1/T2）

## 背景与数据

例行收尾一轮（Debug＋ASan＋IDE三树、e2e串行）实测2.5-4小时：0.13.8收口批不含ASan全量e2e已耗时约46分钟（temp/gates/t7_*时间戳07:40→08:26）；ASan约为Release的7倍慢，全量1763条e2e在ASan下是小时级。阶段停靠与提交批都以这个频次付费，迭代节奏被拖垮。

参照主流实践（presubmit小/提交门中/发布全量的测试分层、测试影响分析、消毒器按风险取子集），把现有门禁按触发点分三层。用户已批准（2026-11-16），并附加约束：**必要的测试不能省略**。

## 层定义

| 层 | 触发点 | 内容 | 目标时长 |
|---|---|---|---|
| T0迭代层 | 任务内每轮 | 受影响的Debug单测族＋受影响e2e族（影响映射表）；映射未命中→保守按Debug全量单测处理 | 分钟级 |
| T1停靠层 | 阶段停靠 | Debug全量单测＋全量e2e（串行）；IDE树全量ctest；ASan全量单测＋e2e内存风险子集；慢测试批；手册五腿（动了手册才跑） | ~1-1.5小时 |
| T2收口层 | 版本收口一次 | 全量：Debug＋ASan全量e2e＋IDE＋Release构建＋Release e2e＋慢测试＋docs五腿，兼制品生产 | 半天，每版本一次 |

Release构建与Release e2e维持「仅发布时点」裁定不变，不因分层回到例行。

## 覆盖不变量（必要测试不省略）

1. **全测覆盖守卫**：每条e2e清单条目至少被一个层的选择器命中；守卫脚本逐条核对，未命中即门禁红。分层只重新调度「何时跑」，不产生「无处可跑」的测试。
2. **T1的ASan子集下界**：子集⊇(本阶段触碰的测试族∪内存风险族)。内存风险族＝并发族、容器族、GC/异常/序列化族（既有裁定「并发族独立压测＋ASan」由此继续满足）。触碰族按阶段实际改动面判定，在停靠记录里显式列出。
3. **T0保守缺省**：影响映射表未命中的源改动→按Debug全量单测跑，不按少跑处理。

## e2e分片并行

断言确定性裁定（测试不得依赖时间）保证并行分片与串行的判决等价；超时帽是校准产物不是断言，分片后按分片数放大余量（Debug腿x3已有，分片内再按最慢校准）。一次A/B对拍验证：同一提交下分片并行与串行判决一致后，分片才成为T1默认形态。ASan腿暂不分片（mnist余量约10%），留T2全量时评估。

## 判决行

判决行带层名：`GATE T1-debug-unit: PASS`／`GATE T1-asan-e2e-subset: PASS`／`GATE T2-release-e2e: PASS`。门禁判决纪律不变：只认gate.sh判决行或退出码。

## 环境门控（活成功前提的环境依赖）

部分条目的活成功前提依赖机器能力（环回ICMP回声、保留TLD的NXDOMAIN诚实性），而环境软件可静默吊销之（VPN过滤驱动吞ICMP、DNS劫持应答保留域名——2026-11-16实证：OrayBoxVpnEnt隧道激活后OS级`ping 127.0.0.1`全丢，同二进制晨绿午红）。这类条目由带外OS先知（oracle）门控：`tests/e2e/env_gates.txt`登记「条目→能力键」，运行器每轮先知一次，能力缺失即打印SKIP行并计入skipped（不算失败，也绝不静默绿）。纯契约断言永不门控——门控只承认「机器根本做不了」，不承认「做得慢、想跳过」。单测侧同型（test_net_platform的活回声以OS ping出口码门控）。

## 性能递延（临时裁剪，等dev测试改造回收）

用户裁定（2026-11-20）：耗时超过1分钟的性能测试暂时砍出所有层，先快速推进版本；dev线测试耗时改造合入后删除递延文件即恢复全量，完整测试套件由该改造后的版本统一承载，本分支不重复做。这区分于环境门控：环境门控承认「机器做不了」，递延承认的是「用户明令暂时不跑」。

- 递延文件即开关，按消费方分两个：`tools/gate_perf_defer_ctest.txt`（ctest腿经`ctest -E`锚定排除，文件存在即裁剪）；`tools/gate_perf_defer_e2e.txt`（e2e腿经`NLANG_E2E_EXCLUDE`递延，运行器逐条打印DEFER行并计入deferred汇总——不静默绿）。删除文件即恢复全量。
- e2e侧运行器对递延名单做两道响亮校验：环境变量指向的文件缺失即退出2（防「文件丢了、静默恢复全量」的假裁剪）；名单里的名字不在全部e2e清单并集内（改名漂移）即退出2。ctest侧无校验（陈旧名只是多跑一条，不产生假绿）。
- 裁剪范围＝纯性能族：ctest三件（nd_resolve_stress_tests 911s、regex_redos_run 354s、threads_tests 258s，ASan实测）＋e2e五件（stdlib_net_ping_stress、concurrent_gate_hammer、str_concat_stress、http_conc_bench、net_conc_bench）。内存风险正确性族（gc_stress、byte_stream_gc_stress、concurrent_churn_gc、func_gc_stress、stdlib_http_conc、stdlib_regex_conc）不裁——ASan e2e腿的存在目的正是它们，且各条均低于1分钟线。
- `tier_coverage_check.py`接受可选递延文件参数，名单内条目豁免「至少被一层选中」不变量（裁剪是用户明令不是分层漏洞）；文件删除后不变量自动收回全覆盖。

## 落地物

- tools/gate_tier.sh：按层名组装既有gate.sh腿（T0/T1/T2各一入口），复用既有机器级锁。
- tools/e2e_shard.py：manifest按族切K片、各自输出目录、汇总判决。
- tools/test_families.txt＋tools/test_impact.py：测试族清单与src目录→族的保守映射。
- tools/tier_coverage_check.py：覆盖不变量守卫（清单条目↔层选择器）。
- tools/gate_perf_defer_ctest.txt＋tools/gate_perf_defer_e2e.txt：临时性能递延名单（见「性能递延」节，删除即恢复全量）。
- AGENTS.md门禁段与CONTRIBUTING同步改写（三树表述→三层表述）。

## 实施顺序

期②按新T1停靠（复用既有腿组装），随后落分层工具（TDD：分片A/B、覆盖守卫、映射表测试），期③起全线使用。
