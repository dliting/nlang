# Linux 移植设计（语言/编译器/VM 核心，不含 IDE）

## 背景

多GPU验证需要真跑双卡（2×RTX 4090 Linux 验收机，CUDA 12.1，见
`docs/dev/multi_gpu_linux_box.md` 与记忆台账）。NLang 核心此前只在 Windows
构建过，先要让语言、编译器、VM、native 标准库模块（含 tensor 与 CUDA
插件）与 ncc/nvm/ndb 工具在 Linux 上构建并全绿，多GPU验收才有载体。

用户裁定（2026-11-25）：**先不移植 nide**（依赖 Qt），专注语言/编译器/VM
的张量运算线。Python 在 Linux 用 `nlang` conda 环境；Windows 暂仍用
`py313`。

## 范围

**入**：核心库（runtime/langservice/compiler/vm/embed）、native 模块
（math/io/fs/net/system/gzip/tensor + CUDA 插件）、ncc/nvm/ndb/ndisasm/nlang
工具、stdlib.npkg 引导、单元测试、e2e 套件（cpu 与 cuda 后端）、CUDA 插件
Linux 构建。

**出**：nide（Qt）、文档站点（`NLANG_BUILD_DOCS=OFF`）、ASan 腿（Linux 上
后续单独立项）、macOS（`__APPLE__` 臂已存在，不在本次验证面）。

## 现状（已核实，非推测）

代码库跨平台化程度远高于早期估计：

| 设施 | 状态 |
|---|---|
| `NativeLibraryLoader.cpp` | 三平台臂（WIN32/Apple/POSIX）齐全，模块名 `libnlang_<ns>.so` 约定已在 |
| `IoPollerPosix.cpp` | 已存在，CMake 已按平台分支 |
| net 模块 | `net_platform_posix.cpp`/`net_platform_icmp_posix.cpp` 已在 |
| 工具入口（ncc/nvm/ndb/nlang/ndisasm） | POSIX 臂已在（argv/exit/进程号/可执行文件路径） |
| tensor SIMD | 非 MSVC 臂已在（`cpuid.h`、`-mavx2;-mfma`） |
| e2e runner | 工具路径为位置参数，显式传入即跨平台 |
| 测试 scratch | C++ 侧走 `std::filesystem::temp_directory_path()`，已跨平台 |

真实缺口（Linux 构建实测 + 源码核查）：

1. `include/nlang/vm/NativeHost.h` 用 `size_t` 未含 `<stddef.h>`（MSVC 经由
   windows.h 传递带入，GCC 不）。已在工作树修复，未提交。
2. **四处类内纯虚函数带函数体**（`= 0 {}`）：标准不合法（纯虚函数的定义
   不得在类定义内，[class.pure]；MSVC 按扩展接受，GCC 拒绝并报
   "pure-specifier on function-definition"，其后一串 private/未声明错误
   是解析失败的级联噪音）：
   - `include/nlang/compiler/Logger.h:25` `~CompileLogger`
   - `include/nlang/runtime/Node.h:26` `~INodeVisitor`、`:39` `~Node`
   - `include/nlang/runtime/NodeIterators.h:30` `~NodeIteratorBase`
   实测：去掉类内空体后，纯虚析构若**无处定义**，派生类析构销毁基类子
   对象时链接报 undefined reference（纯虚析构必须给出类外定义）。
3. `src/native/tensor/backend_registry.cpp`：CUDA 插件探针整段 Windows
   （`GetModuleHandleExA`/`GetModuleFileNameA`/`LoadLibraryA`/`GetProcAddress`
   + 硬编码 `nlang_tensor_cuda.dll`）。
4. `src/native/CMakeLists.txt` CUDA 插件段：`nvcc.exe`、`-ccbin cl.exe`、
   `cudart_static.lib`、`/NODEFAULTLIB:LIBCMT`、`.obj`，全 Windows。
5. `tests/CMakeLists.txt`：`$ENV{TEMP}` 定 scratch 根（POSIX 下为空→路径落
   文件系统根，configure 即失败）；ctest `ENVIRONMENT` 只设 Windows 变量名。
6. 后续构建阶段可能还有 MSVC 宽松点（按编译顺序暴露，逐个语义中性化处理）。

## 设计决策

### D1 纯虚析构：类外定义，保持抽象语义

四处全部：删类内空体，保留 `= 0`；补类外空定义（析构是纯虚且基类子对象
销毁会实调它，链接需要）。定义落点：

- `Node`、`INodeVisitor` → `src/runtime/Node.cpp`（Node 成员的家）。
- `NodeIteratorBase` → 头内类外 `inline` 定义（该头无 .cpp 家，头内类族）。

`~CompileLogger` → `src/compiler/Logger.cpp`（已存在）。

Windows 侧语义中性（MSVC 原来就把类内空体当定义用），但动的是共享源，
收口前须过 Windows 四腿。

### D2 tensor 插件探针跨平台化

`ProbeCudaPlugin()` 保持单一函数，内部按平台分臂：

