# 复合赋值


```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

`x = x op y`的读-改-写简写。支持的左值：局部变量、class字段（`this.f += y`）、struct字段（`pt.x += y`）。成员左值的接收者表达式只求值**一次**：`make().x += 5`只调用一次`make()`，读与写落在同一个对象上（按`make().x = make().x + 5`展开则会调用两次`make()`，写进另一个对象）。

不支持：方法调用结果（`obj.something() += 1`，报`cannot assign to the
result of a method call`，见[常见错误](common-errors.md)「赋值目标」）与下标左值（`arr[i] += 1`，报语法错误）。请改用显式形式`arr[i] = arr[i] + 1`。见[数组](array.md)「下标读写」。
