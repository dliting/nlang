# NLang示例索引

每个示例聚焦一个主题，可独立编译运行。三种运行方式见
`docs/user_manual/zh/getting-started/running.md`（nide帮助菜单 → NLang入门）。

命令行方式（在仓库根目录；安装包用户在安装根目录运行，ncc/nvm位于`bin\`下）：

    ncc build examples/<name>.n -o <name>.ncu
    nvm <name>.ncu

安装包内无PATH时使用`bin\ncc build ...`、`bin\nvm <name>.ncu`。

`stdlib_io`需要一行stdin：`nvm stdlib_io.ncu < examples/stdlib_io.stdin`。
`net_*`示例带有`.flags`文件：测试门禁以`nvm --carriers=1 <name>.ncu`运行它们，把运行钉在单个载体上。
成功运行的退出码列在「退出码」列（Windows限0-255）。

| 示例 | 演示 | 退出码 |
|---|---|---|
| hello.n | 最小程序 | 42 |
| add.n | 函数与返回值 | 42 |
| class_basic.n | class字段/构造器/方法/toString | 3 |
| inheritance_super.n | extends/super()/覆盖/虚分派 | 4 |
| exceptions.n | throw/try/catch/finally/内建异常/用户子类 | 5 |
| list_tour.n | `List<T>`/add/contains/indexOf/foreach | 6 |
| dict_tour.n | `Dict<K,V>`/containsKey/keys()/foreach | 7 |
| array_initlist.n | 定长数组/`[...]`初始化器/length | 8 |
| interpolation_tour.n | `"${}"`插值/toString隐式转换 | 9 |
| cast_operator.n | as拆箱与引用下转（数值转换走隐式赋值） | 10 |
| const_assert.n | const局部/assert/增量赋值 | 11 |
| out_params.n | out参数多返回值 | 12 |
| ndarray_tour.n | 矩形多维数组/逗号下标/rank()/shape()/length | 13 |
| switch_tour.n | switch/enum分支 | 0 |
| enum_methods.n | enum方法与this | 0 |
| delegates_tour.n | 函数/方法代理 | 7 |
| aliases_tour.n | 类型别名 | 0 |
| stdlib_string.n | string方法 | 0 |
| stdlib_math.n | 标准库math包 | 0 |
| stdlib_io.n | io控制台与文件内容（需stdin） | 0 |
| stdlib_fs.n | fs路径/目录/元数据 | 0 |
| tensor_tour.n | tensor包：跨元素类型的创建、逐元素与归约核 | 0 |
| url_tour.n | url包：解析/查看/resolve/查询参数/百分号转义 | 0 |
| http_hello.n | http包：同一进程里的hello服务与其客户端（缓冲GET、echo POST、持久连接复用） | 0 |
| http_conc_bench.n | http包：1000个虚拟线程客户端共用一个客户端，每主机至多4条连接 | 0 |
| http_sse.n | http包：分块流上的服务器推送事件（SSE）、客户端提前断开、生产者取消 | 0 |
| net_echo_server.n | TCP echo服务器：accept循环与每连接一个虚拟线程 | 0 |
| net_echo_client.n | TCP客户端：connect、文本往返、readLine分帧、优雅关闭 | 0 |
| net_http_mini.n | 建立在裸`net`之上的迷你HTTP服务器与客户端：请求行、Content-Length、半关闭 | 0 |
| net_udp_echo.n | UDP请求/应答：sendTo/recvFrom、数据报语义 | 0 |
| net_ping.n | ICMP ping：往返时延、超时异常处理 | 0 |
| net_conc_bench.n | 1000个虚拟线程echo客户端与吞吐报告 | 0 |
| conc_spawn_join.n | 并发：派生任务并用`join`等待 | 13 |
| conc_future.n | 并发：`Future`返回值 | 14 |
| conc_channel_pipeline.n | 并发：通道生产者-消费者与`close` | 15 |
| conc_mutex_counter.n | 并发：`Mutex`保护的共享计数器 | 16 |
| conc_semaphore.n | 并发：`Semaphore`并发上限 | 17 |
| conc_waitgroup.n | 并发：`WaitGroup`覆盖一批任务 | 18 |
| conc_channel_close.n | 并发：`close`广播退出信号 | 19 |
| conc_carriers_demo.n | 并发：载体数不改变结果（试`nvm --carriers=1`与`--carriers=8`） | 20 |
| conc_shared_list.n | 并发：两个任务追加同一个`List`，无锁与`Mutex`串行两种写法 | 21 |
| conc_selectany.n | 并发：`Promise`/`Future.selectAny`多路等待与通道汇聚的替代写法 | 22 |
| hello_project/ | 多文件项目（nide打开`.nproj`） | 0 |
| libdemo/ | 平面搜索路径库（先编库单元，消费方import后运行期装载） | 0 |
| mnist_mlp/ | 两层MNIST感知机——批量矩阵形态的视图、广播与沿轴归约（多模块项目；数据在`data/`，验收门槛0.85） | 0 |
| embed_host/ | 最小C++嵌入宿主（CMake构建，运行`embed_host <产物.ncu|.npkg>`） | 0 |
