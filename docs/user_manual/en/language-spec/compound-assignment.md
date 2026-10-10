# Compound Assignment


```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

Read-modify-write shorthand for `x = x op y`. Supported left-values: local
variables, class fields (`this.f += y`), struct fields (`pt.x += y`), and
subscript elements (`arr[i] += y`, `li[i] += y`, `d[k] += y`). The
left-value is evaluated **only once**: the receiver of a member left-value
and the base/index of a subscript left-value are not re-evaluated —
`make().x += 5` calls `make()` once, and the read and the write land on the
same object (the expansion `make().x = make().x + 5` would call `make()`
twice and store into a different object); `arr[f()] += 1` calls `f()` once.

For string elements only `+=` (concat) is legal; the other four operators
are rejected with `operator not supported on string`. A string base is not
supported (`string does not support subscript access`, see [Common
Errors](common-errors.md) "Assignment targets"). `d[k] += v` throws when
the key is missing (same as reading `d[k]`).
