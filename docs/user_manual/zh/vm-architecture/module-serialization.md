# 模块序列化

编译产出的单元映像以`.ncu`文件分发与加载：字符串常量、函数、struct与class表按固定布局依次序列化，加载器按同一布局读回，并通过major/minor版本号强制最低格式版本。多单元的程序与库打成`.npkg`包归档（见文末）。想直接查看某个产物的内容，可以用[ndisasm](../cli-tools/ndisasm.md)在命令行反汇编检查。

编译后的单元映像保存为`.ncu`文件，布局如下：

```text
"NLANGCU "    magic (8 bytes, NUL-padded)
uint16 majorVer = 2
uint16 minorVer = 1
string modulePath        v2.0：本单元的点分模块路径（包身份）
string moduleName
string entryKey          v2.0：入口函数的限定名；空 = 本单元无入口
string[] stringConstants
function[] functions
struct[] structs
class[] classes
arrayType[] arrayTypes
enumNames[] / enumKeys[] 并行的两张枚举表
importSlots × 4          v2.0：function/class/struct/enum 四张导入槽表
```

四张导入槽表各为一列「模块路径 + 名字 + 形参数」条目（函数表另带ownerClassKey，空 = 命名空间级函数）。自有条目占用各表前缀下标0..n-1，导入槽追加在其后（n..n+m）——跨单元调用的操作数与自有调用共用同一个下标空间，链接器对全部操作数统一重映射，无需区分操作码。入口不再序列化为索引：程序包的入口记录（`.npkg`头部）或裸单元的`<modulePath>.main`约定，在加载期按限定名解析成表下标。最低格式版本为2.0：加载器直接拒绝一切v1.x映像，旧模块必须重新编译。

每个struct包含：name、fieldCount、fieldNames[]、fieldTypeKinds[]、fieldStructIndices[]、fieldClassIndices[]、fieldTypeDescs[]。

每个class包含：name、fieldCount、superClassIdx、fieldNames[]、fieldTypeKinds[]、fieldStructIndices[]、fieldClassIndices[]、fieldTypeDescs[]、fieldAccess[]、methodIndices[]、constructorIdx。

## 包归档（.npkg，格式1.0）

多单元程序与库的分发形态是包归档：头部（包名、格式版本、标志、预留的签名块描述符，以及**程序包的入口记录**——入口成员的模块路径加函数名）、成员表（模块路径 → 偏移/长度/校验和，按路径排序保证字节确定性）与内嵌的`.ncu`单元映像。每个成员带一份FNV-1a 64校验和（防意外损坏，不是安全机制）。库包是同一容器、不带入口记录——标准库`stdlib.npkg`就是成员为`io`/`math`/`fs`的库包。加载器要求成员映像头部的模块路径与成员表条目一致：名实不符的产物被拒绝，而不是被静默接受。
