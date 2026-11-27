# 包产物的编译期消费与标准库包拆分（F1）概要设计

状态：五稿（2026-11-24四轮评审吸收：第一轮3阻断+4Medium+4Low+2Info、第二轮1阻断+3Medium+3Low+2Info、第三轮复审确认前两轮吸收全部根治后暴露2Medium+1Low+1Info亦已处置、第四轮针对性复核确认N1-N4根治、终局判定「适合进入实施计划」，残留5项Low/Info随本稿吸收，见文末登记）。0.13.12规划内随版交付。调研对照Java（jar/classpath/javac同规则、jmod含native）、C#（nupkg/ref assemblies）、Python（wheel/.pyd）、Go与Rust（反面参照：源码强制/ABI不稳定），报告temp/artifact_import/research_mainstream.md（本地）。用户裁定2026-11-24：按自主开发流程推进，版本归0.13.12不单列；「一个pkg类似Java的一个jar包，.ncu类似.class」；std库按领域拆分命名；不需要向下兼容。

## 一、问题

1. **手册自相矛盾**：developing-libraries.md§4宣称预编译包（.npkg）可分发点分子目录库、消费者指向包目录即可；specification/03-program-structure/packages.md§3.9.2说点分路径无源码即编译期错误。运行期`NcuLoader::ResolveTarget`已按成员表逐包扫描（点分产物**运行期可用**）；编译期`LoadModuleArtifact`只认入口记录或唯一成员，多成员npkg直接拒收，且`FindModuleFile`只按文件名查`<dir>/<name>.ncu|.npkg`——点分子模块（tp.sub藏于tp包内）编译期找不到。文件名匹配还有更深一层的陷阱：现状按文件名命中包后取**入口记录**的成员做桩，入口成员与导入名无关——编译期绑定了入口、运行期按导入名找成员，两规则不一致时**编译通过、运行期才报模块不存在**。文件名臂退役后这个缺口一并消失（导入名两端同按成员表解析）。
2. **传递闭包缺失**：源码臂有`DiscoverLibraryUnits`不动点（库依赖库迭代到收敛）；产物臂现状只加载点名的那个单元。桩签名合成的限定类型引用需要被引用单元的桩已在注册表（`tp.sub`的签名引用`tp.core.Type`而`tp.core`未加载→签名解析失败）；手册§3.9.3承诺外部产物循环编译期错误，而循环图只含**已加载**镜像的边——不加载传递依赖时跨镜像循环检测不到，承诺落空。
3. **通配对产物包失效**：D-29规则2的通配并集只算编译入模块（`ApplyWildcardImport`按`!entry.isExternal`过滤），发现侧`DiscoverLibrarySubtree`只walk磁盘源码。artifact-only包（有.npkg无源码）写`import tp.*;`→并集空→硬错「No modules matched import 'tp.*'」。D-28已裁定`import web.*;`为点分多模块库包的规范拼写——0.13.12交付的web包以产物形态被第三方消费时该拼写必须工作。
4. **stdlib.npkg包名误导**：一个包吞下全部领域单元（io/math/fs/thread/…），文件名与身份语义脱节。Java的jar文件名不影响包身份、.NET程序集名与命名空间解耦——主流做法都是「身份来自包内容，文件名只是分发载体」。语言基础与并发等领域应各自成包。
5. **native库无法进包**：包成员只有.ncu一种；nlang_net.dll等平铺在exe旁，第三方带native调用的库无法以单文件分发。jmod/nupkg/wheel三个先例都把native放进容器。

## 二、术语与模型

- **包（package container）**：.npkg文件，自描述容器（格式1.0：header含包名与格式版本＋成员表＋嵌入镜像数据，非zip）。
- **单元（unit）**：一个模块的编译产物.ncu（线格式承载签名与字节码）。
- **导入名**：import语句的点分路径（`tp.sub`）。
- **成员表**：包内（成员路径, 偏移, 长度, 校验和）条目集，按路径排序——包的可导入面的**真源**；.npkg文件名对解析不敏感（容器头的包名字段是分发元数据，同样不参与导入名匹配）。

用户类比的对应关系：pkg↔jar、.ncu↔.class、成员表↔jar清单（MANIFEST）、符号索引（编译期签名面）↔.class内签名/引用程序集。

导入诊断的两族分工（D3与D6分别动其一）：**导入语句族**（`ModuleNotFoundText`："Module 'X' not found. Check the project Sources list or -I import path."）回答「导入的模块不存在」；**使用点族**（D-29折叠："Module 'X' is not imported. Add 'import X;' …"）回答「这个名字是模块路径但本TU没有导入它」。解析与加载归D3，折叠可知性归D6。

