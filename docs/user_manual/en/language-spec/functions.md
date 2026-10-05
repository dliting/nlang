# Functions


A function has a return type, zero or more named parameters, and a body. This
page covers the core (declaration, parameters, return, recursion, `void`) and
the stack-frame layout; each feature has its own page.

```nlang
int add(int a, int b) {
    return a + b;
}
```

- Parameters: int/float/string/enum passed by value, struct passed by value
  (deep copy), class passed by reference — see [Type Semantics](type-semantics.md).
- Return type: int, float, string, enum, struct (deep copy), class (reference).
- Recursion: supported, with a depth limit of 1000.

### Function feature pages

| Feature                     | Page |
|-----------------------------|------|
| default parameters          | [Default Parameters](default-parameters.md) |
| named arguments              | [Named Arguments](named-arguments.md) |
| overload resolution          | [Overload Resolution](overload-resolution.md) |
| out parameters                | [Out Parameters](out-parameters.md) |
| native functions              | [Native Functions](native-functions.md) |
| function types & delegates    | [Function Types & Delegates](function-types-and-delegates.md) |

### `void` functions

A function may declare `void` as its return type — it returns no value. Works
for free functions, class methods (any modifier combination, e.g.
`public static void f()`), and interface members (which still require
`public`, like all interface members).

```nlang
void log(int level) {
    if (level == 0) { return; }   // bare `return;` for early exit
}

class Counter {
    public int hits;
    public void bump(int by) { this.hits = this.hits + by; }
}
```

- Bare `return;` exits early; a void function cannot `return expr;`
- A value-returning function cannot use bare `return;` (compile error)
- The result of a void call cannot be consumed — assigning it, using it as an
  operand, or returning it are all compile errors (the call must be an
  expression statement: `log(3);`)
- Void functions combine with out parameters for side-effect-only calls
- Cross-module: imported void stubs likewise carry no consumable result; the
  same no-result-consumption rules apply on the consumer side
- `void` is not a valid type in any other position (local, field, parameter,
  array element — all rejected at parse time)

### Frame layout

Each function's local frame is sized dynamically from its body — frames
grow with the call's actual needs, and deep recursion is bounded by the
recursion depth limit (1000). A sanity ceiling of 64 formals
prevents unreasonably large frames; exceeding it is a declaration-time
error.
