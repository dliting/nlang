# Known Limitations


- **User-defined generics**: `class Foo<T> { ... }` is not supported. Only
  built-in generic classes (`List<T>`, `Dict<K,V>`) are recognized.
- **Array values in scalar contexts**: an array value has exactly two
  legal destinations — its own array type (interned token identity:
  declarations and value sites share one token per element type) and
  `string` targets in **every** position, whole-value and element slot
  alike: string locals and fields, returns, concatenation, string
  parameters (`f(arr)`), `io.print(arr)`, array subscripts
  (`string[] sa; sa[0] = arr`) and container stores (`List<string>`
  subscript and `.add`) — all via runtime toString coercion
  (`"[1, 2]"`). `null` always converts to an array target. Comparison
  positions are identity-only: `==` / `!=` against another array or
  `null`. Every other use is a compile-time rejection with a named
  error — scalar, class, interface, or `Object` targets, `as` casts
  (`ia as int`), non-array parameters, numeric binary operands
  (`arr + 1`), relational or cross-type comparisons (`arr < arr2`,
  `arr == 5`), condition positions (`if (arr)`), `switch`
  discriminants, and method receivers. Cross-element and
  shared-representation array conversions are rejected alike:
  `string[] b = ia`, the covariant upcast `Base[] ba = da`, and
  `enum[]` ↔ `int[]` interconversion all fail with "an array value
  only converts to the same array type". Elements pulled out of
  `List<T[]>` / `Dict<K,T[]>` follow the same rule; their array-ness
  is visible to the compiler — method-receiver and
  `switch`-discriminant uses (`li[0].rank()`, `switch (li.get(0))`)
  are rejected by name, `foreach` can iterate them into an array-typed
  loop variable (array-valued sources may be used directly and are
  evaluated once), and `Dict` keyed on array types uses handle
  identity.
- **Jagged arrays (`T[][]`)**: multi-dimensional array declarations are
  rejected at compile time ("jagged arrays (T[][]) are not supported") —
  at locals, fields, parameters, return types, `for`/`foreach` loop
  variables, and as generic type arguments. The VM has no
  multi-dimensional array layout; declare flat arrays or use
  `List<List<T>>`-style containers instead.
- **Bare `{...}` collection init**: dict/struct/class init requires the
  explicit `new Type{...}` form (the bare `{...}` form conflicts with
  block-statement grammar). See Collection Initializers above.
- **Bare `[]` empty init**: use `new List<T>{}`, `new Dict<K,V>{}`, or
  `new int[0]` instead. The lexer tokenizes `[]` as a single token used
  by the array-type suffix rule.
- **Right-shift operator (`>>`, `<<`)**: not implemented (no grammar
  production, no opcode). `>>` outside generic context is a compile
  error; inside generic closing position it is split into `'>'` tokens
  (see Nested generics under `List<T>`).
- **Bare init list as function argument**: requires the `new Type{...}`
  explicit form.
- **`List<struct>` value semantics**: adding the same struct variable
  to a List twice shares the underlying heap slot (reference semantics
  at the boxing layer). Use separate struct instances for distinct
  elements.
- **`Dict<K,V>` with interface type**: interface types are not
  supported as generic type arguments. Use concrete class types.
- **Nested-subscript receiver write**: in
  `matrix[i][0].x = v` (array-of-array-of-struct), the receiver's
  inner subscript index evaluation can clobber the RHS temp slot.
  Single-level `arr[i].field = v` works correctly.
- **Eager materialization cost**: `new Point[n]` allocates n+1 heap
  slots at creation (array + one struct per element).
- **Default parameters on imported functions**: cross-module imported
  functions support **constant-foldable** defaults only — int / float /
  string / null literals, plus single negation of numeric literals
  (`-5`, `-3.14`). Complex defaults (identifier references like
  `b = a`, function calls like `b = helper()`, casts, binary
  expressions other than unary `-`, `this.field` references) are
  rejected at the **consumer side** with a compile error. Producers
  (the imported module) accept any default expression; the restriction
  applies only when the consumer imports the function. Workaround for
  complex cross-module defaults: write a wrapper in the producer
  module that has only literal defaults, and have the consumer call
  the wrapper.
- **Named arguments on imported functions**: cross-module named
  arguments (`foo(b = 5, a = 3)` where `foo` is imported) are not
  supported. The consumer-side stub uses placeholder formal names
  (`p0`, `p1`, ...) because the `.nmod` format does not carry formal
  names. Use positional arguments only when calling imported
  functions.
- **Default parameters on interface methods**: interface method
  declarations (no body) do not have their defaults resolved. Callers
  must supply all arguments.
- **Method inheritance of defaults**: derived class overrides do not
  inherit default values from the base class method. Each override
  declares its own defaults independently.
- **Parameter count ceiling**: functions with more than 64 parameters
  (a sanity ceiling) trigger a compile-time error. The
  frame layout is otherwise dynamic — the call-argument staging area and
  the evaluation scratch area are sized per-function based on actual
  call patterns observed in the body.
- **No control flow in finally bodies**: `break` / `continue` / `return` /
  `throw` inside a `finally` body is a compile error (a finally body must
  not swallow the in-flight control flow or exception).
- **`super()` chains only to the direct parent**: there is no syntax for
  invoking a grandparent constructor directly; each ctor forwards to its
  immediate parent.
- **`new C(args)` when `C` has no constructor silently drops `args`**:
  unlike an explicit `super(args)` (which errors), constructor arguments
  at allocation sites are discarded when the class declares no ctor.
- **No lambda expressions / closures**: only references
  to named functions and methods exist. Bound method references carry
  receiver state and cover the common callback scenarios; lambdas with
  captures are a future direction.
- **`List<Func>.contains` / `indexOf` use identity comparison**: two
  references to the same function are distinct heap records, so a
  freshly created reference never `contains`-matches a stored one.
  Documented inconsistency with `==` (content equality).
- **Cross-module function values are rejected, not transported**:
  referencing an imported function, or passing a function reference to
  an imported function, is a compile error (function signatures stay
  outside the `.nmod` type-descriptor grammar, which carries data types
  only). Lifting this requires extending the grammar to `Func`
  signatures.
