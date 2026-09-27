# Compound Assignment


```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

Read-modify-write shorthand for `x = x op y`. Supported left-values: local
variables, class fields (`this.f += y`), struct fields (`pt.x += y`). The left
value is evaluated **only once** (so `obj.something() += 1` would not
double-invoke `something()`).

Not supported: subscript left-value (`arr[i] += 1`). The bytecode frame layout
doesn't have enough scratch slots for single-evaluation of subscript
read-modify-write. Use the explicit form `arr[i] = arr[i] + 1`. See
[Array](array.md) "Subscript read/write".
