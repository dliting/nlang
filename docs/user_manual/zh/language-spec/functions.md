# 函数


函数有返回类型、零个或多个命名形参、以及一个函数体。本页覆盖核心（声明、形
参、返回值、递归、`void`）与栈帧布局；每个特性都有独立页面。

```nlang
int add(int a, int b) {
    return a + b;
}
```

- 形参：int/float/string/enum 按值传递，struct 按值传递（深拷贝），class
  按引用传递——见 [类型语义](type-semantics.md)。
- 返回类型：int、float、string、enum、struct（深拷贝）、class（引用）。
- 递归：支持，带深度上限（默认 1000）。

### 函数特性页面

| 特性                   | 页面 |
|---------------------------|------|
| 默认参数                 | [默认参数](default-parameters.md) |
| 命名实参                 | [命名实参](named-arguments.md) |
| 重载解析                 | [重载解析](overload-resolution.md) |
| out 参数                 | [out 参数](out-parameters.md) |
| native 函数              | [native 函数](native-functions.md) |
| 函数类型与委托            | [函数类型与委托](function-types-and-delegates.md) |

### `void` 函数

函数可以声明 `void` 作为返回类型——不返回值。适用于自由函数、类方法（任意修
饰符组合，例如 `public static void f()`）、以及接口成员（和所有接口成员一样，
仍需 `public`）。

```nlang
void log(int level) {
    if (level == 0) { return; }   // 用裸 `return;` 提前退出
}

class Counter {
    public int hits;
    public void bump(int by) { this.hits = this.hits + by; }
}
```

- 裸 `return;` 提前退出；void 函数不能 `return expr;`
- 值返回函数不能使用裸 `return;`（编译错误）
- void 调用的结果不可消费——把它赋值、作为操作数、或作为返回值都是编译错误
  （调用必须是表达式语句：`log(3);`）
- void 函数可与 out 参数组合，实现只有副作用的调用
- 跨模块：导入的 void stub 同样不携带可消费的结果；消费侧适用同样的"结果不
  可消费"规则
- `void` 在其他任何位置都不是合法类型（局部、字段、形参、数组元素——全部在
  解析期被拒绝）

### 栈帧布局

每个函数的局部帧根据其函数体动态确定大小：

```text
[this?][params][return slot][temps 1-4][call-argument staging area(N)][evaluation scratch area(peak depth)][user locals...]
```

- **N** = 本函数函数体中观察到的被调方最大形参数（方法额外加槽 0 用于
  `this`）。调用实参暂存区是 `OP_CallFunc`/`OP_CallMethod` 消费的最终落地区。
- **峰值深度** = 所有调用点（包括嵌套调用）同时需要的求值暂存区槽位数的最大
  值（例如 `foo(helper(5), helper(10))` 需要 4 个槽：`foo` 的 2 个实参 +
  内层 2 次 `helper` 调用）。

求值暂存区是一个不相交的、遵循栈纪律的暂存区。每次调用生成实参时按栈纪律在
进入时申请一个切片、退出时释放。绑定被写入所申请的切片；一个批量拷贝循环在
调用前把它们移入调用实参暂存区。因此内层调用的绑定永远不会覆盖外层调用已写
入的绑定。

64 个形参的合理性上限防止帧过大；超过它是声明期错误。
