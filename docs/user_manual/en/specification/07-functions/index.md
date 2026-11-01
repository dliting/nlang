# 7. Functions

A function has a return type or `void`, a name, zero or more formal
parameters, and a body. This chapter fixes the declaration forms, the
parameter and argument rules, overload resolution, function values and
native functions; the syntax of a declaration is §2.2 at top level and
§2.4 for class, struct, enum and interface members. Parameters and
arguments (§7.2), overload resolution (§7.3), function values and
delegates (§7.4) and native functions (§7.5) each have their own page.

## 7.1 Declaration Forms

The syntax of a function or method declaration is §2.2 at top
level and §2.4 for class, struct, enum and interface members. A
declaration gives a return type or `void`, a name, zero or more
formal parameters, and a body. A method declaration with no return
type names a constructor (§4.8.6).

A top-level function is *bare* when it carries no access modifier
and no flags (§2.2). The distinction is grammatical only: at top
level the access modifiers and the `static` and `virtual` flags
are accepted and have no effect — they govern class members
(§4.8). Within its own unit every top-level function is
visible bare. Across units, bare visibility follows §3.6, and the
qualified name reaches the function through the import gate
(§3.6, §3.9); neither path consults the modifiers. Only
`native` carries meaning at top level (§7.5).

A body written `;` declares a function without a body: a native
function (§7.5) at top level, or an interface member with no
default implementation (§4.9.2).

A `void` function returns no value. `return;` exits it early
(§6.6); a call to it is used only as a statement — assigning its
result, returning it, or using it as an operand is a compile-time
error (§5.6). `void` is not a type in any other position: a local,
field, parameter or array element of type `void` is a syntax
error.

A function declares at most 64 formal parameters; more is a
declaration-time error. Recursion is allowed. The call depth is
bounded (1000 frames); exceeding the bound raises a *host-level*
run-time error: the host reports the error and ends the program,
and no `catch` clause handles it (chapter 8).

### A function declaration

```nlang
int add(int a, int b) {
    return a + b;
}
```

*Informative:* a method is declared inside a class, struct, enum or
interface and follows the same shape; a method declared with no
return type is a constructor (§4.8.6). What happens to a value at
the parameter and return boundaries follows the value model of
chapter 4: a struct is copied, a class, interface, array or function
value shares its object, and a return value moves by the same rule
(§7.2, §6.6). Arrays and containers work as return types too (see
[Array](../04-types/array.md) and [Collection
initializers](../05-expressions/initializers.md)).

### `void` functions

*Informative:* `void` works for free functions, class methods (any
modifier combination, e.g. `public static void f()`) and interface
members (which default to `public` like all interface members).

```nlang
void log(int level) {
    if (level == 0) { return; }   // bare `return;` for early exit
}

class Counter {
    public int hits;
    public void bump(int by) { this.hits = this.hits + by; }
}
```

- A value-returning function cannot use bare `return;` (compile error)
- Void functions combine with `out` parameters for side-effect-only
  calls (§7.2)
- Cross-module: an imported `void` function likewise carries no
  result; the same no-result rules apply there

### Frame layout

*Informative:* each function's local frame is sized dynamically from
its body — frames grow with the call's actual needs, and deep
recursion is bounded by the recursion depth limit (1000). The
sanity ceiling of 64 formals prevents unreasonably large frames.

### Where to look

- **Formal parameters, default values, named arguments, `out`
  arguments** — [Parameters and Arguments](parameters.md).
- **Which overload a call selects** — [Overload
  Resolution](overloads.md).
- **Passing a function as a value** — [Function Values and
  Delegates](function-values.md).
- **Host-implemented functions** — [Native Functions](native.md).
- **`return` as a statement** — §6.6.