## 三、设计裁决

**D1 成员表是真源，两端同规则，跨包重复点名**：导入名的解析，运行期既有`ResolveTarget`顺序＝入口包成员表→`<dir>/<导入名>.ncu`→逐个扫描搜索目录下的.npkg成员表（文件名序）；编译期镜像后两臂（编译没有入口包——主模块尚未产物化）：`<dir>/<导入名>.ncu`→逐个扫描.npkg成员表。不在编译期引入第二套文件名规则（javac与JVM共用classpath规则的同构）。`.npkg`文件名不参与任何解析判定；单段导入名同样按成员表匹配（`import tp;`要求成员表存在`tp`单元，不再依赖包恰好以tp.npkg为文件名）。**跨包重复成员诊断**：同一导入名在两个包的成员表都命中→编译期错误点名两个包文件（`Duplicate package 'tp.sub': 'dir/old.npkg' and 'dir/tp.npkg'.`）——遗留stdlib.npkg与新领域包并存时（拆包迁移期最现实的混包形态）开发者场景在编译期即报。运行期（nvm直接执行、未经ncc）维持文件名序取先：纯运行期消费的混包属部署错误，由部署删除治理（§六双保险），不在运行期为重复检测付出每次导入全包扫描的代价。加载序见D2；空结果与不可读包的报告见D3。

**D2 源码优先、产物补缺（加载序）**：点分导入名的**加载**顺序为源码臂（`FindLibrarySourceMatches`，既有内联语义）→产物臂（D1扫描）。理由三条（各自独立成立）：
1. **双形态并存的确定性**：标准库目录同时存在源码（可编辑副本）与产物（打包副本），且源码与产物可能分布在不同搜索目录——产物优先等于「先目录的陈旧产物遮蔽后目录的新源码」，结果取决于目录序。源码优先把序规则定为两级：**形态级先于目录级**（有源码用源码、无源码才落产物，产物内部才按目录序），跨目录遮蔽消失，形态选择确定。
2. **开发工作流**：编辑stdlib源码副本→重建的迭代里，产物优先会让陈旧产物遮蔽正在编辑的源码，破坏「改了源码立即生效」的反馈环。
3. **诊断精度**：源码臂的成员诊断带源位置（文件:行），产物臂的桩没有源位置——诊断质量面源码臂严格更优。
（第一稿「源码内联保留全量优化」的理由撤回：源码内联与产物消费在编译期都只供签名解析，函数体都不进主TU产物；两形态的优化差异只存在于发布方的编译时点，不是消费者可感知的排序依据。）
Java的classpath产物优先与NLang的源码优先不冲突：javac的`-sourcepath`语义本就是补缺（产物优先、源码补缺），其成立前提是Java生态无「同目录双形态」惯例且源路径是独立参数；NLang的搜索路径是单一路径序列承载两形态，需要固定序，选了源码优先。**诊断折叠的可知性面与此分立**（D6：源码臂与产物臂都进折叠谓词——诊断问「这名字是不是模块路径」，加载问「用哪种形态满足它」，两问不同答）。两臂并存给第三方自选形态：发源码目录或发.npkg。

**D3 编译期点分产物消费＋桩闭包不动点**：`LoadModuleArtifact`按导入名在成员表定位`.ncu`成员读取（多成员拒绝文案退役；单成员包在新规则下自然覆盖；**程序包的入口记录形态不属可导入面**——入口单元（`main`）与导入名无关，成员表按导入名精确匹配本就miss，入口包=运行期入口（nvm直接执行），编译期导入面=成员表精确匹配的库形态，按包名导入程序包的路径随文件名臂退役一并消失——这是行为收敛而非缺口，该路径在两规则不一致时本就「编译过运行败」）。phase6_loader_design.md §9末的「后续决策项」（点分外部路径的导入门语义）随之了结。签名面读.ncu线格式（ModuleLoader既有管线），不依赖磁盘源码。四个配套裁定：

