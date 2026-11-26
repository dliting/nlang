# nide Linux 移植设计

## 背景

核心（语言/编译器/VM+tensor）的Linux移植已收官（`linux_port_design.md`，M1-M5全绿，0.14.10发布），当时用户裁定（2026-11-25）先不移植nide以专注张量线。用户裁决（2026-11-27）：现在移植nide。nide不被仓库任何其他目标依赖（顶层CMakeLists.txt:245的`NLANG_BUILD_IDE`门把它隔离在工具构建之外），因此本任务与0.14.13收口线并行推进、互不阻塞。

nide是Qt5 Widgets桌面IDE：主层约9.2千行，按CMake拆成8个库——projectmodel/editor/compilelog/dialogs/mainwindow带UI面，recent/settings/debug为GUI-less小库（只链Core，明说为单元测试可链性，src/tools/nide/CMakeLists.txt:76-126）；嵌入式帮助浏览器走`Qt5::WebEngineWidgets`（:193）；终端面板自研（terminal/子目录：libvterm vendored拷贝做终端状态机，`PtyProcess`做进程侧）；翻译用`Qt5LinguistTools`（lrelease产.qm经生成qrc嵌入库，:210-241）。

## 环境事实（已核实）

- 验收机（2×4090盒，Ubuntu 22.04，环境规格见记忆台账multi-gpu条目）有完整自编译Qt 5.15.2于`/opt/qt/5.15`：cmake config含`Qt5LinguistTools`、WebEngine三件套（Qt5WebEngine/WebEngineCore/WebEngineWidgets）与IDE gate闭包所需的`Qt5QuickWidgets`/`Qt5PrintSupport`（顶层CMakeLists.txt:105-106的find_package清单全满足）；`libexec/QtWebEngineProcess`、`resources/`（icudtl.dat+pak——注意自编译prefix布局资源在`<prefix>/resources`，非apt布局的`<prefix>/share/qt5/resources`）、`translations/`齐；platforms插件`libqoffscreen.so`与`libqxcb.so`都在；`bin/`下qmake/lrelease/lupdate齐。`CMAKE_PREFIX_PATH=/opt/qt/5.15`直指可用。apt里没有qt包（dpkg探测会误判为无Qt）。
- 盒上构建链：`/opt/cmake/bin/cmake`（3.29.9——`cmake_minimum_required(VERSION 3.24)`是树级要求，顶层CMakeLists.txt:3，GNU链接臂`$<LINK_GROUP:RESCAN>`同样要求；Ubuntu自带的3.22不够，核心移植已实证走/opt/cmake）、GCC 11.4、glibc 2.35。fontconfig可用（fc-list 410项，含Noto Sans CJK SC）。
- Windows构建约定（AGENTS.reference.md，master树文件）：`-DNLANG_BUILD_IDE=ON -DCMAKE_PREFIX_PATH=<Qt5.15.2>`；`NLANG_BUILD_TESTS`默认OFF，漏配会假绿。
- README依赖表（README.md:58）写Qt模块为Core/Xml/Widgets——漏了WebEngineWidgets（nide_mainwindow实链），随本任务更正。

## 范围

**入**：`nide`可执行与src/tools/nide下全部九个库（含terminal子目录）；`src/3rdparty/libvterm`的Linux构建；IDE树（`NLANG_BUILD_IDE=ON`）ctest在Linux全绿；nide部署检查的跨平台面；README/手册的Linux支持描述。

**出**：Windows安装器与`.rc`资源面（保持不动）；Windows的POST_BUILD运行时拷贝臂（Linux走Qt prefix运行时查找，见平台面对策）；ASan腿（Linux上后续单独立项，沿用核心移植裁定）；macOS（不在验证面）。

## 平台面清单与对策

按层列耦合点与Linux对策。现状盘点由源码核查支撑，含已核实条目与待回填条目（标注）：

### CMake构建面

