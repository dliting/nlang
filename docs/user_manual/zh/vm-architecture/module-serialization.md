# 模块序列化

编译产出的单元映像以`.ncu`文件分发与加载：字符串常量、函数、struct与class表按固定布局依次序列化，加载器按同一布局读回，并通过major/minor版本号强制最低格式版本。多单元的程序与库打成`.npkg`包归档（见文末）。想直接查看某个产物的内容，可以用[ndisasm](../cli-tools/ndisasm.md)在命令行反汇编检查。

编译后的单元映像保存为`.ncu`文件，布局如下：

```text
"NLANGCU "    magic (8 bytes, NUL-padded)
uint16 majorVer = 2
uint16 minorVer = 8
string modulePath        v2.0：本单元的点分模块路径（包身份）
string moduleName
string entryKey          v2.0：入口函数的限定名；空 = 本单元无入口
string[] stringConstants
function[] functions
struct[] structs
class[] classes
interface[] interfaces
arrayType[] arrayTypes
enumNames[] / enumKeys[] 并行的两张枚举表
importSlots × 4          v2.0：function/class/struct/enum 四张导入槽表
```

四张导入槽表各为一列条目，携带目标的模块路径与限定名；函数表另带形参数、属主键（空 = 命名空间级函数，否则为所属class或struct的限定键）、静态标志、属主种类（0 = 命名空间级，1 = class成员，2 = struct成员）、构造器序号（0xFFFF = 非构造器，否则为属主constructorIndices的下标）。自有条目占用各表前缀下标0..n-1，导入槽追加在其后（n..n+m）——跨单元调用的操作数与自有调用共用同一个下标空间，链接器对全部操作数统一重映射，无需区分操作码。入口不再序列化为索引：程序包的入口记录（`.npkg`头部）或裸单元的`<modulePath>.main`约定，在加载期按限定名解析成表下标。最低格式版本随工具链只升不降，当前为2.9（2.2起类型描述符携带`func`签名，2.3起默认值表达式随映像序列化——导入桩绑定的形参名来自函数记录一直在序列化的locals表，2.4起函数记录增加静态标志、class记录增加导出的public-static方法下标，2.5起interface记录增加常量块，2.6起struct记录增加逐字段默认值块与构造器下标段、函数导入记录增加属主种类与构造器序号，2.7起class记录只携带本类声明的自有字段并增加逐方法访问级与虚标志字节、逐构造器访问级（导出的public-static方法下标段移除——静态性由函数记录的静态标志承载），struct记录增加逐构造器访问级，interface记录增加实例方法签名块，2.8起函数记录追加求值区句柄掩码（垃圾回收器对帧带的逐槽根分类），2.9起enum记录增加逐枚举成员方法下标与访问级块（类型记录携带自有成员函数索引这一机制的第四个实例，与class方法下标、struct构造器下标、interface静态方法下标并列））：加载器拒绝一切低于当前版本的映像，旧模块必须重新编译。

每个struct包含：name、fieldCount、fieldNames[]、fieldTypeKinds[]、fieldStructIndices[]、fieldClassIndices[]、fieldTypeDescs[]、constructorIndices（声明序；空 = 无构造器）、fieldDefaults（每字段一条DefaultValueDesc，RTK_Void = 无默认值）、constructorAccess（每构造器一个访问级字节，与constructorIndices平行）。

每个class包含：name、fieldCount、superClassIdx、fieldNames[]、fieldTypeKinds[]、fieldStructIndices[]、fieldClassIndices[]、fieldTypeDescs[]、fieldAccess[]、methodIndices[]、methodAccess[]与methodVirtual[]（每方法一个字节，与methodIndices平行）、constructorIndices（声明序；空 = 无构造器）、constructorAccess（每构造器一个访问级字节，与constructorIndices平行）。字段段只携带本类声明的字段——完整实例布局在装载期沿超类链推导，与JVM class文件、.NET元数据只存声明成员的做法一致。全部成员函数（含私有与静态）都走`methodIndices`；消费侧依据访问级字节执行可见性检查，而不是隐藏成员。

每个interface包含：name、staticMethodIndices[]、常量块、实例方法签名块——每条签名为方法名、形参数、逐形参标志加类型描述符、返回类型描述符与默认值（数量 = 形参数）。实例方法不带函数记录（按名分派）；签名让消费侧解析经导入接口变量的调用并检查`implements`契约。

## 包归档（.npkg，格式1.0）

多单元程序与库的分发形态是包归档：头部（包名、格式版本、标志、预留的签名块描述符，以及**程序包的入口记录**——入口成员的模块路径加函数名）、成员表（模块路径 → 偏移/长度/校验和，按路径排序保证字节确定性）与内嵌的`.ncu`单元映像。每个成员带一份FNV-1a 64校验和（防意外损坏，不是安全机制）。库包是同一容器、不带入口记录——标准库`stdlib.npkg`就是成员为`concurrent`/`fs`/`io`/`math`/`thread`的库包。加载器要求成员映像头部的模块路径与成员表条目一致：名实不符的产物被拒绝，而不是被静默接受。
