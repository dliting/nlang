# 开发设计文档（库管理体系重构）

本目录存放 dev 分支「库管理」重构线的阶段设计稿与实施计划，随分支跟踪，
合并到 master 时随库带走。`temp/` 仍是临时产物区（实验探针、构建日志、
备份目录），不入库；正文中指向 `temp/probe/`、`temp/rb*/` 等的证据只在
本地工作树可复核，权威数据以文中记录的命令为准。

> 2026-09-30：本目录文档自 `temp/` 迁入（用户裁定：计划与设计稿入
> `docs/` 跟踪，便于将来参考与合并）。文中历史性表述「temp/ 不提交」
> 指临时产物，仍然有效。
>
> 评审台账等处引用的 `docs/{zh,en}/…` 旧树路径为迁移前实测记录，行号
> 以当时旧树为准，不改写以免失真；执行任务时按现行树重新定位
> （现行手册树为 `docs/user_manual/{zh,en}`）。

## 重构线总览

从「整项目一份合并 `.nmod` + VM 内建 stdlib」走向「目标文件 + 库归档 +
链接」的常规库管理体系。已完成（阶段 1～4b2，见 dev 分支提交）：
NativeHost ABI 与动态原生模块加载、math/io/fs 改动态原生模块、统一库
搜索路径（`-I`/`NLANG_PATH`）、stdlib/*.n 签名权威、编译器内联编译
NLang 库源码、`ns.Type` 可达性。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 3a～3c | 原生模块加载、库签名、搜索路径 | 已完成（dev 分支提交） |
| 4～4c | 库内联编译、nide 接入、清理 | 已完成（dev 分支提交） |
| 5 | 包名与类型身份（路径唯一决定包名，删 `namespace`） | **计划已定稿，待实施** |
| 6 | 逐包产码 `.nmod` + `.npack` 归档 + 链接器 | 待启动 |
| 7 | 依赖记录、过期检测、增量编译、`_package.n` 可见性 | 待启动 |

## 文档清单

按时间顺序；「取代」列给出文档间的取代关系。

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| `search_path_design.md` | 库搜索路径设计（`-I`、`NLANG_PATH`、nide 配置） | 已实施 |
| `phase3a_design.md` / `phase3b_design.md` / `phase3c_design.md` | 阶段 3 三步设计 | 已实施 |
| `phase4_design.md` | 阶段 4 总设计 | 已实施 |
| `phase4bcd_design.md` | 阶段 4b/c/d 设计 | 已实施 |
| `phase4b2_audit_notes.md` | 4b-2 审核笔记 | 已闭环 |
| `phase4c_plan.md` | 阶段 4c 实施计划 | 已实施 |
| `phase4c_audit_notes.md` | 4c 审核笔记 | 已闭环 |
| `phase4d_plan.md` | 原 4d 计划（依赖记录、增量、可见性） | **归属阶段 7**；依赖/校验和方案在阶段 6 后需重写，`runNccBuild` 异步化与 `ndb` 读库源码两节可留用 |
| `phase4e_design.md` | 原 4e 设计（限定类型） | 被取代 |
| `phases_567_design.md` | 阶段重划总稿（5→6→7）＋ 用户裁决汇总 | 现行 |
| `phase5_design.md` | 阶段 5 详细设计（R1～R5、D1～D13） | 现行 |
| `phase5_plan.md` | 阶段 5 实施计划（9 任务，逐 task 循环完善后实施） | **待实施**；计划文本已经 10 轮评审修订 |

## 实施约定（承 phase5_plan.md 全局约束）

- 行号基线 `d7ca710`；工作树 `E:/cases/nlang/dev`（git worktree，分支 dev）。
- 构建 `cmake --build build-dev --config Release -j 8`；全量 ctest **串行**，
  基线 63/63（Task 4 起 64/64）。
- 逐 task 循环完善：计划段审核无新问题 → 实现 → 实现 diff 审核 → 下一个 task；
  非关键问题可延到下一个 task。
- 红线：不 push；master 不动；不提交 `AGENTS.md`/`CLAUDE.md`、`build-dev/`、
  `temp/`；本工作树禁 `git stash`（autocrlf + lfs 会改写行尾）；测试真实
  编译真实执行，不 mock；公开文字零旧引擎痕迹；不留兼容层。
