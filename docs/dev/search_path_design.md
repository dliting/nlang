# Library Search Path — 统一库搜索路径（Phase 3d）设计

> 位置：docs/dev/，随分支跟踪；2026-09-30 自 temp/ 迁入。本文是"设计 → 自审 → 完善"循环的载体，审核无新问题后按 TDD 实施。

## 1. 目标与范围

1. **ncc**：库搜索路径有缺省值；可 `-I <dir>` 指定；支持 `NLANG_PATH` 环境变量。
   现状：`BuildParams.m_ImportDirs` 默认仅 `"."`，`-I` 追加；stdlib 单独 `m_sStdLibDir`。
2. **nvm**：当前**完全没有** native 搜索路径配置（仅 loader 默认 exe 目录）。补 `-I` 与缺省路径。
3. **ndb**：同样缺搜索路径（调试引用第三方 DLL 的模块会失败）。补 `-I` 与缺省路径。
4. **nide**：搜索路径可在 **全局（Tools > Options）** 与 **项目级（Project Properties）** 配置；UI 支持增/删/上移/下移/浏览。
5. 标准库与第三方、编译期 `.n` 发现与运行期 DLL 加载，统一使用**同一组有序目录**；为混合库（Phase 4）与将来 LSP 预留。

## 2. 参考的主流机制

- **Python**：`sys.path` = 脚本目录（局部）→ `PYTHONPATH`（env）→ 标准库/site-packages（系统默认）。局部优先于 env。
- **GCC**：`-I` 前置到系统 include 路径（显式最高）。
- **Java**：`-cp` 覆盖 `CLASSPATH`；默认 classpath 含 `.`。
- **Node**：局部 `node_modules` 优先于全局；`NODE_PATH` 补充。
- 结论：**显式命令行 > 项目配置 > 局部（模块/源/项目目录）> 环境变量 > 系统默认**。

## 3. 搜索路径分层（有序，前者优先）

`BuildLibrarySearchPath` 按以下顺序拼接，去重后返回：

1. **explicitDirs** — 命令行 `-I`（按出现顺序，最高）。
2. **configuredDirs** — `.nproj` 的 `<ImportPaths>`（项目显式声明）。
3. **baseDirs** — 局部：项目目录 / 源文件目录 / 模块所在目录。
4. **pathEnv** — `NLANG_PATH` 环境变量拆分（`;` Win / `:` POSIX）。
5. **systemDirs** — 系统默认：stdlib 目录、可执行文件目录、当前工作目录 `.`。

**规范化/去重**：每个条目相对 CWD 转绝对 → `lexically_normal()` → Windows 下小写折叠，作为 key；同 key 保留**首次**出现。空字符串丢弃。**不**检查目录是否存在（保持纯函数、无 IO、确定性）。

## 4. 新增组件

### 4.1 `include/nlang/common/LibrarySearchPath.h`（header-only，纯 STL，inline）

放**公共头**而非编入 nlang_vm：ncc/nvm/ndb 可直接 include（零新增链接），将来纯 STL 的
langservice / LSP 也能用（**不迫使 langservice 链接 vm**，守住依赖方向）。函数小，inline 可行。

```cpp
namespace nlang {

struct SearchPathInput {
    std::vector<std::string> explicitDirs;   // CLI -I
    std::vector<std::string> configuredDirs; // .nproj <ImportPaths>
    std::vector<std::string> baseDirs;       // project/source/module dir
    std::string pathEnv;                     // raw NLANG_PATH
    std::vector<std::string> systemDirs;     // stdlib, exe dir, "."
};

// Split a PATH-like value on the platform separator (';' Win, ':' POSIX);
// empty entries dropped.
std::vector<std::string> SplitSearchPathEnv(const std::string& value);

// Ordered, normalized, de-duplicated search path. Pure: no filesystem
// existence checks (only lexical normalization), so deterministic/testable.
std::vector<std::string> BuildLibrarySearchPath(const SearchPathInput& input);

} // namespace nlang
```

