# 空字符串表示设计——与Go对照

## 1. 目的与适用范围

本文逐角色对照Go语言与NLang对空字符串的表示(裁定D26、D27),作为今后涉及字符串表示、null/空区分、native边界的设计决策的统一参照。范围覆盖VM内部表示、语言面、native ABI边界,以及序列化、调试器等外围面。

## 2. Go的总体模型

Go对空字符串的立场由三点构成:

1. **string是值类型,"可以为空,但不会是nil"**。Go标准库`builtin.go`的原文定义:"A string may be empty, but not nil. Values of string type are immutable."。nil只是指针、切片、map、channel、函数、interface这几类类型的零值,不是string的取值。

2. **零值机制**。声明未赋值的变量自动获得类型零值:string的零值是`""`而非nil;数组与结构体的零值是其每个元素、每个字段的零值。因此Go里不存在"未初始化的字符串",也就不需要额外的"null字符串"。社区对"为什么string没有nil"的标准回答:nil string代表什么?空字符串已经有了;未初始化的string不存在。

3. **内部表示**。string是一个(数据指针,长度)二元头;空字符串常量的数据指针为0、长度为0,不占任何存储(实现细节,非规范承诺)。相等按内容比较,与指针无关。

一句话概括:**在Go里,空字符串是普通值;"缺席"由string类型之外的手段表达(nil指针、ok惯用法、error),从不由字符串自己表达。**

## 3. 逐角色对照

NLang侧的机制细节见VM源码(`VmExecutorStrings.cpp`、`VmExecutorNativeHost.cpp`)与实施台账;本表只列表示结论。

| 角色 | Go的表示 | NLang的表示(裁定D26/D27) | 判定 |
|------|---------|--------------------------|------|
| 局部变量 | `var s string`→`""` | 帧槽零初始化→句柄0 | 一致 |
| 函数实参 | 按值传`""`头 | 调用方写arg cell 0 | 一致 |
| 默认参数 | 无默认参数,靠零值约定 | `=""`默认值经运行时intern表铸造→句柄0 | NLang扩展,同向 |
| 函数返回值 | `return ""` | resultCell=0 | 一致 |
| 结构体字段/包级变量 | 零值`""` | 记录cell/静态槽零初始化→0 | 一致 |
| 数组/切片元素 | 元素零值`""` | payload零初始化→0;view/slice复制句柄 | 一致 |
| map值 | 缺失键→`""`,comma-ok区分"无" | Dict值装箱payload 0;`contains()`区分"无" | 一致 |
| 泛型类型参数T=string | `var z T`→`""` | 擦除槽0按实参种类解读→`""` | 一致 |
| 装箱(interface) | interface本身可为nil,内部string永不为nil | Object箱idx≥1,payload 0;`o==null`区分"无" | 一致 |
| 异常消息 | `Error()`可返回`""` | message句柄0,格式化仅typeName | 一致 |
| 拼接 | `""+x==x`;`strings.Join`空片段→`""` | `cons(0,x)`/`cons(x,0)`;`""+""`→0 | 一致 |
| 截取空结果 | `s[:0]`→`""`头 | `MintNewString("")`→0 | 一致 |
| 相等比较 | 按内容,与指针无关 | 0对0走interned句柄快路径;0对新空对象走内容比较 | 一致(NLang多一条快路径) |
| 序列化 | JSON缺失字段→`""`;`""`→`""` | len+bytes(0字节),重载mint→0 | 一致 |
| 调试器显示 | `""` | `StrValCopy(0)`→`""` | 一致 |
| 闭包捕获 | 按值复制`""` | 复制句柄0 | 一致 |
| GC | 空串不分配(nil指针头) | slot 0不朽、永不sweep | 同向(都零成本) |
| native实参(Go→C) | `C.CString("")`分配新的`'\0'`缓冲,从不返回nil | `getString(0)`→有效`""`C串(非NULL指针) | 一致 |
| native结果(C→Go) | `C.GoString(nil)`静默返回`""`(`findnull`对nil返回长度0,无strlen、无panic);惯例先判nil | 句柄0→host级错误"returned null string" | 分歧(Go把NULL静默变`""`,NLang按契约违例报错) |
| 宿主API产出字符串 | `C.CString("")`新分配 | `newString("")`发新非interned空对象(≥1) | 一致 |
| 读行 | `bufio.Scanner`:EOF由`Scan()`返回false表达,`Text()`为`""` | `readLine`:EOF与空行都返回`""`,靠`hasInput()`区分;native宿主的null=EOF惯例在native层折叠,从不外露;`hasInput()`对控制台/调试会话仅报告已缓冲输入,循环须写成`hasInput()`→`readLine()`,不能读后判EOF | 同向(缺席由独立探针表达,字符串自身恒为`""`) |
| Channel元素 | channel本身可为nil,元素是string值 | 元素=擦除堆句柄,装箱跨载体(box idx≥1,payload 0),与List同 | 一致 |
| VM内建字符串操作(intrinsic) | 运行时内部直接构造string头 | `OP_CallIntrinsic`不经native边界检查,合法产出句柄0(内部域) | 一致 |
| 编译期常量池 | `""`常量=nil指针头+0长度 | `""`常量intern至slot 0,Execute起始物化 | 一致 |
| native读string数组元素(`getArrayView`) | cgo不直接传slice,无直接对应 | 元素句柄0＝"",经`getString`读得有效`""`C串 | NLang特有,同D26 |
| 宿主Null入string形参 | 不适用(cgo无此形状) | 容忍:落为句柄0→被调读`""`(文档化降级,与既有事实行为一致) | NLang扩展 |
| embed桥string结果(VM→宿主) | 不适用 | 句柄0一律拷出`""`(`StrValCopy`),宿主永不见null字符串 | NLang扩展(值语义) |
| embed桥宿主产出字符串(宿主→VM) | `C.CString("")`新分配 | `MintHostString`与`newString`同规永不铸0,空内容发新非interned空对象 | 一致 |