1. **管线相位裁定（发现/门/装载三阶段）**：产物臂镜像`DiscoverLibraryUnits`的位置拆分——
   - **发现阶段**（DiscoverLibraryUnits同一相位，RegisterUnits/MergeTransUnits/BuildGate之前）：逐TU导入spec，对源码臂（FindLibrarySourceMatches）未命中的名做D1产物臂扫描（`.ncu`直文件→成员表——两子臂都进发现相位，裸`.ncu`单段导入的现存主路径不回归）；命中→读取镜像（CompiledModule）→**存入待装载集（导入名，镜像）对**→读其导入记录（五张槽表的modulePath）→不动点迭代：新引用名按D2序解析，**源码臂命中时内联在发现相位完成（此时内联合法——RegisterUnits未跑，与现状库TU同一并入路径），产物臂命中则继续读镜像入集**。通配spec的成员表精确∪前缀成员全部进发现集（D7.1）。此相位也承载不可读包登记（见4）。external条目**不在发现期物化**：RegisterUnits的首动作清空注册表（reg.Reset()），且模块索引空间要求TU条目先于external条目占位——发现期只定型（名与镜像入集），`AddExternalModule`物化钉在**RegisterUnits之后、门之前**的衔接点。
   - **门阶段**（BuildImportGates，现状位置与形态不变）：external条目已在注册表（发现期定型、RegisterUnits后物化）——`ApplyNonWildcardImport`的dotted名分支新增查external条目：命中→gate.exact开放；单段名与dotted名统一为「查条目开门」（条目在=发现期产物臂命中过，门不再是乐观猜测；现状单段名的乐观开门与装载候选收集（externalOut）随之退役——待装载集发现期定型，门期不做装载动作，未命中名在门期直接走既有not found臂而非装载期才失败，错误时机前移且文案不变）。`isLibraryNamespace`臂（"indexed but its source was not found"）检查面同步扩为两种形态：索引知道该包而源码与产物都不可得时仍报此错，文案措辞随实施更新（"source"扩为"source or package"），语义不变（报告搜索路径配置问题而非导入名拼错）。`ApplyWildcardImport`的prefix匹配扩展到external条目（D7.2，相位对齐后生效）。
   - **装载阶段**（BuildImportGates之后的LoadExternalModule循环，现状位置）：待装载集逐个桩合成（BuildFromCompiledModule）＋stubs填充/TagOwner（external条目已在（RegisterUnits后物化），装载期填SetExternalStubs——桩碰撞检查仍要求merge先于桩，现状不变量保持）→m_loadedImports持有镜像。装载期失败=桩合成异常（损坏镜像），既有「Failed to load module」文案保留；发现期读取失败分两类：包打开失败/成员缺失走4的not-found报告，镜像解析失败（包可开、成员表命中、字节坏）按既有「Failed to load module」文案在发现期报告——包在、成员在，并入not-found族会使文案失真。
   - 循环检查在装载后（现状顺序），不动点发现的镜像边全部在图——§3.9.3承诺兑现。
2. **桩闭包不动点**：即发现阶段裁定中的迭代——效果(a)桩签名合成的限定类型引用可达（`tp.sub`的签名引用`tp.core.Type`时`tp.core`的镜像已读取、桩在装载期合成）；效果(b)循环图含全部镜像边（`test_import_cycles`族扩产物形态用例）。**失败模式**：不动点中解析失败的引用名（包缺成员、源码与产物都无）与直接导入同走not-found报告（不静默跳过——静默跳过会让签名级联报错且循环图缺边）；**发射相位**：直接导入名走门期not found臂（现状路径），传递引用名（无任何TU的import spec点名、仅镜像导入记录引用——门的spec遍历看不见）由**发现相位直接发射**，同`ModuleNotFoundText`文案＋不可读包附注；不可读包清单附注见4。**表口径对齐（预存在不对称，随本批修）**：编译侧闭包收集五张槽表（`ImageImportTargets`含staticSlotImports），运行期`NcuLoader`闭包walk只收四张（缺staticSlotImports）——运行期补齐第五表，两端同口径（依赖「静态槽引用恒伴随classImports」的未成文不变量不再需要）。
3. **点分external的导入门语义**：`gate.exact`记录**成员模块路径**（`import tp.sub;`→gate.exact含`tp.sub`），与源码臂V1第四支（属主模块在显式导入列表→非限定名可见）同构——external桩owner的模块路径就是成员路径，`import tp.sub;`＋非限定调用`x()`与源码形态行为一致（单段external的V1统一语义在点分段延续，不再有形态差；测试矩阵既有`bareCrossModuleRejected`钉单段正例，点分段同规则扩验）。门期实现=上述门阶段臂（ApplyNonWildcardImport查external条目，const门期方法只查不开载——装载系于待装载集，不是门期动作）。
4. **不可读包并入not-found报告**：发现阶段成员表扫描中Open失败的.npkg不中止扫描（其余包继续），登记进不可读清单；导入名最终无着落时（门期not found臂与发现相位发射臂）报告附带（`Module 'tp.sub' not found. … Note: package 'dir/x.npkg' could not be opened.`）——镜像运行期`NoteTargetNotFound`的不可读包清单，两端口径一致。