- 模块目录：Windows 维持 `GetModuleHandleExA(FROM_ADDRESS, &ProbeCudaPlugin)`；
  POSIX 用 `dladdr((void*)&ProbeCudaPlugin, &di)` 取 `dli_fname`（地址必落在
  libnlang_tensor.so 段内，dladdr 按映射区间定位对象，不依赖符号导出）。
- 插件名：`nlang_tensor_cuda.dll`（Windows）/ `libnlang_tensor_cuda.so`
  （POSIX），常量按平台定。
- 加载/取符号/关闭：`LoadLibraryA`/`GetProcAddress`/`FreeLibrary` 对
  `dlopen(RTLD_NOW|RTLD_LOCAL)`/`dlsym`/`dlclose`。

不抽公共头：三个助手各三四行，为它们加一层共享设施不值（KISS）；tensor
模块只见 `NativeHost.h` 稳定 ABI 头，不宜再挂 vm 层头。

### D3 CUDA 插件 CMake 双平台

- nvcc 路径：`bin/nvcc.exe`（Windows）/ `bin/nvcc`（POSIX）。
- 架构：维持 `NLANG_CUDA_ARCH` 缓存变量（默认 sm_86），验收机 configure
  时传 `sm_89`（RTX 40 系）。单架构/次构建，可重配。
- nvcc 参数：POSIX 臂 = `-arch=<arch> -std=c++17 -O3 -fmad=false
  -Xcompiler -fPIC,-O3,-std=c++17`（`-fmad=false` 与 Windows 一致，保证
  跨平台核函数数值同值；`-fPIC` 因链接进 .so）。
- 链接：Windows 维持 `cudart_static.lib`+`/NODEFAULTLIB:LIBCMT`；POSIX 链
  动态 `libcudart.so`（`-L<cuda>/lib64`）+ `libcuda`。CUDA 运行时随插件
  .so 动态加载，宿主进程不直接依赖 CUDA 符号（RTLD_LOCAL 无交叉置入）。
- 目标名/产物名：CMake 默认即 `libnlang_tensor_cuda.so`（Linux）/
  `nlang_tensor_cuda.dll`（Windows），与 D2 常量一致，无需特判。
- 中间目标后缀：`CMAKE_CXX_COMPILER_RECOGNIZED_OBJECT_SUFFIX`（`.o`/`.obj`）。

### D4 测试 scratch 根可移植

`tests/CMakeLists.txt`：configure 期按平台取根（`TEMP`→`TMPDIR`→`/tmp`
兜底）；ctest `ENVIRONMENT` 按平台设 `TEMP;TMP`（Windows）或 `TMPDIR`
（POSIX）。

### D5 验收机构建配置

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DNLANG_BUILD_TESTS=ON -DNLANG_BUILD_IDE=OFF -DNLANG_BUILD_DOCS=OFF \
  -DNLANG_CUDA_ROOT=/usr/local/cuda -DNLANG_CUDA_ARCH=sm_89 \
  -DFLEX_EXE=/usr/bin/flex -DBISON_EXE=/usr/bin/bison
cmake --build build -j64
```

## 里程碑与验收

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 核心可构建 | D1-D5 落地 | ncc/nvm/ndb + 七 native 模块 + stdlib.npkg 全编出 |
| M2 冒烟 | hello 级 .n 编译运行；tensor 导入 | `NLANG_TENSOR_BACKEND=cpu` 下 tensor 冒烟过 |
| M3 单测全绿 | ctest 全量 | 0 fail |
| M4 e2e 全绿 | 2572 条，cpu 后端 | 0 fail（与 Windows 同套 manifest） |
| M5 CUDA | 插件编出、cuda 后端生效 | `NLANG_TENSOR_BACKEND=cuda` 设备数≥1；tensor e2e 子集 cuda 绿；mnist 单卡跑通 |

M5 绿即多GPU验收的载体就绪（多GPU特性本身另立任务）。

## 验证纪律

- 每批修复在验收机增量重编验证，不攒批盲改。
- 共享源变更（D1/D2/D3/D4 + NativeHost.h）语义中性，但收口前过
  **Windows 四腿**（dbg_unit + asan_unit + cpu_e2e + cuda build-bench），
  不允许只验 Linux。
- e2e 钉值不变：cpu 腿是全量数值钉；cuda 腿验证设备臂。

## 风险与开放项

- 后续构建阶段（compiler/vm/tools 目标）可能再暴露 MSVC 宽松点；按同一
  纪律（标准合法 + Windows 语义中性）逐个处理，不引入平台分支扩张。
- `RuntimeNode m_upChildren` private 报错目前判为 D1 的级联噪音；D1 落地
  后复验，若仍在则单独查。
- Linux 动态 CUDA 链接若 ldconfig 未含 CUDA lib64，dlopen 报
  `libcudart.so.12` 缺失——验收机已装 CUDA 12.1，先按系统 ldconfig 走，
  缺失再议 rpath（不为小概率预先加）。

## 台账

- 2026-11-25 定稿：现状核查（IoPollerPosix/net posix 臂/工具 POSIX 臂
  均已存在，缺口收窄为 D1-D5）；纯虚析构标准条款经 cppreference 与 GCC
  最小用例双重确认；纯虚析构类外定义必要性经链接实测确认。
