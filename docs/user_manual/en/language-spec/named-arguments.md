# Named Arguments

The normative rules are in [specification §7.2](../specification/07-functions.md).
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
  (`positional argument cannot follow a named argument in call to "foo".`)
- `foo(1, a = 2)` — duplicate binding for `a`: compile error
  (`duplicate named argument "a" in call to "foo".`)
- `foo(c = 1)` — unknown parameter name: compile error

Constructor calls take arguments positionally only — `Point(x = 1, y = 2)`
is a compile error (`named arguments are not supported in constructor
calls`); see [Overload Resolution](overload-resolution.md) "Constructor
overloads".
