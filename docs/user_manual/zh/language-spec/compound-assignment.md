# 复合赋值


```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

`x = x op y`的读-改-写简写。支持的左值：局部变量、class字段（`this.f += y`）、struct字段（`pt.x += y`）、下标元素（`arr[i] += y`、`li[i] += y`、`d[k] += y`）。左值只求值**一次**：成员左值的接收者与下标左值的基/索引都不重算——`make().x += 5`只调用一次`make()`，读与写落在同一个对象上（按`make().x = make().x + 5`展开则会调用两次`make()`，写进另一个对象）；`arr[f()] += 1`只调用一次`f()`。

元素为string时仅`+=`（拼接）合法，其余四个操作报`operator not supported on string`。string基不支持（`string does not support subscript access`，见[常见错误](common-errors.md)「赋值目标」）。`d[k] += v`在键缺失时抛异常（与读`d[k]`相同）。