**D4 标准库按领域拆包**：stdlib.npkg拆为领域包，一包一领域（用户裁定语义「语言基础=lang、并发=concurrent」）：

| 包 | 单元 |
|---|---|
| concurrent.npkg | thread、concurrent |
| io.npkg | io |
| math.npkg | math |
| fs.npkg | fs |
| tensor.npkg | tensor |
| net.npkg | net |
| http.npkg | http |
| url.npkg | url |
| system.npkg | system |
| gzip.npkg | gzip |
| regex.npkg | regex |
| json.npkg | json |
| web.npkg | web、web.static、web.sse、web.form（随0.13.12 web收口） |

lang与containers**不产物化**（现状维持）：二者是prelude，编译期嵌入、每个TU隐式可用，导入槽不存在——包成员运行期与编译期都无消费者，产物化只是死重。「语言基础」的包语义由源码目录与嵌入副本承载，无需lang.npkg。拆分对解析零影响（D1：成员表真源，文件名不敏感）。**发现面现状**：运行期`NcuLoader`本就按扩展名扫描搜索目录内全部.npkg（src/vm/NcuLoader.cpp，目录扫描已存在）；编译期stdlib目录经`FindStdLibDir`上行定位后进入搜索路径，按导入名解析（D1后与第三方包同规则，无stdlib特殊路径）。因此「stdlib.npkg文件名退役」的编辑面不在源码——src/与include/零代码字面量（src内仅两处注释：src/tools/nvm/main.cpp、src/tools/nlang/ScriptRunner.h），实际编辑面＝根部CMakeLists打包链（nproj生成/目标/产物名/install规则，7处）＋tests/CMakeLists.txt一处文件级实参＋工具脚本路径拼接与字面量断言（gate_freshness.py与gate_freshness_test.py、run_e2e_tests.py、run_embed_host.py、test_runner_timing_gate.py、debug_assert_smoke.sh、test_nculoader.cpp×3——其中gate_freshness_test.py对findings文本的`stdlib.npkg`字面量断言必改）＋手册10页×2语言＋README×2。`nlang_stdlib_package`目标名与`NLANG_STDLIB_*`/`STDLIB_DIR`/`nlang_stdlib_test`符号族是配套改名决策点（约90处引用，多为机械替换；目录级符号STDLIB_DIR语义不变可保留）。CMake的stdlib打包目标按领域拆分。语言核心（lang）与容器（containers）不进任何包、thread与concurrent合包：领域内聚高于单元数。

**D5 包内native成员（格式能力）**：成员表增加「成员种类」字段（单元|native库），容器格式版本1.0→1.1（.ncu线格式与ABI不动——这是容器层字段）。三点契约：
1. **成员表键**：native成员以**文件名**为键（`nlang_net.dll`）——单元成员以模块路径为键（命名单元），native库不是命名单元、无名可点，文件名即其身份。D1的导入名匹配只对单元成员做（native成员不参与导入名解析——`import nlang_net.dll;`不是语言面）。
2. **运行期集成（编译期零接触）**：包打开时（`NcuLoader`闭包发现见到native成员）抽取到该包的缓存目录，并把该缓存目录加入`NativeLibraryLoader`的搜索路径（`AddSearchDir`，与exe目录上行并列）。`EnsureLoaded(ns)`按既有`ModuleFileName(ns)`语义定位——抽取文件保留原名（`nlang_<ns>.dll`），自然命中，**首次调用懒加载维持**（phase6裁定不变）。编译期ncc对native成员只在成员表里可见（知道包带native），不加载——native绑定按名派发（`RegisterNative`），签名面没有native概念。
3. **缓存与校验**：缓存根目录裁定为`%LOCALAPPDATA%\nlang\pkg-cache`（用户级、可写——exe旁安装目录可能只读，`%TEMP%`会话级会随多包/多进程孤儿膨胀）；并发进程对同包的并发抽取以「临时文件写入后原子改名」落地（他进程至多读到旧完整文件或新完整文件，永不读半写DLL——`EnsureLoaded`直通`OpenShared`，无部分写防护的现状由原子写补齐）；缓存目录键=包文件绝对路径哈希（同名包在不同搜索目录不互串），抽取落地前核对成员校验和，落地文件与成员表校验和不符→覆盖重抽；陈旧缓存目录无害（按需重抽），卸载不自动清理、目录可整删重建（孤儿治理=用户级可删，不引入清理机制）。
stdlib自身的DLL迁移递延：native平铺部署的部署闭包（ASan DLL复制、IDE部署、NSIS组件）改动面大而收益仅为少几个平铺文件，第三方库的native分发才是本裁决的真实需求；stdlib迁移随Linux部署形态定案一并评估（独立理由，非递延问题）。**递延锚**：本设计落地时在roadmap v0.13.12条目登记F1（含本递延项），递延有明确归处。
native先例的形态修正：nupkg与wheel都在**安装时**展开native（它们的生态有安装步）；NLang没有安装步——drop-in模型（包即部署，复制到搜索目录即完成），运行期抽取缓存由此模型辩护，陈旧性由上条校验和守卫。jmod含native但运行时同样从文件系统读取——「容器内携带、文件系统消费」是三个先例的共同底座，NLang与其一致。

