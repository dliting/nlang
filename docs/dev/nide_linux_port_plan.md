# nide Linux 移植实施计划

依据`nide_linux_port_design.md`（下称spec）。每阶段改动→验证→记录（台账），中间里程碑挂受影响测试族+盒上实编（轻门禁裁决），收口一次全量。Windows侧回归钉全程保持：IDE树ctest+dbg/asan单元+cpu e2e受影响族。

## 阶段0：环境预演（已达成，2026-11-27）

- 盒上`build-ide`树configure+build（HEAD 9d3f0451起步，`-DNLANG_BUILD_IDE=ON -DNLANG_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/qt/5.15`+CUDA/flex/bison参数对齐build-cuda树）。
- 产出四类发现并已当场修复（见阶段1改动清单）：configure期`$<TARGET_FILE:Qt5::WebChannel/Positioning>`无目标炸generate；MSVC函数式cast×6（TerminalEmulator.cpp:300-305）；`static const int`类内常量ODR-use链接失败（CodeEditor.h:53）；AUTOMOC跟入`<filesystem>`在GCC 11的`bits/fs_fwd.h`炸moc（测试cpp直include 14处+项目头链7处）。
- 末轮`BUILD_RC=0`：nide可执行+ncc/nvm/ndb三工具全部编出。

## 阶段1：CMake构建面跨平台化（N1）

改动全集（第1-4、6项随阶段0预演落地；第5项为审核H1补落，随阶段3前同步盒上验证）：

1. `nide.rc`移入`if(WIN32)`条件源（src/tools/nide/CMakeLists.txt）；POST_BUILD加`elseif(UNIX)`臂：拷`$<TARGET_FILE:ncc/nvm/ndb>`三工具到`$<TARGET_FILE_DIR:nide>`（`toolPath()`同目录裸名依赖，MainWindow.cpp:212-215）。Qt库/platforms插件不拷（prefix运行时查找，已实证）。
2. 顶层CMakeLists.txt：WebEngine闭包DLL清单的WebChannel/Positioning按`if(TARGET)`守卫拼装（缺模块的Qt不再炸generate）；install规则的Qt闭包段（:451-470）包`if(WIN32)`（Linux闭包归阶段4）。
3. TerminalEmulator.cpp函数式cast改`static_cast`；CodeEditor.h类内常量改`static constexpr`（implicit inline，ODR-use安全）。
4. moc守卫：`#ifndef Q_MOC_RUN`包裹`#include <filesystem>`——测试cpp 14处+项目头7处（LibrarySearchPath.h/NLang.h/ModuleRegistry.h/ModuleSaver.hpp/SourceCompiler.h/test_scratch.h/test_debugger_common.h）。
5. **测试exe的工具复制UNIX臂**（第三方审核H1，已核实）：`test_mainwindow`与`test_searchpath_integration`的WIN32-only POST_BUILD工具拷贝（tests/CMakeLists.txt:2065-2078、:2134-2144）加`if(UNIX)`臂——否则Linux下mainwindow_tests百余用例与searchpath_integration两条全红。
6. tools/qtest_progress.py的POSIX kill臂（:86-92）改`start_new_session=True`+`os.killpg`（审核M4：孙进程孤儿违反:1423-1424的suite存活保证）。

验证：盒上build-ide全量零错（已过）；Windows build-ide树重建+IDE树ctest不回归（后台进行中）。

## 阶段2：PtyProcess POSIX臂真验证+四缺口修复（N2）

1. workingDirectory：`SpawnPosix`签名与`Start` POSIX臂传workingDirectory（PtyProcess.cpp:156-158、:183-184），fork后exec前`chdir`——chdir失败子进程以`_exit(126)`终结（exec约定码126=无法进入执行），父进程reader的waitpid读到126退出码照常走Finished链路，Run输出面板呈现非零退出；补测试钉住"Run产物目录"语义。
2. 陈旧pid（审核M2）：waitpid收割后置`childPid=-1`（:459处），`Kill()`加`childPid>0`守卫（:424）；补"自然退出后再Start新会话"用例。
3. Write短写（审核M3）：POSIX臂循环写满（:364-367），EIO按会话已终处理；补>4KB输入用例。
4. master fd加`FD_CLOEXEC`（:313，审核L4）。
5. offscreen冒烟脚本（tools/，可重用）：`QT_QPA_PLATFORM=offscreen`下nide启动→MainWindow构造→定时退出，exit 0。
6. terminal_tests跨平台化：test_terminal.cpp:974-1010的cmd.exe用例按平台选shell（POSIX用`sh -c`；Linux pty行规范模式的回车语义按平台分支）。
7. N2实测结论落档：offscreen下QMessageBox布局（对照tests/CMakeLists.txt:2046-2048的Windows场景结论）、WebEngine初始化行为（Linux Debug下`_DEBUG`守卫的QSKIP不触发，test_mainwindow.cpp:4115-4123，真跑定行为）。

