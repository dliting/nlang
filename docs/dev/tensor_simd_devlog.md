#张量SIMD算子加速——开发过程记事

技术文章素材库：实现过程中的内核bug、数值精度解题、SIMD陷阱，按遇到顺序随手记，发布前再整理成文。主线进度优先，每条只记"现象→根因→修法→可复用结论"。

##1. 向量expf：朴素归约的误差天花板与Cody-Waite拆减（2026-11-11）

- 现象：AVX2/SSE2多项式exp对libm的最大相对误差~1e-6（约8 ulp），远超4 ulp契约；测试显示误差峰值出现在|x|≈45处而非多项式窗口边界。
- 根因：`r = x*log2e`在f32中计算，|r|可达128，一次舍入的绝对误差~0.5ulp(128)≈7.6e-6，经`exp`的链式法则放大为ln2·Δr≈1e-6级相对误差——**多项式再准也白搭**，误差主导项在归约步。
- 修法：Cody-Waite拆减。n=round(x·log2e)仍用f32（只用于定n），关键值f=(x−n·c1)−n·c2直接在自然对数域计算，c1取ln2的12位尾数前缀（0.693115234375，n·c1在n≤255时f32精确），c2=ln2−c1补足低位。修正后实测最大误差0.65 ulp——比契约好6倍。
- 可复用结论：向量超越函数的精度瓶颈通常在参数归约不在多项式；误差预算要先算归约舍入（Δexp≈Δf），多项式阶数按剩余预算选。

##2. tanh的exp公式在小|x|处病态（2026-11-11）

- 现象：tanh用恒等式1−2/(1+e^{2x})实现，|x|≈0.05处相对误差~5e-6，且随|x|→0线性恶化。
- 根因：exp的绝对误差ε直接进入最终减法（1−大数相减），相对误差被放大ε/|tanh(x)|≈ε/|x|倍——**尾端相消公式的老问题**。
- 修法：|x|<0.5切奇次Taylor（Horner至x¹³项，截断<5e-8），否则exp式（该处tanh≥0.46，放大可控）。两分支在缝口处与真值差均<1e-6，无可见折点。
- 可复用结论：凡"1−f(x)"形态的公式，检查f(x)→1时的误差放大；小参数区换Taylor/有理式是标准解。

##3. SIMD abs掩码写成FLT_MAX：把指数砍了一位（2026-11-11）

- 现象：tanh双分支在x≈±1.06处选错分支（走了Taylor分支），误差0.5%。
- 根因：求|x|用`and(x, FLT_MAX)`——FLT_MAX位模式是0x7F7FFFFF而非0x7FFFFFFF，**把指数域最低位也清零了**：凡指数为奇数的值，绝对值被砍半（1.06→0.53<0.625门槛）。这类错误不报错、只在特定值域表现，测试值域覆盖不足时极难发现。
- 修法：abs掩码用位常量`_mm_set1_epi32(0x7FFFFFFF)`（或`andnot(sign_mask)`）。
- 可复用结论：SIMD位技巧的常量必须写位模式+注释位域，不要"看起来等价"的浮点常量；分支选择的值域测试要覆盖指数奇偶两侧。

##4. Horner链改系数时静默丢项（2026-11-11）

- 现象：tanh Taylor升阶（Q5→Q6）后误差反而变大且两SIMD档同错~4e-6。
- 根因：把初始化行`q=Q5`改成`q=Q6`时，链首的`q=q·t+Q4`没动——**Q5项被静默跳过**，多项式次数与系数集不再对应。编辑式修改对系数链是高危操作。
- 修法：补回+Q5行；用探针程序（直接调内核、打印中间值）定位——先验证"数学对、代码错"再读内核。
- 可复用结论：系数链要么整体重写、要么用python按同一次序数值仿真对照（本次sim与内核输出的分叉直接指认了代码层问题）；"数学正确但实现错"优先怀疑链结构不匹配。

##5. 2^n位装配的n=128悬崖：溢出边缘的有限值（2026-11-11）

- 现象：exp在x=88.72（ln(FLT_MAX)附近但未溢出）返回inf，而libm返回有限大数。
- 根因：2^n用`(n+127)<<23`装配，n=128时得指数255、尾数0=+inf；但2^128·2^f（f<0）的真实乘积可表示——**位装配法在边界处系统性地把可表示值变inf**。
- 修法：装配时n钳到127（位模式永不自爆），n==128的情况用一个2.0×比例因子恢复（`f32`内2·2^127=2^128精确无舍入）。
- 可复用结论：位装配向量法的边界分析要和libm逐点对拍"溢出边缘带"；库函数契约（哪些点必须有限）要写成测试。

