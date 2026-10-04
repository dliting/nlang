# Default Parameters


Function parameters may have default values. Defaults can appear at any
position (not just trailing). A default expression may reference earlier
formal parameters.

```nlang
int foo(int a, int b = 0) { return a + b; }
int foo(int a, int b = a + 1) { return b; }       // references earlier param
int foo(int a, int b = 0, int c) { return c; }     // default not at end
```

- `foo(5)` → `b` gets default value
- `foo(5, 10)` → `b` is 10, default not evaluated
- Default expressions are evaluated at the call site (not at declaration)
- Type mismatch between default expression and parameter type is a compile
  error

Note: enum methods do **not** support default parameters (see
[Enum](enum.md) "Enum methods").