- 去重 key：`fs::absolute(p).lexically_normal()`，Windows 再小写折叠；保留首次。
- 测试（test_vm 或独立）只 include 该头即可，不依赖 vm 符号。

### 4.2 `SymbolIndex` 增补（langservice）

- `void Clear();` — 清空 `m_symbols` 与 `m_loadedFiles`，支持配置变更后**原地重建**
  （编辑器持有 `&m_symbolIndex`，不能更换对象地址）。

### 4.3 nide `SearchPathArgs.h`（header-only，纯函数，Qt）

nide 不做完整解析（那是工具内 `BuildLibrarySearchPath` 的职责），只把"项目 + 全局"路径
**排序、绝对化、去重并展开为命令行 `-I` 参数**。派生决策放纯函数（与 `SettingsStore`
现有静态 resolver 风格一致），测试无需起 QProcess。

```cpp
namespace nlang {
// Ordered -I dirs: project paths first, then global paths; cleaned and
// de-duplicated (dedupKey). Project-relative paths resolve against
// projectDir; global-relative paths resolve against the user home dir
// (QStandardPaths::HomeLocation), since the nide process CWD is not stable.
QStringList buildImportArgs(const QStringList& projectPaths,
                            const QStringList& globalPaths,
                            const QString& projectDir);
// Convenience: interleave dirs into ["-I", d, "-I", d, ...] for QProcess.
QStringList appendImportArgs(QStringList args, const QStringList& dirs);
} // namespace nlang
```

> 已知限制（Phase 4/LSP 再扩展）：路径中暂不展开 `%VAR%`/`$VAR` 宏；browse 默认给绝对路径。

## 5. 各工具的 input 装配

| 工具/模式 | explicit | configured | base | pathEnv | system |
|---|---|---|---|---|---|
| ncc 单文件 compile/build/run(compile段) | CLI -I | — | 源文件 parent | NLANG_PATH | stdlibDir, exeDir, `.` |
| ncc `-p` 项目 | CLI -I（nide 传 项目+全局） | ProjectFile.importPaths | projectDir | NLANG_PATH | stdlibDir, exeDir, `.` |
| ncc `run <nmod>` | CLI -I（run 分支新增解析） | — | nmod parent | NLANG_PATH | exeDir, `.`（无 stdlib） |
| nvm `<nmod>` | CLI -I（新增） | — | nmod parent | NLANG_PATH | exeDir, `.` |
| ndb `[--machine] <nmod>` | CLI -I（新增） | — | nmod parent | NLANG_PATH | exeDir, `.` |

- ncc：用 `BuildLibrarySearchPath` 结果**赋值** `params.m_ImportDirs`（覆盖默认 `"."`；结果 system 已含 `.`）。
  `BuildEnvironment.h:35` 的默认 `"."` 保留（compiler 库独立/测试使用）。
- nvm/ndb：对结果逐条 `executor.AddNativeSearchDir(...)`。
- 命令行：ncc 统一解析已支持 `-I x` / `-Ix`；nvm/ndb 新增 `-I x`（可重复，任意位置）。
- help/usage 文案同步更新。

## 6. nide 配置

### 6.1 全局 — `SettingsStore`（QSettings）

- 新增 `QStringList m_librarySearchPaths`，key `ide/librarySearchPaths`；
  getter/setter `librarySearchPaths()` / `setLibrarySearchPaths(...)`；`load/save` 直接存取 QStringList。

### 6.2 项目级 — `ProjectNode`（.nproj XML）

- 新增 `std::vector<QString> m_importPaths`：内存中为**绝对路径**；保存时相对 projectDir（与 FileNode 一致）。
- API：`addImportPath(path)`（dedupKey 去重，返回是否新增）、`removeImportPath(index)`、
  `moveImportPath(index, delta)`（上移/下移）、`importPathCount()`、`importPathAt(i)`、clear。