##6. 测试侧：溢出带参考值的映射与浮点打印（2026-11-11）

- 域采样测试的参考值`std::exp(double)`在真实结果超float上限时不是inf，若直接算相对误差会误报——参考值需按`ref>FLT_MAX→期望inf`映射。
- 失败信息里`std::to_string(float)`默认6位十进制，会掩盖关键差异；定位时用探针+`%.9g`。

##待记录（占位）

- （M2）matmul微内核分块与小矩阵预算的实测归因
- （M3）广播/strided驱动的连续快速路径判定
- （M5）编译器脱糖import gate的绕行方案

##7. matmul测试度量的两个教训（2026-11-11）

- 累加契约：向量内核（清零累加器后存储）与标量参考（空k循环不写）在inner=0时行为表面不同，但真实驱动语义是"累加进调用方零化的新数组"——契约应锚定在驱动的真实存储约定上，而不是任一实现的副作用。契约写进后端表注释，测试按契约断言。
- 纯相对容差在相消点假警：随机[-1,1]点积K=256时，结果可相消到|w|~1e-4，f32累加噪声1e-5级→相对误差1e-2级假警。度量改成numpy allclose式|g−w|≤atol+rtol·|w|（atol按K·eps·|term|预算定）。f64同结构内核全绿是"结构无误、纯属度量"的关键旁证。
- 实测：Zen3上AVX2+FMA 4x8微内核（k顺序广播+FMA，B行L2驻留、无打包）在MNIST主形状跑52.6 GFLOP/s（峰值147的36%），512立方30.9 GFLOP/s；标量档仅7.5-8.3。微基准用两点斜率法（wall(R)−wall(1)）消掉进程启动开销。

##8. 向量归约的两个"看起来对"的bug（2026-11-11）

- **extractf128(v,1)取的是高半**：把f32块拓宽为double时，对`loadu_ps(cells+i+4)`再做`extractf128(...,1)`取到的是i+8..i+11——比预期多偏4个元素且越过块尾（越界读）。正确写法：低半用`castps256_ps128`，高半才用`extractf128(...,1)`。这个写法同时踩了越界与漏算（元素4..7永远没进累加器），测试显示"差一点"而非"完全错"，极具迷惑性。定位路径：8元素已知和探针→dumpbin反汇编obj发现加载偏移+0x20（源码是+0x10）→反推取半错误。
- **unpackhi_ps(sel,sel)[0]是lane2不是lane1**：UNPCKHPS交错两操作数的高半：结果=[dst[2],src[2],dst[3],src[3]]。要在4lane向量里取lane1得用`shuffle_ps(sel,sel,_MM_SHUFFLE(1,1,1,1))`。而pd版本（2 lane）的unpackhi恰好取lane1——同构代码在f32/f64双精度下行为分叉，f64测试全绿掩盖了f32错误。教训：**同构模板的lane几何在宽度不同时不同构**，按宽度写单测，不要只信"对照版"。
- **CMake per-file arch属性必须跟源文件清单同步**：新增AVX2 TU（VecBinary/VecReduce/MatMul Avx2.cpp）时忘了加进`/arch:AVX2`属性列表——MSVC对未开arch的TU编译AVX intrinsics会生成缺少arch契约的代码，运行期表现为"偶尔对偶尔错"的诡异行为。修法：CMake注释里写明"此清单必须与SIMD tier源同步"。

##9. 工程记事

- ctest必须在构建目录跑（或`--test-dir`）：源根目录的空CTestTestfile会让人误判"测试没注册"。
- 对拍度量的三层容差设计：内核层（atol+rtol混合，预算按K·eps定）、e2e层（精确断言仅限双精确可表示的小数组）、基准层（GFLOP/s绝对预算）。

##10. 标准库表面设计的三课（2026-11-11）

- **静态类型VS numpy语义的边界**：`max(a, axis)`在numpy里rank-1时返回标量，但NLang的native声明静态返回Array<T>——同一签名无法双返回。裁决：axis臂限rank>=2，rank-1给指名诊断（"reduce with max(a), not an axis arm"）。sum家族同型洞是既有潜伏问题（master上不可达），记录不扩散。
- **重载歧义是既有什么就绕什么**：int数组的`max(a, int)`早被clamp臂占用（int bound），axis臂（int axis）与之类型全同——int/long不发axis公共overload，float/double无歧义正常发。
- **registry名与.n声明约定**：注册名是无后缀的kind分派入口（addScalar模式），.n用同名重载native声明；后缀名（fullI/negI）只用于每个kind独立内核的创建族。wrapper自指（`return rsubScalar(a, value)`在自己体内）会静默成无限递归——wrapper只能桥接不同名。