## 4. Go的native边界(cgo)与NLang的D27

cgo的三个事实:

- `C.CString("")`返回指向单个`'\0'`字节的有效指针——Go到C方向,空内容也有真实存储,永不返回nil。
- `C.GoString(nil)`静默返回`""`——C到Go方向,NULL指针被直接转成空字符串:`findnull`对nil返回长度0,`gostring`对长度0直接返回`""`,不经strlen、不崩溃不panic;cgo官方文档亦明文规定"If the pointer is nil, C.GoString returns the empty string."(源码与官方文档双重核实,见§8;坊间与本地参考流传的"直接panic"说法与两者均不符)。
- 社区标准配方是先判nil再决定:`if p == nil { ... }`,按契约把NULL处理成`""`或错误,绝不盲目转换。

NLang的D27是这个配方的制度化:native ABI的string结果域是"句柄或null"——0表示"native没有产出字符串",属契约违例,宿主直接抛带函数名的host级错误。这里与Go是有意分歧:Go的`GoString`把NULL静默变成`""`,把生产者的契约违例伪装成合法空字符串;社区"先判nil"的惯例本身就说明盲目转换被认为有风险,NLang不把这一风险留给调用方。native要产出字符串(包括空内容)必须经`newString`,而`newString`永不发0——空内容发一个新的非interned空对象,对应`C.CString("")`的新分配。

内部面则相反:句柄0是不朽interned空字符串(D26),对应Go的`""`常量(nil指针头、不占存储、直接可用)。**内部0=空字符串,边界0=null**,两个域各持一端,边界检查是两者之间唯一的转换点。同一规则有两个边界实例:native ABI(`newString`铸柄+派发后检查)与embed宿主桥(`MintHostString`铸柄+同一检查);embed反方向(VM→宿主)的string单元一律解码为String值(句柄0拷出`""`),宿主永不见null字符串。

## 5. "缺席"的表达方式

Go表达"缺席"的手段全部在string类型之外:

- `*string`的nil指针:区分"无"与"有但为空";
- map的comma-ok惯用法;`url.Values.Get`、`Header.Get`对缺失返回`""`(标准库在这两处主动选择把"无"与"空"合并);
- interface的nil;
- error:cgo不表达"缺席"——`GoString(nil)`把NULL静默变`""`,"无"与"空"的区分留给调用方"先判nil"的惯例;NLang在边界直接以host级错误表达,不伪装成字符串。