- XML：在 `<Sources>` 之后写
  ```xml
  <ImportPaths>
    <Dir path="libs/acme"/>
  </ImportPaths>
  ```
  无路径时省略该元素。

### 6.3 ncc `ProjectFile`（tinyxml2）

- 解析可选 `<ImportPaths>`，每个 `<Dir path="...">` 相对 projectDir 转绝对，存入
  `std::vector<std::string> importPaths`；Dir 无 path 报错；多个 `<ImportPaths>` 块报错（同 Sources 规则）。

### 6.4 UI（.ui + 对话框代码）

- **SettingsDialog**：QListWidget（搜索路径）+ Add / Browse / Remove / Up / Down。
  `init(...)` 增加 paths 入参；新增 `librarySearchPaths()` getter。
- **ProjectPropDialog**：同样一组控件；create 模式默认空，edit 模式载入/回写项目 importPaths。
- 新增文案走 `tr()`；更新 `translations/nide_zh.ts`、`nide_en.ts`（lupdate 后补译）。

## 7. nide 集成：命令行与编辑器索引

### 7.1 合并顺序（项目优先于全局）

nide 传给工具的 `-I` 有序目录由 `SearchPathArgs::buildImportArgs(项目paths, 全局paths, baseDir)`
统一生成 = **项目 importPaths（前）+ 全局 searchPaths（后）**，绝对化、clean、去重；
再用 `appendImportArgs` 交错为 `-I d -I d ...` 追加到 QProcess 参数。

- `buildProject`：`ncc build -p ... -o ... [-I proj...] [-I global...]`。
  （ncc 还会读 .nproj configured，项目路径在 -I 前部已先出现，configured 副本去重丢弃。）
- `buildStandaloneFile`：`ncc build file -o out [-I global...]`。
- `runProject`：`nvm out [-I proj...] [-I global...]`。
- `runStandaloneFile`：`nvm out [-I global...]`。
- 调试：`DebugClient::launch(module, searchPaths)` → `ndb --machine module [-I proj...] [-I global...]`。

最终生效顺序：项目 → 全局(IDE 用户级) → 局部模块目录 → NLANG_PATH → exe/cwd（见第 3 节）。

### 7.2 编辑器代码辅助索引（为 LSP 树模式）

- 新增 `MainWindow::reindexConfiguredLibraries()`：
  `m_symbolIndex.Clear()` → `LoadLibraryDir(stdlibDir)` → 对全局 paths、**solution 中所有项目**的 importPaths 逐个 `LoadLibraryDir(...)`。
  （不索引 projectDir：项目顶层源文件不是库符号；库存放的子目录由 importPaths 显式指向。）
- 调用时机：启动、设置保存后、solution/项目加载、项目增删、项目属性保存后。增删路径均即时生效。
- 这确立"配置变更 → 重建索引"的单一模式，LSP 复用同一 search path 配置源。

## 8. TDD 计划（每步先写红测试，再实现转绿）

1. `tests/test_vm/test_library_search_path.cpp`（新，CHECK+main 风格）：
   - Split：平台分隔符、空/连续分隔符丢弃、单值、多值顺序。
   - Build：五层顺序；env 在 base 之后；跨层去重保留首次；相对转绝对；`a/../b` 规范化；Win 大小写折叠；空层/空串忽略。
2. `SymbolIndex::Clear`：langservice 加用例（clear 后 size==0、可重新加载）。
3. `tests/test_ncc/test_projectfile.cpp`：ImportPaths 正常解析/相对转绝对/顺序；Dir 无 path 报错；多个块报错；缺省合法为空。
4. e2e（ctest，tests/CMakeLists）：
   - ncc 缺省：包 `.n`+dll 放**源同目录**（base），不传 -I，编译运行成功。
   - `NLANG_PATH`：`set_tests_properties(... ENVIRONMENT NLANG_PATH=...)`。
   - ncc `run` 缺省模块目录。
