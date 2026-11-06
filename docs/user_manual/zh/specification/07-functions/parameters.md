> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 7.2 参数与实参

形式参数属于函数作用域（§3.5），在每次调用时由对应的实参初始化。初始化遵循第4章的值模型：结构体形参收到副本（§4.7），而类、接口、数组或函数形参与实参指代同一个对象（§4.10、§4.8、§7.4）。实参必须可赋值给参数类型（§4.4）。参数与局部变量一样可赋值（§6.2）。

形参可以在任意位置声明默认值`T p = e`（§2.4）。默认表达式在声明的作用域中解析，在调用点求值，于每次省略该实参的调用时求值；它可以引用同一声明中更早的形参。该值必须可赋值给`T`（§4.4）。默认值属于声明：经模块导入的调用以同样的方式填充它们，包括引用更早形参或声明单元中函数的默认值。

调用可以按形参名字绑定实参：`f(name = expr)`。按位置的实参排在所有命名实参之前；命名实参可以按任意顺序出现，按名字而非位置绑定。每个形参至多绑定一次。不是所选候选者（§7.3）的形参的名字是编译期错误。命名实参从左到右求值（§5.1）。构造器调用只接受按位置的实参（§7.3）。

声明为`out T p`（§2.4）的形参是一个inout槽：实参的当前值初始化该形参，被调方返回时，形参的最终值写回实参。写回在调用返回后按参数顺序发生，且调用的返回值先于写回存储：当结果变量与某个`out`实参是同一个变量时，写回是最终的存储。实参必须是局部变量或参数：字段、元素、下标或任意表达式都是编译期错误。下列每一项都是编译期错误：带默认值的`out`形参（§2.4）；结构体类型的`out`形参；带超过32个`out`实参的调用；在构造器或`super()`调用、内建容器方法、虚分派或接口分派的调用、枚举方法或原生函数（§7.5）上使用`out`实参。`out`是保留关键字（第1章）。

### 默认值

*资料性：*默认值可以出现在任意位置（不限于末尾）。默认表达式可以引用更前面的形参。

```nlang
int foo(int a, int b = 0) { return a + b; }
int foo(int a, int b = a + 1) { return b; }       // 引用更前面的形参
int foo(int a, int b = 0, int c) { return c; }     // 默认值不在末尾
```

- `foo(5)` → `b`取默认值
- `foo(5, 10)` → `b`为10，默认值不评估
- 默认表达式在调用点求值（不是在声明处）
- 默认表达式与形参类型不匹配是编译错误
- enum方法**不**支持默认参数（§4.6.2）

class或struct构造函数的形参默认值不能引用`this`——显式`this.a`与裸字段名的隐式形式都是编译错误，因为默认表达式在构造调用点求值，此时被构造的对象尚不存在（§4.8.6、§4.7.5）。

跨越`.ncu`边界的默认表达式以小表达式树的形式传递，覆盖一个可折叠的子集（§21.6）；子集之外的默认值由必须把它实体化的消费方报告为错误。在同一工程里一起编译的源码直接使用默认值。

### 命名实参

*资料性：*实参可以用`name = expr`语法按参数名传递。位置实参必须出现在命名实参之前。

```nlang
int foo(int a, int b) { return a * 10 + b; }
foo(a = 5, b = 7);     // 命名，任意顺序
foo(5, b = 7);          // 混合：先位置后命名
foo(b = 7, a = 5);      // 命名，倒序
```

错误：

- `foo(b = 2, 1)` —— 命名后出现位置实参：编译错误（`positional argument cannot follow a named argument in call to "foo".`）
- `foo(1, a = 2)` —— 对`a`重复绑定：编译错误（`duplicate named argument "a" in call to "foo".`）
- `foo(c = 1)` —— 未知形参名：编译错误

构造器调用只接受位置实参——`Point(x = 1, y = 2)`是编译错误（`named arguments are not supported in constructor calls`）；见[重载解析](overloads.md)「构造器重载」。

### `out`参数

*资料性：*声明为`out`的形参是写回槽：调用方的局部变量被拷贝初始化到被调方的帧槽中，调用返回后，被调方的值被拷贝回调用方的变量。

```nlang
int divide(int a, int b, out int rem) {
    rem = a % b;
    return a / b;
}
int r = 0;
int q = divide(17, 5, out r);  // q=3, r=2
```

- out参数是**inout**：被调方看到的参数局部初值是调用方当前的值。
- 写回发生在调用返回后。
- 调用的返回值在out写回**之前**存入结果变量，所以`int x = f(out x)`在返回值赋值时读到旧`x`，随后用被调方的输出覆盖`x`。

编译期限制及其诊断文案：

- out实参必须是**局部变量**（不能是字段、数组元素、或任意表达式）。
- out参数可以是任意标量或class类型；**struct是唯一例外**（`out parameter "rem" cannot be a struct`）——struct有变长值语义，与固定槽写回机制不兼容。
- out参数不能带默认值。
- native函数不能有out参数（`native function "f" cannot have out parameters`）；enum方法同样（`enum method "m" cannot have out parameters`）。
- 内建（标准库）方法调用不接受out实参（`out arguments are not supported by built-in methods`）。
- 虚/接口分派调用上不能用`out`——写回掩码按静态绑定的被调方布局烧录（`out arguments are not supported on virtual method "m".`）。
- 构造调用或`super()`上不能用`out`（`out arguments are not supported in constructor calls.`）。
- 每次调用最多32个out参数。
- `out`是保留关键字。