NLang的对应手段:

- 引用类槽(Object/class槽)保持0=null,字符串只以装箱形式进入;
- `Object == null`区分"无";
- Dict的`contains()`区分"无";
- native边界以host级错误表达"没有产出字符串";
- URL库对query/fragment区分"无"与"空"(表示机制随URL库设计,原则与本文一致:缺席不由字符串类型自身表达)。

## 6. 分歧点与理由

| 分歧 | Go | NLang | 理由 |
|------|----|-------|------|
| 表示形态 | 值头(ptr,len),按值复制 | int32句柄指向GC堆store | NLang字符串有cons→flat的原地变形,需要统一GC根与标记;4字节句柄使帧槽、记录cell的复制与零初始化都便宜 |
| intern | 不intern,相等按内容 | 短串interned,句柄相等为快路径 | 相等热点(Dict键、switch、缓存)受益;slot 0即interned`""`,是全表唯一 |
| NULL C串入Go | 静默返回`""`(`findnull`的nil守卫) | host级错误,带函数名 | 分歧:Go的静默转换把生产者契约违例伪装成合法空串;NLang的带名错误让缺陷可定位,与社区"先判nil"的纪律一致 |
| "无"与"空"合并 | 标准库部分API主动合并(如`os.Getenv`) | native必须返回字符串,"缺席"须在API形状上显式表达(out参数、异常或引用槽) | NLang不允许null字符串,倒逼API把"缺席"显式化,不留隐式歧义 |

## 7. 裁定摘要

- **D26**:VM内部字符串域没有null。句柄0是不朽、interned的空字符串;所有零初始化槽(帧局部、堆记录、静态槽、擦除泛型槽)直接读出`""`,无需填充遍历。
- **D27**:native ABI边界保持0=null。声明返回string的native返回0属契约违例,抛host级错误"native function '<name>' returned null string";非活动句柄抛"invalid string handle N"。`newString`永不发0,空内容发新的非interned空对象(句柄≥1)。
- **D27配套**:embed宿主桥是第二个外部产出边界——`MintHostString`与`newString`同规永不铸0(空内容发新非interned空对象);反方向(VM→宿主)string单元一律产出String值(句柄0拷出`""`);宿主`Value::Null`入string形参被容忍,落为`""`(文档化降级)。
- 不变量:slot 0是全表唯一interned的`""`;新发的空对象按内容相等;interning唯一性不变量(每个内容至多一个活动interned对象)不受影响。

## 8. 参考来源

- Go标准库`builtin.go`对string的定义("A string may be empty, but not nil"):https://go.dev/src/builtin/builtin.go
- Go语言规范(零值):https://go.dev/ref/spec
- cgo文档(`C.GoString`/`C.CString`):https://pkg.go.dev/cmd/cgo
- Go cgo官方文档源码`src/cmd/cgo/doc.go`(原文"If the pointer is nil, C.GoString returns the empty string."):https://gitee.com/mirrors/go/raw/master/src/cmd/cgo/doc.go
- unsafe包文档(`unsafe.String`对nil指针/零长度的规定):https://pkg.go.dev/unsafe#String
- Go 1.20发布说明(unsafe.String):https://go.dev/doc/go1.20#unsafe
- 社区讨论"为什么Go的string没有nil":https://www.xnip.cn/tiku/170229.html
- cgo NULL指针处理配方(先判nil再GoString):https://www.17golang.com/article/633109.html
- Go源码`runtime/string.go`(`gostring`对长度0直接返回`""`,`findnull`对nil返回0):https://gitee.com/mirrors/go/raw/master/src/runtime/string.go
- Go源码`cmd/cgo/out.go`(`goStringDef`生成对`runtime.gostring`的调用;`cStringDef`分配`len+1`并写终止符,永不返回nil):https://gitee.com/mirrors/go/raw/master/src/cmd/cgo/out.go
- 本地参考:`docs/ref/null_vs_go_string.md`(其中"C.GoString(nil)会直接panic"的说法经上述源码核实不成立,系坊间以讹传讹,本文已按源码修正)