验证（2026-11-27实测）：冒烟**PASS**（tools/nide_offscreen_smoke.sh，offscreen下启动8秒存活+SIGTERM干净退出rc=143——**WebEngine在无显示环境初始化不崩，无需Chromium flag**，"propagateSizeHints"警告无害）；terminal_tests盒上绿（1.03s，PTY真子进程四用例全过——workingDirectory/chdir、Write循环、Kill守卫、CLOEXEC修复后的POSIX臂行为面成立）；Windows侧terminal_tests+mainwindow_tests不回归（随N1回归批验证）。

## 阶段3：测试全绿（N3）

1. `nlang_add_qtest`包装器（tests/CMakeLists.txt:1412-1436）统一注入：Linux下widget测试设`QT_QPA_PLATFORM=offscreen`（平台条件，Windows维持真实QPA不动）。
2. **IDE树30条**ctest盒上全绿（31条−`nide_deploy_check`，后者归阶段4不进本条验收——审核H2的调序裁决）；与Windows差异逐条处置并记录（预期集中在WebEngine用例与平台语义差异）。
3. `DebugClient.cpp:14`注释顺手更正（kill语义平台差异）。

验证（2026-11-27实测，三轮收敛）：**终局sweep 30条全量0 fail**。失败分三层逐层修：①机制层7条——8处显式`ENVIRONMENT`赋值按CMake替换语义覆盖掉包装器的offscreen QPA（无DISPLAY下QApplication abort），包装进`if(WIN32)`并留REPLACE语义戒条注释；②数据层6条——fixture的Windows形状路径在以`QDir::isAbsolutePath`分支的产品逻辑下失真（searchpathargs/settingsstore/compilelogbrowser/dialogs），按平台常量化（期望值同构构建）；③语义层4条——大小写折叠与文件锁的产品行为本身平台分叉（projectmodel两处CaseVariant、mainwindow锁下重命名），按各平台语义双向断言（POSIX分支验证自己的行为而非跳过）。Windows侧mainwindow_tests重跑全绿（Windows分支逐字未动）。

**Windows回归**：IDE树9条（nide族）225.78s全绿+mainwindow_tests单条重跑绿——阶段1-3全部改动MSVC零回归。

## 阶段4：部署检查Linux形状（N4）

1. install规则POSIX臂（顶层CMakeLists.txt:447-471的`if(WIN32)`对面）：nide+ncc/nvm/ndb+`libnlang_*.so`入bin/、Qt库、platforms、WebEngine闭包（按自编译prefix布局取`<prefix>/libexec|resources|translations`；qt.conf `Prefix=.`下helper走`<root>/libexec/`，闭包落`libexec/QtWebEngineProcess`或qt.conf显式设LibraryExecutables——审核I2）；`nide`目标加`INSTALL_RPATH=$ORIGIN`；qt.conf写入部署根。
2. `nlang_copy_qtwebengine_runtime`保持WIN32-only。
3. nide_deploy_check.cmake加Linux分支：**布局从install规则现装配**（`cmake --install`到CHECK_DIR——审核M1：Linux的build目录按阶段1对策不自含，Windows版"整目录拷走`$<TARGET_FILE_DIR:nide>`"前提不成立）；探针二进制带`Qt5::Test`库拷进bin/与部署的nide/工具同层，五探针从bin/以部署视角运行（`cmake -E env QT_QPA_PLATFORM=offscreen`——execute_process无ENVIRONMENT参数，首版踩坑即修）；库隔离不做（ELF RUNPATH优先于qt.conf，剥离需patchelf——portable布局的前提是prefix Qt，脚本头注释如实声明，Windows版全量隔离逻辑原样保留）。

验证（2026-11-27实测）：Linux deploy_check绿（1.46s——install布局装配+五探针自含解析全过）；Windows deploy_check随回归批验证（WIN32分支逻辑未动，仅add_test管道换生成器表达式传参）。

## 阶段5：文档与版本（N5）

1. README依赖表更正（Qt模块加WebEngineWidgets）+Linux构建/安装段；README.zh-CN.md成对。
2. CPack Linux分支：TGZ生成器、包名`-linux64`（对照:485-505的WIN32分支）；**Linux包冒烟验证**（verify_package.py加TGZ臂或一次性清单——审核M5：install/CPack的Linux闭包无现成验证载体）。
3. **VERSION提升**（审核L5：用户可见功能变更同步提升VERSION的义务不分分支）+CHANGELOG新版本节记录Linux平台支持（开放项：按次版本提升为0.15.0——新平台支持属功能级；在0.14.13收口发布之后收口本任务，收口时按届时HEAD再核）。
4. 手册核对：`docs/user_manual`是否含平台限定表述，有则中英成对更正并跑`tools/manual_sync_check.py`；README不在五类同步面内，但公开文本零前代痕迹门禁仍适用。

## 收口

全量一次：盒上IDE树ctest全量+冒烟+deploy_check；Windows IDE树ctest+dbg/asan单元+cpu e2e受影响族+**IDE树Release构建**（打包路径验证腿，nide线独立收口不并入0.14.13批次——审核I3）；文档五腿（动README/CHANGELOG则跑docs检查）。台账记于本文档逐阶段追加。版本提升与CHANGELOG在收口提交内完成；提交信息Conventional Commits英文；不推送。
