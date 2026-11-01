# 7.2 Parameters and Arguments

A formal parameter belongs to the function scope (§3.5) and is
initialized from the corresponding argument at each call. The
initialization follows the value model of chapter 4: a struct
formal receives a copy (§4.7), while a class, interface, array or
function formal refers to the same object as the argument (§4.10,
§4.8, §7.4). The argument must be assignable to the parameter
type (§4.4). Parameters are assignable like local variables
(§6.2).

A formal may declare a default value `T p = e` at any position
(§2.4). The default expression is resolved in the scope of the
declaration and evaluated at the call site, at each call that
omits the argument; it may reference earlier formals of the same
declaration. The value must be assignable to `T` (§4.4). Defaults
belong to the declaration: a call through a module import fills
them the same way, including defaults that reference earlier
formals or functions of the declaring unit.

A call may bind arguments by formal name: `f(name = expr)`.
Positional arguments precede all named ones; named arguments may
appear in any order and bind by name, not position. Each formal is
bound at most once. A name that is not a formal of the selected
candidate (§7.3) is a compile-time error. Named arguments are
evaluated left to right (§5.1). A constructor call takes
positional arguments only (§7.3).

A formal declared `out T p` (§2.4) is an inout slot: the
argument's current value initializes the formal, and when the
callee returns, the formal's final value is written back to the
argument. The write-backs happen in parameter order after the call
returns, and the call's return value is stored before them: when
the result variable and an `out` argument are the same variable,
the write-back is the final store. The argument must be a local
variable or parameter: a field, an element, a subscript or an
arbitrary expression is a compile-time error. Each of the
following is a compile-time error: an `out` formal with a default
value (§2.4); an `out` formal of struct type; a call with more
than 32 `out` arguments; an `out` argument on a constructor or
`super()` call, on a built-in container method, on a call that
dispatches virtually or through an interface, on an enum method,
or on a native function (§7.5). `out` is a reserved keyword
(chapter 1).

### Default values

*Informative:* defaults can appear at any position, not only at the
end, and a default expression may reference earlier formal
parameters.

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
- Enum methods do **not** support default parameters (§4.6.2)

A class or struct constructor's default parameter expressions cannot
reference `this` — both the explicit `this.a` form and the implicit
bare-field form are compile errors, because defaults evaluate at the
construction call site, where the object under construction does not
exist yet (§4.8.6, §4.7.5).

A default expression that crosses a `.ncu` boundary travels as a
small expression tree over a foldable subset (§20.6); a default
outside that subset is reported as an error by the consumer that
must materialize it. Sources compiled together in one project use
defaults directly.

### Named arguments

*Informative:* arguments may be passed by name with `name = expr`
syntax; positional arguments must precede named arguments.

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
calls`); see [Overload Resolution](overloads.md) "Constructor overloads".

### `out` parameters

*Informative:* an `out` parameter is a writeback slot: the caller's
local variable is copy-initialized into the callee's frame slot, and
after the call returns, the callee's value is copied back to the
caller's variable.

```nlang
int divide(int a, int b, out int rem) {
    rem = a % b;
    return a / b;
}
int r = 0;
int q = divide(17, 5, out r);  // q=3, r=2
```

- Out parameters are **inout**: the callee sees the caller's current value
  as the initial value of the parameter local.
- The writeback occurs after the call returns.
- The call's return value is stored to the result variable
  **before** out writebacks, so `int x = f(out x)` reads the old `x`
  for the return assignment and then overwrites `x` with the callee's
  output.

The compile-time restrictions and their diagnostics:

- Out arguments must be **local variables** (not fields, array elements,
  or arbitrary expressions).
- An out parameter may be any scalar or class type; a **struct is the one
  exclusion** (`out parameter "rem" cannot be a struct`) — structs have
  variable-size value semantics incompatible with the fixed-slot
  writeback mechanism.
- An out parameter cannot carry a default value.
- No `out` parameters on native functions (`native function "f" cannot
  have out parameters`) or enum methods (`enum method "m" cannot have
  out parameters`).
- No `out` arguments on built-in (standard-library) method calls
  (`out arguments are not supported by built-in methods`).
- No `out` on virtual/interface dispatch calls — the writeback mask is
  baked against the statically bound callee's layout (`out arguments are
  not supported on virtual method "m".`).
- No `out` on constructor calls or `super()` (`out arguments are not
  supported in constructor calls.`).
- At most 32 out parameters per call.
- `out` is a reserved keyword.
