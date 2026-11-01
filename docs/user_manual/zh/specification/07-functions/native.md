> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 7.5 原生函数

标志含`native`且体为`;`的声明（§2.2、§2.4）没有体：由宿主在执行期提供实现。原生函数在其他方面表现得像普通函数：默认实参在分派之前于调用点填充（§7.2），跨模块边界亦然。

自由原生函数以`<package>.<name>`为键，其中`<package>`是声明单元的模块路径（§3.9），`<name>`是它的名字。类成员原生函数保持其裸名；对实例方法，接收者是第一个宿主实参。首次调用时，宿主为该包载入实现；对单段包的自由原生函数，加载器在宿主搜索路径上寻找`nlang_<package>.dll`。包名超过一段的单元不得声明原生函数：编译期错误。原生函数没有`out`参数（§7.2）。

宿主找不到实现时，该调用抛出宿主级执行期错误（§7.1）。由宿主负责匹配声明的签名：声明与宿主实现之间的不一致不是类型错误，产生未指明的结果（§0.4）。调用约定见§22.4。

### 原生声明

*资料性：*`native`声明**没有函数体**——实现由嵌入宿主在运行期提供：

```nlang
native int natAdd(int a, int b);
native float natFAdd(float a, float b);
native void natPing();

int main() {
    return natAdd(20, 22);  // 42 —— 分派到宿主
}
```

### 调用如何解析

*资料性：*声明编译为只携带签名的函数记录（无字节码）。调用点上，VM先查宿主注册的native函数表；带包名的键未命中时，装载器在搜索路径上定位`nlang_<package>.dll`并由它注册（懒加载：首次调用才触发装载）；仍未命中才抛错。命中后直接以调用方暂存的实参单元调用native实现：

- 值布局——每个实参一个8字节单元、方法的接收者槽与返回单元——见§22.4（另见[栈帧布局](../../vm-architecture/stack-frame-layout.md)）。
- 解析失败在调用点抛错，绝不静默产生垃圾：包名对应的`nlang_<package>.dll`不存在时报`cannot find native module 'nlang_<package>.dll' for package '<package>'`（后随已搜索目录列表）；装载后仍未注册该名字的模块报`native function not registered: <name>`。
- 默认参数可用（分派前在调用点填充），跨模块导入亦然（默认值随native标志一起序列化进模块文件）。

### 限制

*资料性：*

- `native`声明**不得有函数体**——编译错误。实现归宿主所有。
- **不支持out参数**——写回需要被调方栈帧，而native函数没有。编译错误；VM对手工构造的模块执行同样检查。
- 自由函数的键是**全限定**的（`<package>.<name>`）：不同包里的同名native函数互不相干，同一包加名字只算一个表条目。宿主注册方负责匹配声明的签名；不匹配（如对int native函数声明`native string`）会产出垃圾输出，而不是类型错误。
- **多段包名（含`.`）里不允许声明native函数**——宿主动态链接库（DLL，dynamic-link library）按第一个点之前的包名段命名，多段包名无法命名一个DLL；编译错误。
- 跨模块：导入含native函数的`.ncu`的模块经同一张表调用它们（native标志在模块合并后保留）。
- class成员的`native`方法可用：分派经常规方法路径到达native实现，接收者位于第一个实参单元，后接声明的参数——与字节码调用约定一致（§22.4）。
- 超出单槽值单元的string/struct/class实参编组不支持。

### 测试宿主

*资料性：*`ncc`、`nvm`与`ndb`二进制是测试宿主——它们按特定包名注册了一小撮测试native函数（`natAdd`、`natConst`、`natFAdd`、`natPing`）；自己工程里声明同名native函数走的是`nlang_<自家包名>.dll`查找，不会命中它们。生产嵌入方的宿主不注册这些测试名字；对这样的宿主调用未注册的名字会在运行期得到`native function not registered`。