**D6 诊断折叠的产物臂（第四臂）**：D-29可知性谓词`KnowsLibraryModule`加产物臂——`HasLibraryArtifact(name)`＝导入名在某个搜索目录的.npkg成员表（单元成员）中作为模块路径存在（记忆化，逐前缀一次，镜像`HasLibrarySource`的记忆化**形态**；**记忆宿主钉在导入管线翻译单元**——与探针定义同TU，不是`HasLibrarySource`的BuildEnvironment宿主，层裁定使然；D3发现阶段的成员表扫描共用此缓存——发现与折叠两个消费者一份查询设施，不可读包登记同源）。不加产物臂则0.13.12刚清偿的Low 2缺口在产物形态重现：无源码的第三方包，消费者写`tp.sub.Fn.f()`而未导入，三臂全败、诊断按整链点名`import tp.sub.Fn;`——主建议在`Fn`是类时不可用，违反D-29规则3（每条补救建议必须可用）。产物臂与源码臂同构对称（磁盘声明了该模块路径即可知），javac同类先例（classpath上的包名参与「package does not exist」判定先于符号解析）。实现面层边界：探针定义住导入管线翻译单元（其消费者全链vm库），声明走`ModuleBuilderImports.h`缝头；BuildEnvironment不触碰NcuPackageReader（环境层消费者如langservice测试保持只链环境层可链接——0.13.12 Low 2修复的层倒置教训）。已知前缀上的类型锚语义与源码臂一致：产物未加载时类型探针查不到声明，导入后走registry第一臂。折叠谓词的源码臂判定维持刚交付的三臂不动（`HasLibrarySource`按点分名查源文件存在性）；产物臂是新增第四臂，不回改源码臂语义。

**D7 通配并集的产物臂**：并集＝源码子树∪产物成员表子树，三处贯通（相位依D3裁定）：
1. **发现侧**（D3发现阶段）：通配spec的产物臂＝扫描搜索目录全部.npkg成员表，**精确成员（导入名自身，若存在）∪以导入名为前缀的单元成员**进入发现集→待装载集（与`DiscoverLibrarySubtree`的磁盘walk同构——「the package file itself＋子树」↔「成员表精确命中＋前缀命中」；native成员不进通配集，D5）。发现集是D3不动点的起点扩集：前缀成员全部点名后，不动点再补传递依赖。
2. **gate侧**（D3门阶段）：external条目已在注册表（发现期定型、RegisterUnits后物化、path=成员模块路径）——`ApplyWildcardImport`的prefix匹配扩展到external条目（前缀`tp.`命中`tp.sub`条目）后自然生效；exact与prefix的判定面同步扩展；空并集诊断文案不变（其「Check the project Sources list or -I import path」检查面已涵盖产物目录）。
3. **循环检查侧**（D3装载后）：`ExpandWildcard`用全部注册路径（含external条目）——发现集全部装载后该侧零改动（确认性陈述，非新逻辑）。
通配仍不授非限定名（D5防倾倒不变）：external桩owner虽在注册表，`IsBareVisible`只认gate.exact不认gate.wildcards——产物形态与源码形态在此完全一致。

## 四、落地顺序（依赖序）

