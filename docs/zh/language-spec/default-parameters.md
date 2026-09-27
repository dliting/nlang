# 默认参数


函数的形参可以带默认值。默认值可以出现在任意位置（不限于末尾）。默认表达式
可以引用更前面的形参。

```nlang
int foo(int a, int b = 0) { return a + b; }
int foo(int a, int b = a + 1) { return b; }       // 引用更前面的形参
int foo(int a, int b = 0, int c) { return c; }     // 默认值不在末尾
```

- `foo(5)` → `b` 取默认值
- `foo(5, 10)` → `b` 为 10，默认值不评估
- 默认表达式在调用点求值（不是在声明处）
- 默认表达式与形参类型不匹配是编译错误

注意：enum 方法**不**支持默认参数（见 [枚举](enum.md)「枚举方法」）。
