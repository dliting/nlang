# 用户手册编辑规范

本文件是`docs/user_manual/{zh,en}`双树编辑政策的单一权威源，人类编辑与自动化工具都以这里为准。第2节的术语对照表被缩略语共现守卫直接解析（`tools/nlang-docs/src/nlang_docs/manual_style.py`，随 docs 测试套件运行），因此该表是增删受控缩略语的唯一入口：新发现的缩写先查证双语全称、加进表，再让扫描结果归零。

## 1. 政策

### P1 零内部进度（硬政策）

手册不得出现：阶段/Step/轮次引用、提交号、设计状态日期、「剩余工作」类段落、内部文档路径（`docs/dev/`、`docs/superpowers/`）。CHANGELOG是唯一允许叙述版本演进的位置。

### P2 缩略语首现展开

- 每页独立计算首现：正文散文与表格说明文字中的首字母缩略语一律在首现处展开，此后同页用缩写。
- 中文格式：`伪随机数生成器（PRNG，Pseudo Random Number Generator）`；英文格式：`pseudorandom number generator (PRNG)`。
- 豁免：行内代码（反引号）、代码块、命令行、文件名、格式扩展名（`.n`/`.ncu`/`.npkg`）。
- 标题（含nav标题）用短形，正文首现处展开。
- 专有产品/项目名不展开：NLang、Qt、Python、Windows、Linux、LLVM、Flex、Bison、mkdocs、Material、NSIS、CPack、Unicode、VM、Visual C++（写作VC++/MSVC）、IEEE 754、POSIX、JavaScript（写作JS）、FNV-1a。
- 双语译名以第2节术语表为单一源。

### P3 行文风格

避免：空洞总结句（「总之」「综上」收尾段）；模板化排比与列表条目同头词；填充语（「值得注意的是」「需要指出的是」）；过度加粗；破折号滥用；每节机械地以列表收尾；翻译腔长定语。要求：动词短句、例子先行、表格只用于真实枚举；段落承载论证，列表只列并列项；两树同一事实同一语气。

### P4 紧凑空格（规则 A 由守卫强制）

- 规则A（守卫强制）：汉字/全角标点与拉丁字母、数字之间不加水平空格（双向，多空格等同违规）。
- 规则B（归一脚本执行，不做守卫）：汉字或全角标点与行内代码span、方括号链接、圆括号、星号强调之间不加空格；汉字之间、汉字与全角标点之间同样不加空格。下划线、竖线、井号边界不动（markdown安全边界）。
- 豁免：围栏代码块内部、行内代码内部、front-matter、URL。新写内容一律紧凑风格。

### P5 事实零变更

既有语义、命令、示例行为不改。新增事实仅限新章内容与依赖清单，且依赖清单以install-layout.md与实测为准。

### 双树平价纪律

nav路径两树有序一致（tree parity守卫）；两树同一事实同一语气；改一树必须同步另一树（根部文档成对维护规则在手册内的延伸）。

### 历史政策归并

2026-09-21手册新手友好化项目的R1（去阶段引用）并入P1，R4（事实零变更）并入P5；R2（内部标识符概念化：机制名优先于源码符号，例外仅`OP_*`/`.nmod`字段/内建方法名/`std::`）与R3（现在时、术语首现即定义、zh-en语义一致）继续独立有效，由审核循环裁量。

## 2. 术语对照表（守卫解析源）

本表是本文件内唯一的三列表格（守卫只解析本文件，不扫手册正文）；「缩写」列按大小写敏感匹配正文，全称按不区分大小写匹配同页共现。

| 缩写 | 中文全称 | 英文全称 |
|---|---|---|
| PRNG | 伪随机数生成器 | pseudorandom number generator |
| ABI | 应用二进制接口 | application binary interface |
| API | 应用程序编程接口 | application programming interface |
| DLL | 动态链接库 | dynamic-link library |
| UTF-8 | Unicode转换格式 | Unicode Transformation Format |
| ASCII | 美国信息交换标准代码 | American Standard Code for Information Interchange |
| GC | 垃圾回收 | garbage collection |
| AST | 抽象语法树 | abstract syntax tree |
| IDE | 集成开发环境 | integrated development environment |
| XML | 可扩展标记语言 | Extensible Markup Language |
| BOM | 字节顺序标记 | byte order mark |
| CRLF | 回车与换行 | carriage return and line feed |
| CLI | 命令行界面 | command-line interface |
| EXE | 可执行文件 | executable |
| JNI | Java本地接口 | Java Native Interface |
| AI | 人工智能 | artificial intelligence |
| BMP | 基本多文种平面 | Basic Multilingual Plane |
| CI | 持续集成 | continuous integration |
| EOF | 文件结束 | end of file |
| EOS | 流结束 | end of stream |
| ID | 标识符 | identifier |
| IO | 输入输出 | input/output |
| NPE | 空指针异常 | null pointer exception |
| TU | 编译单元 | translation unit |
| UTF-16 | Unicode转换格式 | Unicode Transformation Format |

## 3. 政策与执行映射

| 政策 | 执行方式 |
|---|---|
| 双树平价/nav一致 | tree parity守卫（docs pytest） |
| 片段真实可运行 | snippets审计（```nlang围栏真编译真运行） |
| 链接有效 | mkdocs `--strict`构建＋linkcheck |
| 零前代痕迹 | public_text守卫 |
| P4规则A（紧凑空格） | manual_style紧凑空格守卫 |
| P2缩略语共现 | manual_style缩略语共现守卫（表来源＝第2节） |
| P1零内部进度 | 审核循环裁量（守卫只覆盖前代痕迹子集） |
| P3行文、R2概念化、R3术语定义 | 审核循环裁量 |
| P5事实零变更 | 审核循环裁量（事实修正须单独举证实测） |

## 4. 样例纪律与守卫用法

本文件与守卫源码展示违规样例时一律用行内代码包裹（守卫豁免代码区，防自伤，同public_text.py自指豁免思路）。本文术语表须保持本文件内唯一的三列表格——解析器按「三竖线列」行模式匹配，若将来在本文件增加其他三列表格，须同步收紧`parse_term_table`的锚定（例如加表头文字判断）。

守卫手跑方式（不必重新configure）：

```text
PYTHONPATH=tools/nlang-docs/src D:/dev/miniconda3/envs/py313/python.exe -m pytest tools/nlang-docs/tests/test_manual_style.py -q
```

ctest 入口为 `nlang_docs_pytest`（随全套串行跑）。