- `add_executable(nide WIN32 main.cpp nide.rc)`（src/tools/nide/CMakeLists.txt:244-247）：`nide.rc`内容仅一行icon声明（nide.rc:1），非MSVC不能编。对策：`nide.rc`移入`if(WIN32)`条件源；`WIN32`可执行属性在Linux被CMake忽略，无需处理。
- POST_BUILD臂（:259-276）全在`if(WIN32)`内，其中**工具同目录复制是运行依赖而非部署装饰**：`toolPath()`=IDE exe同目录裸名拉起ncc/nvm/ndb（MainWindow.cpp:212-215），Linux缺这个臂则Run/调试全挂。对策：Linux臂只拷三个工具（`copy_if_different $<TARGET_FILE:ncc/nvm/ndb>`），Qt库与platforms插件不拷——自编译Qt装在固定prefix，插件路径编译进libQt5Core，构建树rpath自动含`/opt/qt/5.15/lib`，直跑。
- `nlang_copy_qtwebengine_runtime`（顶层CMakeLists.txt:153-170）是Windows目录形态假设：helper进程名带`.exe`+Debug`d`后缀（:131-137）、resources/locales目录按`${Qt5_DIR}/../../../`上溯（:125-148）。对策：保持WIN32-only不扩展；Linux开发者构建下WebEngine从prefix自取（libexec/resources均在prefix内，已实测）；部署形态的自含闭包在N4按实际prefix布局新写。**WebChannel/Positioning是WebEngine闭包清单的windeployqt伙伴件，Qt构建可以不带这两个模块**（盒上自编译Qt即缺）——清单按`if(TARGET)`守卫拼装，否则任何缺模块的Qt在generate期直接失败（N1预演实证）。install规则的Qt闭包段同样Windows形态：包`if(WIN32)`，Linux闭包归N4。
- **测试exe的工具复制臂也是WIN32-only**（第三方审核发现，已核实并补落UNIX臂）：`test_mainwindow`与`test_searchpath_integration`依赖WIN32的POST_BUILD把ncc/nvm/ndb拷到测试exe旁（tests/CMakeLists.txt:2065-2078、:2134-2144），`toolPath()`从applicationDirPath解析裸名而Linux单配置树里工具与测试exe不同目录——不补UNIX臂则mainwindow_tests百余用例与searchpath_integration两条全红。
- `Qt5LinguistTools`的find_package与lrelease链路在盒上已实证可用；`_nlang_sweep_asan_runtime`对非MSVC直接return（顶层:201-213），Linux零处理。
- **moc与libstdc++头的解析冲突（N1预演实证）**：IDE树给核心测试挂QtTest链接后AUTOMOC启动，moc不是完整C++解析器，跟入`<filesystem>`（直include或经项目头链）在GCC 11的`bits/fs_fwd.h`上炸。对策：`#ifndef Q_MOC_RUN`守卫（moc预定义该宏，编译器不定义、语义零变化）——测试cpp与项目头（LibrarySearchPath.h、ModuleRegistry.h等）已全量落守卫。
- IDE树ctest注册面（tests/CMakeLists.txt）：`if(NLANG_BUILD_IDE)`门内**31条**——16条nide相关（projectmodel/solutiontreemodel/recentstore/settingsstore/searchpathargs/debug_protocol/debug_client/editor/syntaxhighlighter/compilelogbrowser/runerrorbrowser/dialogs/mainwindow/nide_deploy_check/searchpath_integration/terminal）加15条编译器侧QtTest（import_parse/generic_decl/type_access_prefix/ref_declarator/ref_binding/method_contracts/const_decl/generic_descriptor/descriptor_spelling/generic_typeparam_gc/prelude_faces/builtin_surface_coverage/module_import/array_property/statement_diag），全部首次在Linux编译；其中30条经`nlang_add_qtest`包装（:1412-1436），deploy_check为裸`add_test`；per-tree scratch根已按TEMP/TMPDIR分平台（:44-63）。

### 进程与终端面（terminal/）

