# 复合赋值


```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

`x = x op y`的读-改-写简写。支持的左值：局部变量、class字段
（`this.f += y`）、struct字段（`pt.x += y`）。左值只求值**一次**（所以
`obj.something() += 1`不会两次调用`something()`）。

不支持：下标左值（`arr[i] += 1`）。字节码帧布局没有足够的暂存槽来做下标读-
改-写的单次求值。请改用显式形式`arr[i] = arr[i] + 1`。见[数组](array.md)
「下标读写」。