##11. gather语义定稿与重写丢参事故（2026-11-13）

- **take-per-position语义的裁决**：numpy有两个相近表面——`np.take(a, idx, axis)`（idx长度成为新轴长）与花式索引`a[arange(n), labels]`（每行取自己的标签）。MLP损失项要的是后者。裁决：`gather(a, axis, indices)`的indices长度==非轴元素数（elemCount/along），indices[flat]按行主序命名该非轴位置的轴下标，结果去掉轴维。秩1源退化成普通花式索引（去掉唯一的轴后没有可走的位置，indices直接驱动唯一轴，长度即输出长度）——数学上不均匀，但程序员直觉（numpy的`v[idx]`）优先。
- **重写丢参事故**：把VisitGather从3参改成带indices的版本时，重写稿漏掉了indices形参，回调收到的是flat本身——gather退化成恒等取对角（picked=[a[0,0],a[1,1],...]），而秩2轴0用例的期望值恰好部分吻合，最小复现"看起来差一点"而非"全错"。教训：**重写小函数比改小函数更容易丢参**——review重写稿时先对旧稿做形参清单比对；期望值部分吻合的用例要再挑一个非对称数据点（本例indices=[2,0,2]若对角化正好[0,1,2]，picked第三个值碰巧相同）。
- **负例测试的自我矛盾**：op_tensor_gather.n尾部写了"越界抛异常（未捕获exit 1）"却在期望exit 0的manifest下——同一文件正负例混装必然自我矛盾。负例一律独立成`_throw`文件，manifest按exit 1登记。
- **旧契约测试随功能演进失效**：0.7.3的"数组+数值拒绝"pin在v0.13.7数组标量算术合法化后必然失败——编译器报的不再是类型不匹配而是"未导入tensor包"。pin要随契约演进改写（同种类标量广播合法、混合种类仍拒绝），并补一条新诊断（import gate）的独立pin。

##12. oneHot视图offset事故与uniform设计（2026-11-13）

- **核函数读实参数组必须走记录的offset/strides**：oneHot内核直接`labels[i]`读`v.data`——拥有数组offset=0时全对，切片视图（`labels[first:first+rows]`，offset=first）就从属主元素0开始读。mlp改写用切片视图喂oneHot后训练静默发散（每批标签都错但都合法，无异常可抓），靠三段变体二分（uniform只换init收敛→oneHot换写收敛→切片视图发散）定位。修复走`v.offset + i*strides[0]`。教训：**内核单测用拥有数组永远测不出视图offset bug**——e2e回归钉必须含"切片视图喂核函数"用例（op_tensor_gather/op_tensor_reduce2已加）。此前gather/eq内核写了stride遍历是对的，oneHot漏了——同一文件里两套写法并存，说明"读视图"没有统一的强制入口，值得将来把StrideWalk提为唯一合法路径（编译期约定）。
- **uniform骑宿主PRNG流**：随机数组内核不own生成器，逐元素调`host->nextRandom`——与math.random同一条流、同一[0,1)映射（高24位/2^24），行主序抽取顺序与嵌套循环逐元素调用完全一致，"调用方播种则整跑可复现"的契约原样保持。e2e钉子同时钉两个方向：同种子uniform自 reproduces、同种子下uniform与math.random嵌套循环逐元素相等。float实参读低半cell（ArgFloat非ArgDouble）——写完走一遍F/D两臂各有reader的创建家族宏模式。

##13. P3四条根因：mirror掩盖层（2026-11-16）

mirror后端（malloc hooks+cpu表）验收了P2驻留模型，但它的"设备"内存就是宿主内存、它的表就是cpu表——**任何错误路由在mirror下都碰巧正确**。cuda真跑把四个被掩盖的实现缺陷全部引爆，每条都值得留档：

