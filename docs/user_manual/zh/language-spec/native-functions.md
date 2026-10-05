# native函数


标记为`native`的函数声明**没有函数体**——实现由嵌入宿主在运行期提供：

```nlang
native int natAdd(int a, int b);
native float natFAdd(float a, float b);
native void natPing();

int main() {
    return natAdd(20, 22);  // 42 —— 分派到宿主
}
```

**模型。**声明编译为只携带签名的函数记录（无字节码）。自由函数的查找键是`<包名>.<函数名>`（包名取声明所在翻译单元的模块路径；类成员方法保持裸名）。调用点上，VM先查宿主注册的native函数表；带包名的键未命中时，装载器按包名在搜索路径上定位`nlang_<包名>.dll`并由它注册（懒加载：首次调用才触发装载）；仍未命中才抛错。命中后直接以调用方暂存的实参单元调用native实现：

- 应用二进制接口（ABI，application binary interface）：实参`i`是`args[i*4]`处的裸4字节单元——小端`int32`/`float`位或堆索引，与内建函数ABI相同。native实现把4字节返回值写入`ret`（`void` native函数可为null）。
- 解析失败在调用点抛错，绝不静默产生垃圾：包名对应的`nlang_<包名>.dll`不存在时报`cannot find native module 'nlang_<包名>.dll' for package '<包名>'`（后随已搜索目录列表）；动态链接库（DLL，dynamic-link library）装载后仍未注册该名字时报`native function not registered: <name>`。
- 默认参数可用（分派前在调用点填充），跨模块导入亦然（默认值随native标志一起序列化进模块文件）。

**限制：**
- `native`声明**不得有函数体**——编译错误。实现归宿主所有。
- **不支持out参数**——写回需要被调方栈帧，而native函数没有。编译错误；VM对手工构造的模块执行同样检查。
- 自由函数的键是**全限定**的（`<包名>.<函数名>`）：不同包里的同名native函数互不相干，同一包内同名只算一个表条目。宿主注册方负责匹配声明的签名；不匹配（如对int native函数声明`native string`）会产出垃圾输出，而不是类型错误。
- **多段包名（含`.`）里不允许声明native函数**——宿主DLL按第一个点之前的包名段命名，多段包名无法命名一个DLL；编译错误。
- 跨模块：导入含native函数的`.ncu`的模块经同一张表调用它们（native标志在模块合并后保留）。
- class成员的`native`方法可用：分派经常规方法路径到达native实现，`this`位于`args[0]`（接收者的堆索引），后接声明的参数——与字节码调用约定一致。因此同一实现绑定为方法时，宿主native函数在`args[(1+j)*4]`读用户参数`j`；绑定为自由函数时在`args[j*4]`。把一个实现同时注册成两种形状属于签名不匹配（见上）。
- 超出裸4字节ABI的string/struct/class实参编组不支持。
- **测试宿主说明**：`ncc`、`nvm`与`ndb`二进制是测试宿主——它们按特定包名注册了一小撮测试native函数（`natAdd`、`natConst`、`natFAdd`、`natPing`）；自己工程里声明同名native函数走的是`nlang_<自家包名>.dll`查找，不会命中测试注册。生产嵌入方的宿主不注册这些测试名字；对这样的宿主调用未注册的名字会在运行期得到`"native function not registered"`。