1. **拆分改名**（D4）：CMake打包目标拆分＋全消费面更名（上述勘察清单：根部CMakeLists打包链7处、tests/CMakeLists.txt文件级实参、六个工具脚本的路径拼接与字面量断言、手册10页×2语言、README×2、roadmap、tools/http_perf/probe_pair_ab.sh:51按名构建目标、tests/test_embed/test_embed_api.cpp的NLANG_TEST_STDLIB_DIR五处——src/与include/零代码改动）。运行期与编译期解析零改动（运行期目录扫描已是现状）。先行独立可停靠。
2. **编译期点分产物消费**（D2/D3/D6/D7）：发现面收点分名＋多成员包按导入名加载＋桩闭包不动点＋通配并集产物臂＋折叠第四臂＋e2e语料（预编译包点分子模块编译并运行）。与步骤1不依赖（多成员消费与拆分正交），同版交付、验收语料合流。
3. **包内native成员**（D5）：容器格式扩展＋抽取缓存＋校验和键＋加载器搜索目录注入。纯增量（新字段新路径），前两步的回归护栏（既有包在新读取器下全绿）。

**与web线的顺序协调**：F1步骤1的CMake拆分与web线Tasks 8-10（web包成员扩展为web/web.static/web.sse/web.form）是同版同CMake机械动作——web线先定成员清单（其源码随web收口入库），F1拆分以该清单一次到位，避免两线各自改打包目标互相踩踏；web包成员在F1视角只是既有nproj成员列表的既有成员，无需特殊处理。

## 五、验收面

- e2e：预编译.npkg包（多单元、点分子模块）的编译+运行；**传递闭包**（只`import tp.sub;`，`tp.core`桩自动加载、跨单元签名解析通过）；**传递依赖not-found负例**（包缺`tp.core`成员→编译期报`Module 'tp.core' not found`，不静默跳过、无签名级联）；**外部产物循环编译期拒绝**（两包互导→编译期报环，对齐§3.9.3承诺）；**通配产物**（artifact-only包`import tp.*;`非空并集、限定调用可达）；**D6产物形态折叠**（无import时`tp.sub.Fn.f()`折叠到`tp.sub`，主建议`import tp.sub;`（备选`import tp.*;`）可用——折叠取最长已知前缀，产物臂使`tp.sub`即已知）；**点分V1延续**（`import tp.sub;`＋非限定调用其成员函数，与源码形态一致）；**单段文件名不敏感**（`import tp;`命中成员表带`tp`单元而文件名非tp.npkg的包）；**D2双形态序**（同库陈旧产物＋新源码并存→源码赢、产物不遮蔽）；源码/产物双形态同库对拍（诊断与行为一致）；第3步落地后同包native成员的加载运行（**不加载维持**：导入该包但无native调用的运行DLL不进进程——抽取随包打开发生（D5.2，闭包walk必开包），懒的是`EnsureLoaded`的装载而非抽取；**校验和重抽**：篡改缓存文件→重抽后行为正常）。
- 单测：NcuPackageReader格式1.1成员种类往返与旧读取器拒绝（容器版本守卫）；裸`.ncu`单段导入回归（现存external主路径——D3.1发现相位两子臂都不回归）；ApplyWildcardImport的external前缀匹配（BuildGate时条目已在的相位形态）；发现期不动点（传递依赖加载与环检测）；HasLibraryArtifact折叠谓词（产物臂不污染源码臂）；跨包重复成员诊断；不可读包报告；运行期`DiscoverClosure`补齐staticSlotImports第五表后的两端同口径。
- 手册（真源docs/user_manual/{zh,en}，docs/site为构建期生成站）：packages.md§3.9.2改写（点分路径＝源码或产物任一可得即可导入，消除与developing-libraries.md的矛盾）、§3.9.4标准库包清单按领域重列；点名stdlib.npkg的10页逐页核对（getting-started/install-layout、cli-tools/ncc、cli-tools/nvm、cli-tools/ndb、libraries/integrating-nlang、libraries/embedding-api、vm-architecture/module-serialization、vm-architecture/library-mechanism、specification/03-program-structure/packages、specification/formats/28-npkg——zh/en成对）；28-npkg.md容器格式1.1成员种类字段。**手册「包」语义随拆包统一**：包＝容器、单元＝模块——现状页把单元称「包」（如「十四个标准包」实为十四个单元）的措辞按新语义重列（拆包后13个领域包）；**现状已不一致的包清单页随本批对齐**（module-serialization.md列3成员、ncc.md列5成员、实际14单元——三处口径统一到D4清单，与web收口后的成员数按交付时点核准）。中英成对。
- dev-doc同步：phase6_loader_design.md§9末「后续决策项」了结登记（本设计D3）；name_resolution_design.md D-29可知性三臂→四臂带日期修订（本设计D6）。
- 工程面：VERSION次版本+1、CHANGELOG记录；roadmap v0.13.12条目登记F1（含D5递延锚）；影响映射核对（ExprResolver与导入管线属宽面文件，T0保守回退Debug全量单测）；部署闭包与IDE树门禁覆盖（拆包后部署面变更必须IDE树全量ctest）。