`PtyProcess`（terminal/PtyProcess.cpp）是唯一的进程-终端桥。Windows臂用ConPTY（动态解析三入口，缺失→`StartError::Unsupported`）；**POSIX臂已按规格预写**（文件头自述"待Linux移植验证"）：`SpawnPosix`走openpty/fork/execvp（:303-349）、Write→write()（:364-367）、Resize→`TIOCSWINSZ`（:377-382）、Kill→SIGKILL+靠EOF收割（:424-431）、ReaderMain read()+waitpid带128+sig退出码（:453-468）。本任务=首次编译验证+补缺口（N1预演已过编译）。

**行为缺口清单（盘点+第三方审核确认）**：
- workingDirectory丢失：`SpawnPosix`签名与`Start`调用臂都不传（PtyProcess.cpp:156-158、:183-184；Windows臂传，:285-289）——而Run逻辑依赖"子进程CWD=产物目录，示例写文件落临时区"（MainWindowBuildRun.cpp:218-228）。对策：fork后exec前`chdir`（失败`_exit(126)`，父进程经waitpid见126退出码呈现在Run输出里），补测试钉住。
- Kill的陈旧pid风险：`kill(childPid, SIGKILL)`无`childPid>0`守卫（:424），waitpid收割后childPid不复位（:459）——自然退出后每次重启先`Kill()`（:171），pid复用可能误杀无关进程；Windows臂靠句柄关闭天然失效。对策：收割后置`childPid=-1`+Kill守卫，补"自然退出后再Start"用例。
- Write短写：单次write无循环（:364-367），pty缓冲约4KB，大段粘贴静默截断。对策：循环写满（EIO按会话已终处理），补>4KB输入用例。
- master fd无`FD_CLOEXEC`（:313）：泄漏进ncc/ndb/WebEngine helper等全部子进程。对策：一行`fcntl`。

### 源码级平台面（盘点结论：代码跨平台化程度高）

- **大小写折叠五处已有`#ifdef`双臂**（ProjectPaths.h:19、BreakpointStore.cpp:19、FileEditor.cpp:16、RecentStore.cpp:17、MainWindowSolution.cpp:48-52）：NTFS折叠/POSIX保持，零改动。
- **编码防护已到位**：`QTextStream::setCodec("UTF-8")`显式钉死（CodeFileEditor.cpp:45,60）、ncc输出按UTF-8解码（MainWindowBuildRun.cpp:91-94）、行尾`\r`剥除双兼容（RunErrorBrowser.cpp:30-32，ConPTY给CRLF/Linux pty给LF）。
- **QSettings**（org "NLang"/app "nide"，main.cpp:13-17）：Qt原生后端差异（Windows注册表/Linux ~/.config ini），API零改动。
- **工具调用无`.exe`硬编码**（同目录裸名，MainWindow.cpp:212-215；测试侧路径走`$<TARGET_FILE:...>`生成器表达式）；ncc同步QProcess、ndb经DebugClient的QProcess（停止=`QProcess::kill`，Linux语义SIGTERM）、nvm经PtyProcess终端运行。
- 行为差异记录（不改动）：文件名禁用字符表在Linux更严（MainWindowSolution.cpp:24）、终端字体Consolas硬编码有`QFont::Monospace`兜底（TerminalWidget.cpp:38-39）、`DebugClient.cpp:14`的"kill()即TerminateProcess"注释在Linux下语义过时（顺手更正）。

### 测试面