5. nvm e2e：`nvm -I` 找到 dll；缺省模块同目录。
6. ndb：`-I` 解析 + 默认模块目录（machine 路径；配合 test_debug_client）。
7. `test_nide/test_settingsstore.cpp`：全局 paths 默认空、round-trip。
8. `test_nide/test_projectmodel.cpp`：.nproj ImportPaths 读写/相对/去重/增删/排序。
9. `test_nide/test_dialogs.cpp`：两个对话框路径 list 的 Add/Browse/Remove/Up/Down。
10. `test_nide/test_searchpathargs.cpp`（新）：buildImportArgs 顺序（项目→全局）、绝对化、clean、去重；appendImportArgs 交错。
11. `test_nide/test_mainwindow.cpp`：build/run/debug 实际命令行含 -I（集成，真实编译/运行）。
12. `test_nide/test_debug_client.cpp`：launch 命令行带 -I。

约束：真实编译 `.n`、VM 真执行字节码、真实加载 DLL，不 mock；覆盖率 >90%。

## 9. 文档与配套

- docs/{zh,en}：standard-library 增加"库搜索路径 / 包 / NLANG_PATH / .nproj ImportPaths"小节；vm-architecture 说明统一解析。
- ncc/nvm/ndb usage 文案。
- 检查 `nide_deploy_check.cmake` 与 install 布局（stdlib、nlang_*.dll 位置）；本特性不改安装布局。

## 10. 自审 Checklist（循环，直到无新问题）

- [x] 优先级对齐主流（CLI > 项目 > 局部 > env > 系统）。
- [x] 标准库/第三方、.n/DLL、编译期/运行期统一。
- [x] nide 全局 vs 项目，项目优先；路径转绝对。
- [x] LSP 预留：单一 search-path 配置源 + 配置变更重建索引。
- [x] 跨平台 env 分隔符；路径含空格/中文（QProcess/argv 原样传递）。
- [x] 不需要向后兼容（用户已确认）。
- [x] 真实测试、不 mock、覆盖率。
- [x] DiscoverLibrarySources 在含 stdlibDir 的 import dirs 中幂等（LoadFileOnce），stdlib 不会被误判为第三方。
- [x] BuildParams 默认 "." 保留；ncc assign resolver 结果，不重复。
- [x] nide 派生（排序/展开 -I）放纯函数 SearchPathArgs，QProcess 只执行，易测。
- [x] 编辑器索引：不索引 projectDir；纳入 solution 所有项目 importPaths。
- [x] ctest ENVIRONMENT 中 `;` 是 CMake 列表分隔符：NLANG_PATH e2e 用单目录，多路径由单元测试覆盖。
- [x] nvm/ndb 不链接 langservice（运行时不需 .n；stdlib dll 在 exeDir）。
- [x] ndb 以 machine e2e 验证（stdin run/quit）；interactive 共用 -I 解析。
- [x] resolver header-only 落 include/nlang/common：守依赖方向，LSP/langservice 可零耦合复用。
- [x] 全局相对路径相对 home；项目相对路径相对 projectDir；暂不支持路径宏（记录为限制）。
- [x] 实现注意：ncc/nvm/ndb e2e 共享一个已构建的第三方 nmod（CMake fixture/ALL target）；ndb e2e 前核对 machine 协议关键字（run/quit、output 事件）。

## 循环审核结论

第 1–4 轮挖掘并修订：编辑器索引范围（不索引 projectDir、覆盖所有项目）、nide SearchPathArgs 纯函数、
resolver 依赖方向（header-only 落 common，避免 langservice 被迫链接 vm）、相对路径基准（project/home）、
ctest ENVIRONMENT 单目录、nvm/ndb 不链接 langservice。第 3、4 轮未再出现改变架构的新问题，**设计定稿，进入 TDD 实现**。
- [ ] 实现中如发现新问题，在此追加。