## 六、风险与边界

- 多成员npkg编译期按需扫描的成本：导入名命中才读包（逐包读成员表是小IO）；不预扫全部包（Java lazy解析同理）。通配是例外形态（必须全包扫成员表前缀），编译期一次、量级为包数×成员表尺寸。
- 容器格式版本守卫：旧读取器遇1.1包拒绝报错（不向下兼容原则，报错文案具名版本）。
- 产物与源码同目录并存（用户把源码目录和.npkg一起发布）：D2顺序保证确定性（源码赢），诊断面不出现「两形态打架」。
- Windows大小写不敏感文件系统的边角（D-29已登记的同类）：产物臂的成员表匹配是字节精确的（成员表内嵌名字），不吃文件系统语义——比源码臂更干净。
- 混包迁移期：拆包后遗留旧stdlib.npkg与新领域包并存——D1跨包重复成员诊断在编译期点名，部署面（NSIS/本地部署脚本）同时删除旧包文件（双保险：诊断兜住未删净的场景）。

## 附：评审吸收登记（2026-11-24，两轮）

| 评审发现 | 级别 | 处置 | 落位 |
|---|---|---|---|
| 1 产物臂传递闭包未裁定（桩签名引用、循环承诺落空） | Critical | 采纳：桩闭包不动点（镜像DiscoverLibraryUnits）＋循环图不动点后完整 | D3.1 |
| 2 手册§3.9.3循环承诺与「循环图只见已加载边」矛盾 | Critical | 采纳：同上，验收面加产物环e2e | D3.1、§五 |
| 3 通配对artifact-only包空并集（D-28规范拼写失效） | Medium按阻断 | 采纳：并集三处贯通（发现/gate/循环） | D7 |
| 4 D2「源码内联保留全量优化」理由假前提 | Medium | 采纳：撤回假理由，重述三条独立理由（双形态确定性、dev工作流、诊断精度） | D2 |
| 5 D5三点未定（成员键/加载集成/缓存键） | Medium | 采纳：三点契约（文件名键、缓存目录进搜索路径＋懒加载维持、路径哈希＋成员校验和） | D5 |
| 6 文档/验收面低估（手册9页、28-npkg格式章、dev-doc、D6/通配/传递e2e） | Medium | 采纳：验收面按项补齐；页级清单勘察后并入§五（实为10页，含「包」语义统一与现状不一致对齐） | §五 |
| 7 更名面漏（freshness探测、影响映射键、debug_assert_smoke、部署fixture） | Medium | 采纳：勘察盘点并入§四.1（src零字面量，编辑面=CMake打包链+六个脚本+手册+README） | §四.1、D4 |
| 8 不可读包编译期静默跳过 | Low | 采纳：并入not-found报告（镜像运行期口径） | D3.3 |
| 9 陈旧混包（旧stdlib.npkg与新包并存） | Low | 采纳：跨包重复成员诊断＋部署删除双保险 | D1、§六 |
| 10 D5递延无roadmap锚 | Low | 采纳：v0.13.12条目登记F1含递延锚 | §五 |
| 11 F1步骤1与web线Tasks 8-10同CMake机械冲突 | Low | 采纳：顺序协调句 | §四 |
| 12 D3措辞混淆not found族与is not imported族 | Info | 采纳：两族分工写进术语节；顺带明说文件名臂退役杀掉「编译过运行败」入口陷阱 | §二、§一.1 |
| 13 调研两处过陈（native无运行时抽取先例、Java优先级过简） | Info | 采纳：D5先例形态修正＋D2对比精确化；调研报告同步修订 | D2、D5 |

第二轮（同日，判定「不适合进入实施计划」，9项发现）：

