# out参数

规范性规则见规范§7.2。
声明为`out`的形参是写回槽：调用方的局部变量被拷贝初始化到被调方的帧槽中，调用返回后，被调方的值被拷贝回调用方的变量。

```nlang
int divide(int a, int b, out int rem) {
    rem = a % b;
    return a / b;
}
int r = 0;
int q = divide(17, 5, out r);  // q=3, r=2
```

**语义：**
- out参数是**inout**：被调方看到的参数局部初值是调用方当前的值。
- 写回发生在调用返回后。
- 调用的返回值在out写回**之前**存入结果变量，所以`int x = f(out x)`在返回值赋值时读到旧`x`，随后用被调方的输出覆盖`x`。

**限制：**
- out实参必须是**局部变量**（不能是字段、数组元素、或任意表达式）。否则编译错误。
- out参数可以是任意标量或class类型；**struct是唯一例外**（`out parameter "rem" cannot be a struct`）——struct有变长值语义，与固定槽写回机制不兼容。
- out参数不能带默认值。
- native函数不能有out参数（`native function "f" cannot have out parameters`）；enum方法同样（`enum method "m" cannot have out parameters`）。
- 内建（标准库）方法调用不接受out实参（`out arguments are not supported by built-in methods`）。
- 虚/接口分派调用上不能用`out`——写回掩码按静态绑定的被调方布局烧录（`out arguments are not supported on virtual method "m".`）。
- 构造调用或`super()`上不能用`out`（`out arguments are not supported in constructor calls.`）。
- 每次调用最多32个out参数。
- `out`是保留关键字。
