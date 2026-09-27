# Named Arguments


Arguments may be passed by name using `name = expr` syntax. Positional
arguments must precede named arguments.

```nlang
int foo(int a, int b) { return a * 10 + b; }
foo(a = 5, b = 7);     // named, any order
foo(5, b = 7);          // mixed: positional then named
foo(b = 7, a = 5);      // named, reversed order
```

Errors:
- `foo(b = 2, 1)` — positional after named: compile error
- `foo(1, a = 2)` — duplicate binding for `a`: compile error
- `foo(c = 1)` — unknown parameter name: compile error