| 评审发现 | 级别 | 处置 | 落位 |
|---|---|---|---|
| 14 产物臂管线相位未裁定——BuildGate先于external装载、不动点源码臂只能在发现相位内联，D3.1/D3.2/D7.1/D7.2四处文字互斥 | Critical | 采纳：发现/门/装载三阶段相位裁定（发现期成员表发现＋读镜像＋**定型external条目（名与镜像入集）**＋不动点（传递引用源码臂此时内联合法）；RegisterUnits后物化条目（N1修正）；门期查条目开门（单段与点分统一，乐观开门退役）；装载期桩合成（merge后现状位置不变量保持）；循环检查在装载后） | D3.1、D3.3、D7 |
| 15 §五D6验收期望值与折叠行为相反（折叠取最长已知前缀，产物臂使tp.sub即已知） | Medium | 采纳：验收改「折叠到tp.sub、主建议import tp.sub;（备选import tp.*;）可用」 | §五 |
| 16 不动点失败模式未裁定（静默跳过=签名级联+循环缺边） | Medium | 采纳：解析失败与直接导入同走not-found报告（不静默跳过）＋传递依赖负例验收 | D3.2、§五 |
| 17 D5缓存缺物理归宿与并发/生命周期裁定 | Medium | 采纳：%LOCALAPPDATA%\nlang\pkg-cache＋临时名原子改名（防半写DLL）＋孤儿=用户级可整删 | D5.3 |
| 18 入口记录包「自然覆盖」对程序包不成立（入口单元≠导入名） | Low | 采纳：程序包入口形态裁定为非可导入面（运行期入口；行为收敛非缺口，该路径本就「编译过运行败」） | D3头 |
| 19 验收缺四条线（V1点分延续/单段文件名不敏感/双形态序/懒加载与重抽） | Low | 采纳：四条e2e全部补入 | §五 |
| 20 勘察漏两处＋页数不符（probe_pair_ab.sh按名构建目标、test_embed_api.cpp五处符号；11页实为10页） | Low | 采纳：补入§四.1；页数修正 | §四.1、D4、§五 |
| 21 D6记忆宿主与层裁定相抵（HasLibrarySource宿主在BuildEnvironment） | Info | 采纳：记忆宿主钉在导入管线翻译单元（镜像其记忆化形态而非宿主） | D6 |
| 22 五表vs四表运行期不对称（DiscoverClosure缺staticSlotImports，预存在） | Info | 采纳：运行期补齐第五表（随本批修，两端同口径） | D3.2、§五 |

第三轮（同日复审：确认第二轮9项吸收全部根治；4项新发现，2Medium+1Low+1Info）：

| 评审发现 | 级别 | 处置 | 落位 |
|---|---|---|---|
| N1 发现期条目物化与RegisterUnits注册表生命周期相抵（reg.Reset()首动作清表＋TU先external后的索引序——发现期物化的条目到门期必被清掉） | Medium | 采纳：发现期只定型（名，镜像对入待装载集），AddExternalModule物化钉在RegisterUnits之后、门之前；四处措辞统一「发现期定型、RegisterUnits后物化」 | D3.1、D7.2 |
| N2 §五native验收「不抽取」与D5.2「包打开时抽取」互斥（闭包walk必开包，抽取随开包发生，懒的是装载） | Medium | 采纳：验收改「不加载维持：导入包但无native调用的运行DLL不进进程；抽取随包打开发生」 | §五 |
| N3 传递引用名的not-found发射相位未钉（门的spec遍历只见TU导入spec，镜像导入记录点名的名只能发现相位发射） | Low | 采纳：D3.2补发射相位（直接导入走门期臂、传递引用由发现相位直接发射，同文案+附注） | D3.2 |
| N4 「src/include零字面量」路径标签不准（头文件树在include/，两处注释实在src/tools） | Info | 采纳：标签改「src/与include/零代码字面量」并点名注释真实位置 | D4、§四.1 |

第四轮（同日针对性复核：确认第三轮N1-N4全部根治；5项新发现，2Low+3Info；终局判定「适合进入实施计划」）：

| 评审发现 | 级别 | 处置 | 落位 |
|---|---|---|---|
| F1 登记表行14处置列残留「注册external条目」旧措辞，与N1「发现期定型、RegisterUnits后物化」相抵 | Low | 采纳：改为「定型external条目（名与镜像入集）」，物化另列（N1修正） | 附表行14 |
| F2 D3.1「D1成员表扫描」字面挤出`.ncu`直文件子臂（现存单段external主路径回归风险） | Low | 采纳：改「D1产物臂扫描（`.ncu`直文件→成员表）」并明说两子臂都进发现相位；§五补裸`.ncu`单段导入回归用例 | D3.1、§五 |
| F3 状态行两轮Low计数互换（第一轮实为4Low、第二轮实为3Low） | Info | 采纳：计数修正并升五稿 | 状态行 |
| F4 D3.4括注「门期not found臂」未随N3发射相位裁定扩展 | Info | 采纳：括注改「门期not found臂与发现相位发射臂」 | D3.4 |
| F5 发现期镜像解析失败（成员在而字节坏）未分类，恐并入not-found族致文案失真 | Info | 采纳：按既有「Failed to load module」文案在发现期报告，不并入not-found族 | D3.1装载阶段失败分类 |