- 现状：widget测试全部只设`QT_QPA_PLATFORM_PLUGIN_PATH`跑真实QPA，**无offscreen配置**；tests/CMakeLists.txt:2046-2048注释"offscreen无部署字体在QMessageBox布局崩溃"——那是Windows无字体场景的结论，盒上fontconfig有410项字体（Noto CJK在位），Linux offscreen的字体前提成立，仍需N2实测确认。
- **terminal_tests的子进程硬编码cmd.exe**（test_terminal.cpp:974-1010：`cmd /c echo`、`cmd /c exit 3`、交互cmd）：跨平台化改写——按平台选shell（POSIX用sh），PTY测试本体无头可跑。
- **mainwindow_tests**（115+测试函数，900s上限）真ncc/nvm/ndb全真驱动；模态框经零定时器驱动+`AA_DontUseNativeDialogs`（test_mainwindow.cpp:606-615）；QSettings定向"nide-test"隔离；帮助浏览器测试按objectName找`QWebEngineView`（HelpBrowser.cpp:34-35专为测试命名）；Qt 5.15 Debug下WebEngine两用例的QSKIP由`#ifdef _DEBUG`守卫（test_mainwindow.cpp:4115-4123、:4196）——那是MSVC专属宏，**Linux Debug下永不触发**，即Linux按真跑处理，行为按N2实测定（若确需跳过，改用CMake注入宏而非_debug）。
- **测试包装器qtest_progress.py的POSIX kill臂缺口**（tools/qtest_progress.py:86-92）：`os.kill(pid, 9)`只杀直接子进程且未用进程组——挂起时nvm/ndb孙进程成孤儿，违反tests/CMakeLists.txt:1423-1424明言的"a stuck nvm/ndb debug child cannot outlive the suite"。对策：Linux臂`start_new_session=True`+`os.killpg`，归N1/N2改动面。
- **nide_deploy_check**（tests/test_nide/nide_deploy_check.cmake）整篇Windows形态：.exe名（:37-47）、7个`nlang_*.dll`复制（:79-87）、qt.conf `Prefix=.`（:102）、五探针含活PTY（:130-137）；其检查目录取`$<TARGET_FILE_DIR:nide>`（Windows的POST_BUILD自含拼装前提）——Linux按spec对策不自含，**N4的Linux检查目录改从install树取**（`cmake --install`到staging prefix，add_test管道同步改传参）。对策：N4落Linux形状，结构对齐。
- AGENTS「涉及IDE的修改必须包含浏览器交互验证（Playwright等）」查证=mkdocs站点+嵌入帮助浏览器工作流（master的bilingual-support spec:101,127；Playwright脚本在master不在本仓库），对应本任务的验证路径是mainwindow_tests的QWebEngineView用例，不引入新工具。

### 部署面

Linux交付=portable tar.gz自含目录，对齐Windows portable zip的既有形状（顶层CMakeLists.txt:430-447的deploy-dir布局：可执行与Qt运行时同居bin/，nide从自身目录拉起工具）：`bin/nide`+ncc/nvm/ndb+`libnlang_*.so`+Qt库+`platforms/`+WebEngine闭包（按自编译prefix布局取自`<prefix>/libexec|resources|translations`；qt.conf `Prefix=.`下Linux的helper查找走`LibraryExecutablesPath`即`<root>/libexec/`，与Windows"与exe同目录"形状不同，闭包清单须落`libexec/QtWebEngineProcess`或qt.conf显式设LibraryExecutables），`$ORIGIN`rpath+`qt.conf`重定向插件/资源根。这是linuxdeployqt的原理自实现——不引入打包工具、不做AppImage/deb（内部工具，无跨发行版诉求，KISS）。注意：install/CPack规则的既有验证者是tests/packaging/verify_package.py（Windows zip/NSIS专用、未注册ctest），**install规则的Linux闭包没有现成验证载体**——N5的Linux包冒烟验证是新落物。CPack现有`WIN32`内ZIP/NSIS（:485-505），Linux加TGZ分支（N5）。具体拷贝清单在实施计划定。

## 里程碑

对齐核心移植的M1-M5形状（标N避免混淆）：

