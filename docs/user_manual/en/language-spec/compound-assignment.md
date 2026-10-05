# Compound Assignment


```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

Read-modify-write shorthand for `x = x op y`. Supported left-values: local
variables, class fields (`this.f += y`), struct fields (`pt.x += y`). The
receiver expression of a member left-value is evaluated **only once**:
`make().x += 5` calls `make()` once, and the read and the write land on the
same object (the expansion `make().x = make().x + 5` would call `make()`
twice and store into a different object).

Not supported: a method call result (`obj.something() += 1`, rejected with
`cannot assign to the result of a method call`, see
[Common Errors](common-errors.md) "Assignment targets") and subscript
left-values (`arr[i] += 1`, a syntax error). Use the explicit form
`arr[i] = arr[i] + 1`. See [Array](array.md)
"Subscript read/write".
