# native 函数


标记为 `native` 的函数声明**没有函数体**——实现由嵌入宿主在运行期提供：

```nlang
native int natAdd(int a, int b);
native float natFAdd(float a, float b);
native void natPing();

int main() {
    return natAdd(20, 22);  // 42 —— 分派到宿主
}
```

**模型。** 声明编译为一条函数记录，只携带它的签名（没有字节码）。在调用点，
VM 在宿主注册的 native 函数表里查找该名字，并用调用方暂存好的实参单元直接
调用 native：

- ABI：第 `i` 个实参是 `args[i*4]` 处的原始 4 字节单元——小端 `int32`/
  `float` 位或堆索引，与内建 ABI 相同。native 把它的 4 字节返回值写入
  `ret`（对 `void` native 可为 null）。
- 调用宿主从未注册的 native 会在调用点抛出（`native function not
  registered: <name>`）——绝不静默产生垃圾值。
- 默认参数可用（在分派前于调用点填充），包括跨模块导入（默认值随 native
  标志一起序列化进模块文件）。

**限制：**
- `native` 声明**不能有函数体**——编译错误。宿主拥有实现。
- **不能有 `out` 参数**——写回需要被调方帧，而 native 没有。编译错误；VM
  对手工构造的模块强制同样规则。
- 表只以**声明名**为键。宿主注册负责匹配声明的签名；签名不匹配（例如对一个
  int native 声明 `native string`）会得到垃圾输出，而不是类型错误。两个模块
  声明同一个 native 名共享一个表项。
- 跨模块：导入含 native 的 `.nmod` 的模块通过同一张表调用它们（native 标志
  在模块合并后保留）。
- 类成员 `native` 方法可用：分派通过正常方法路径到达 native，`this` 位于
  `args[0]`（接收者的堆索引），后随声明的形参——镜像字节码调用约定。因此宿主
  native 作为方法绑定时在 `args[(1+j)*4]` 读用户形参 `j`，作为自由函数绑定
  时在 `args[j*4]` 读。用同一实现注册两种形态属于签名不匹配（见上）。
- 超出原始 4 字节 ABI 的 string/struct/class 实参编组尚未支持。
- **测试宿主说明**：`ncc` 和 `nvm` 二进制是测试宿主——它们始终注册
  `natAdd`、`natConst`、`natFAdd` 和 `natPing`，以便测试套件能演练绑定路径。
  生产嵌入方的宿主不注册这些测试名；脚本对这样的宿主调用这些名字会在运行
  期得到 `"native function not registered"`。