- **全局选表VS逐调用路由**：P2给ActiveTensorOps加了"设备优先"覆盖，P3又在staged路径里调`OpsForResidency(0)`想拿cpu表——后者委托前者，于是"宿主walk"实际跑的是cuda表（staged gather把宿主staging缓冲的指针喂给GPU内核=illegal address）。教训：**同一张表有两个获取入口（环境选择vs按驻留选择）时，"设备优先"的全局入口必然反噬显式要cpu的调用点**。修复=ActiveTensorOps回归纯cpu查表+新增CpuTensorOps直接入口，设备路由只归OpsForResidency一个函数。
- **flat内核忽略视图offset**：mnist每个batch的行切片视图（offset=first*784）经IsContiguous直通后，matmul等flat内核从`data[0]`读——**所有batch都读batch 0的行**。症状极具迷惑性：训练机械跑通、loss从2.34降到2.303（ln10）后平台、accuracy 0.184≈随机。修复=ToLaunchDense把offset折进基指针（一处修复覆盖所有dense内核）。教训：**连续≠offset为0**——切片视图连续但它不是从owner元素0开始；探针必须覆盖"同owner不同offset的切片"。
- **枚举序手写数字**：ReduceAxisOp{Sum,Max,Min,...}，内核门写成op2/op3（错位一位）——max被拒NotSupported。修复时又发现槽实现硬编码op=0（Sum）——max返回了每行的和（12而非9）。教训：**跨边界传枚举必须用枚举名传参，禁止手写数字字面量**；症状"结果看着像别的东西"（和vs最大值）时先查op编码。
- **LaunchView忘拷elemKind**：ToLaunch建几何结构时漏了一个字段，f32数组的elemKind默认0(=int32)——归约内核的门把它当int拒掉。教训：**手写结构体拷贝函数必须逐字段对照定义**，漏字段在默认值恰好合法时静默通过。

这四条与镜像掩盖的关系是系统性的：**验收后端与生产后端共享越少代码路径，掩盖越少**。mirror的长期定位是"驻留语义测试 vehicle"，性能与路由正确性必须靠真cuda的parity钉。

##14. D3a选择语义与P4收尾（2026-11-16）

- **静默落回对拼错的名字是谎话**：D3a把默认定为auto（未设置env→探测→第一个驱动成功启动的设备，缺席静默落回cpu），但显式名字是契约——拼错的`NLANG_TENSOR_BACKEND=cdua`若也静默落回，用户要的加速就无声丢了。裁决：未知名向错误流写一行（`tensor: unknown backend 'cdua' — running on cpu`）并跑cpu；显式`cuda`缺席则fail-fast（进程在程序启动前退出）。验收靠四条e2e钉：cpu断言、auto双值断言（b=="cpu"||b=="cuda"，硬件无关）、env侧车钉stderr文案、坏flag钉退出码2（CLI在模块加载前就拒绝）。
- **注册表死代码**：P3路由定稿后`ActiveTensorOps()`（env名字查表）与`RegisterTensorOps()`（名字注册）零调用方——删掉名字查找间接层，宿主路径一律走`CpuTensorOps()`直接入口。这与§13第一条是同一类病的两次发作：**同一张表有两个获取入口时，全局名字入口必然反噬显式要cpu的调用点**；mirror的表就是cpu表，所以名字查找在mirror下永远碰巧对。
- **e2e套件的硬件无关化**：套件默认钉`NLANG_TENSOR_BACKEND=cpu`（结果不依赖本机有没有GPU），`NLANG_E2E_TENSOR_BACKEND=cuda|mirror`开设备双跑腿，逐测试`.env`伴随文件（KEY=VALUE行）在钉上叠环境变量——env-only的臂（如未知名诊断）nvm CLI够不着（CLI在模块加载前就拒了），只能靠运行期环境钉。`.flags`仍优先于env。
- **staleness守卫是有意的过近似**：工具mtime必须≥最新src、npkg必须≥ncc.exe。工具不依赖tensor源时，no-op重建不刷mtime，`touch`合法（守卫文档明说的过近似）；npkg那条则必须真重建`nlang_stdlib_package`，touch会留下假新。
- **ABI改签名漏改测试**：P2给模块init加`NativeHost*`参数（ABI v13）时同步改了`NativeLibraryLoader::EnsureLoaded`的形参表，但`test_native_loader.cpp`的八处调用没补第三个实参——已提交代码在tests开启的树上编译不过。Release树配成`NLANG_BUILD_TESTS=OFF`，单测腿不存在，编译错误要到Debug树的ctest才炸出来。教训：**ABI类变更的完成定义包含测试树的全量编译**——"生产树能编"不等于"分支能编"；三树门禁的价值正在于各树测试配置不同，单树门禁看不见这种缺口。
