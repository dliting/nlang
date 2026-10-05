# 函数


函数有返回类型、零个或多个命名形参、以及一个函数体。本页覆盖核心（声明、形参、返回值、递归、`void`）与栈帧布局；每个特性都有独立页面。

```nlang
int add(int a, int b) {
    return a + b;
}
```

- 形参：int/float/string/enum按值传递，struct按值传递（深拷贝），class按引用传递——见[类型语义](type-semantics.md)。
- 返回类型：int、float、string、enum、struct（深拷贝）、class（引用）。
- 递归：支持，深度上限1000。

### 函数特性页面

| 特性                   | 页面 |
|---------------------------|------|
| 默认参数                 | [默认参数](default-parameters.md) |
| 命名实参                 | [命名实参](named-arguments.md) |
| 重载解析                 | [重载解析](overload-resolution.md) |
| out参数                 | [out参数](out-parameters.md) |
| native函数              | [native函数](native-functions.md) |
| 函数类型与委托            | [函数类型与委托](function-types-and-delegates.md) |

### `void`函数

函数可以声明`void`作为返回类型——不返回值。适用于自由函数、类方法（任意修饰符组合，例如`public static void f()`）、以及接口成员（和所有接口成员一样，仍需`public`）。

```nlang
void log(int level) {
    if (level == 0) { return; }   // 用裸 `return;` 提前退出
}

class Counter {
    public int hits;
    public void bump(int by) { this.hits = this.hits + by; }
}
```

- 裸`return;`提前退出；void函数不能`return expr;`
- 值返回函数不能使用裸`return;`（编译错误）
- void调用的结果不可消费——把它赋值、作为操作数、或作为返回值都是编译错误（调用必须是表达式语句：`log(3);`）
- void函数可与out参数组合，实现只有副作用的调用
- 跨模块：导入的void stub同样不携带可消费的结果；消费侧适用同样的"结果不可消费"规则
- `void`在其他任何位置都不是合法类型（局部、字段、形参、数组元素——全部在解析期被拒绝）

### 栈帧布局

每个函数的局部帧按其函数体动态确定大小，随调用的实际需要增长；深层递归由递归深度上限（1000）约束。64个形参的合理性上限防止帧过大；超过它是声明期错误。
