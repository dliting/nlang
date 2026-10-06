# fs——名字、目录、元数据

`fs`从不读写内容——内容属于`io`。这一划分是操作原则：io = 全部内容（控制台 + 磁盘文本），fs = 命名空间/目录/元数据。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| exists / isFile / isDirectory | (string) → bool | 从不抛错 |
| size | (string) → int | 字节大小；仅普通文件 |
| listFiles | (string) → List\<string\> | 仅名字，非递归，仅普通文件，按字典序排序 |
| makeDirs | (string) → void | mkdir -p（幂等） |
| remove | (string) → void | 文件或空目录 |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()`——处处正斜杠 |

**错误模型**：`size`/`listFiles`/`makeDirs`/`remove`失败时抛`IOException`。三个谓词从不抛错——无法陈述的路径（不存在或不可访问）直接回答`false`。对缺失路径`remove`是静默无操作。对目录或特殊文件`size`抛IOException（目录的「大小」是文件系统噪音）。

**join的边界语义**（std::filesystem路径追加，与Python
`os.path.join`相同）：带根的右侧（`"/b"`）会**替换**左侧；空的右侧留下结尾分隔符（`join("a","")`是`"a/"`）；空的左侧得到右侧本身。

**Windows编码约定**：路径与文件名经进程活动代码页转换（`generic_string`、文件打开）。命令行工具以Unicode转换格式（UTF-8，Unicode Transformation Format）为进程活动代码页运行（内嵌清单声明，Windows 10 1903+生效），非美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）路径与文件名按UTF-8往返，`io.readFile`/`writeFile`/`appendFile`同样适用；以系统代码页运行VM的嵌入宿主仍受该代码页限制。
