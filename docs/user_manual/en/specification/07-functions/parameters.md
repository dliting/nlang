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
small expression tree over a foldable subset (§28.6); a default
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

### Reference parameters

A formal declared `T& p` names the caller's argument storage
(§4.7.9): no copy is made, and writes through `p` land in the
argument. At the call site the argument must be bindable storage of
the exact type — a local, a formal, a field or an element; literals
and computed results are rejected by the binding rules (§4.7.9). A
`const T& p` formal accepts the same arguments read-only, and
additionally binds const storage and expression results, which
materialize into a temporary kept alive by the formal.

Which formals can modify the caller's variable:

| Formal kind | Caller's variable after the call |
|-------------|----------------------------------|
| `T p` (value) | unchanged — the formal worked on a copy |
| `T& p` (reference) | carries every write made through `p` |
| `const T& p` | unchanged — writes through `p` are compile errors |

A reference formal passes through virtual and interface dispatch
like any other formal — the reference is data in the ordinary
parameter slot, and the dispatch itself is unaware of it.

`out` remains a reserved word (chapter 1), but the reference formal
is the only output-parameter form: a declaration spelling `out T p`
is a compile-time error pointing at the `T&` spelling (`out
parameters are retired; declare "p" as a reference formal
instead.`).