| 里程碑 | 内容 | 验收 |
| --- | --- | --- |
| N1 构建绿 | IDE树configure+build零错（libvterm/WebEngine/lrelease全链；`nide.rc`条件化、Linux工具拷贝臂、**测试exe工具复制UNIX臂**、moc守卫、WebChannel/Positioning TARGET守卫） | 盒上build-ide零错（已达成）；Windows IDE树构建+ctest不回归 |
| N2 冒烟 | offscreen下nide启动-退出闭环；PtyProcess POSIX臂真验证（spawn+输出回读+尺寸变更+退出码收割）+四缺口修复（workingDirectory/陈旧pid/短写/CLOEXEC） | 自动化脚本绿（已达成）；offscreen下QMessageBox布局与WebEngine初始化实测结论落档（已落） |
| N3 单测全绿 | IDE树ctest **30条**全量（31条−deploy_check，Linux，offscreen；**deploy_check归N4，不进本条验收**）；terminal_tests的shell跨平台化在此完成 | 0 fail；与Windows IDE树计数差异逐条注明平台性 |
| N4 部署检查 | nide_deploy_check的Linux形状（so清单/qt.conf/探针按POSIX pty）；**检查目录改从install树取**（staging prefix）；自含闭包按自编译prefix布局新写 | Linux deploy_check绿 |
| N5 文档版本 | README依赖表更正+Linux安装/构建段；CPack TGZ分支+**Linux包冒烟验证**；手册同步；CHANGELOG；**VERSION提升** | 中英成对；SYNC_BASELINE推进 |

门禁节奏沿用轻门禁裁决（`multi_gpu_impl_plan.md`§7同款）：中间里程碑挂受影响测试族+盒上实编，收口一次全量（Linux侧=IDE树ctest全量+冒烟；Windows侧=IDE树ctest+dbg/asan单元+cpu e2e受影响族+**IDE树Release构建**——打包路径的验证腿，nide线独立收口不并入0.14.13批次）。

## 风险

- **WebEngine无显示初始化**（已实测关闭）：N2冒烟证明offscreen下WebEngine栈不崩、无需Chromium flag（tools/nide_offscreen_smoke.sh，2026-11-27）；mainwindow_tests的QWebEngineView用例在N3真跑定行为。Windows的5.15 Debug渲染进程QSKIP由`_DEBUG`守卫，Linux下不触发。
- **offscreen字体**：mainwindow测试块的"offscreen在QMessageBox布局崩溃"注释（tests/CMakeLists.txt:2047-2048）出自Windows无部署字体场景；盒上fontconfig 410项字体（Noto CJK在位）前提下预期不成立，N2以真用例实证后关闭或确认。
- **剪贴板/键盘语义**：X11 PRIMARY selection与Windows clipboard的差异是行为差异非缺陷，记录进文档不阻塞。
- **libvterm内建拷贝**：上游本是Linux项目（纯C99、9个.c、无平台耦合，src/3rdparty/libvterm/CMakeLists.txt:3-16），风险低。
- **Qt资源路径**：qrc/翻译/qm嵌入库内（src/tools/nide/CMakeLists.txt:216-241生成qrc），路径与平台无关，预期零改动。
- **POSIX臂首验**：编译面已过（N1预演BUILD_RC=0，GNU首见）；行为面（信号/waitpid/EOF收割、四个缺口修复）由N2真验收口。
- **moc解析面**：IDE树给核心测试挂QtTest后AUTOMOC跟入重C++头的解析冲突已全量守卫（测试cpp 14处+项目头7处），新增测试若再include重头需同样加`Q_MOC_RUN`守卫——守卫是惯用法、编译器语义零变化。

## 开源参考

GitHub/raw通道两端不可达（TLS重置，本地与验收盒双实证在案，见记忆台账）。forkpty+QSocketNotifier模式以man7.org man页（[openpty(3)](https://man7.org/linux/man-pages/man3/openpty.3.html)）与社区实践为准；无头测试策略引证[Qt论坛CI讨论](https://forum.qt.io/topic/133749/adding-gui-tests-to-ci-without-display)与[pytest-qt排障指南](https://pytest-qt.readthedocs.io/en/latest/troubleshooting.html)（offscreen为主、显示敏感上xvfb）；部署策略引证[linuxdeployqt原理](https://discourse.appimage.org/t/linuxdeployqt-new-linux-deployment-tool-for-qt/57)与[$ORIGIN rpath实践](https://stackoverflow.com/questions/27384406/setting-rpath-order-in-qmake)——工具本身不引入，自实现同等原理。libvterm用仓库vendored拷贝。
