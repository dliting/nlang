# Known Limitations


- **User-defined generics**: `class Foo<T> { ... }` is not supported. Only
  built-in generic classes (`List<T>`, `Dict<K,V>`) are recognized.
- **Array values inside generic containers** (Phase 12 residual):
  `List<T[]>` / `Dict<K,T[]>` store arrays as erased references — pulling
  one out (`li[0]`, `li.get(0)`) yields an expression whose array-ness is
  invisible to the compiler's array gates. Calling a method on it
  (`li[0].rank()`) or switching on it (`switch (li[0])`) compiles but
  misbehaves at run time (the array's heap index is used as the value).
  Workaround: pull it into a typed local first (`Color[] a = li[0];`),
  which restores the array gates. Direct array-typed shapes
  (`a.rank()`, `switch (arr)`) are rejected at compile time.
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
- **Bare init list as function argument**: requires `new Type{...}`
  explicit form. Phase 8e-6 overload uniqueness (Phase G) deferred.
- **`List<struct>` value semantics**: adding the same struct variable
  to a List twice shares the underlying heap slot (reference semantics
  at the boxing layer). Use separate struct instances for distinct
  elements.
- **`Dict<K,V>` with interface type**: interface types are not
  supported as generic type arguments. Use concrete class types.
- **Nested-subscript receiver write** (Phase 9d-3 leftover): in
  `matrix[i][0].x = v` (array-of-array-of-struct), the receiver's
  inner subscript index evaluation can clobber the RHS temp slot.
  Single-level `arr[i].field = v` works correctly. Fix deferred to the
  array redesign.
- **Eager materialization cost**: `new Point[n]` allocates n+1 heap
  slots at creation (array + one struct per element). Cost revisited at
  the array redesign.
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
  (`kMaxFuncParams` sanity ceiling) trigger a compile-time error. The
  frame layout is otherwise dynamic — callParamBase and evalArea are
  sized per-function based on actual call patterns observed in the body.
- **No control flow in finally bodies**: `break` / `continue` / `return` /
  `throw` inside a `finally` body is a compile error (a finally body must
  not swallow the in-flight control flow or exception).
- **`super()` chains only to the direct parent**: there is no syntax for
  invoking a grandparent constructor directly; each ctor forwards to its
  immediate parent.
- **`new C(args)` when `C` has no constructor silently drops `args`**:
  unlike an explicit `super(args)` (which errors), constructor arguments
  at allocation sites are discarded when the class declares no ctor.
- **No lambda expressions / closures** (Phase 13 scope): only references
  to named functions and methods exist. Bound method references carry
  receiver state and cover the common callback scenarios; lambdas with
  captures are a future direction.
- **`List<Func>.contains` / `indexOf` use identity comparison**: two
  references to the same function are distinct heap records, so a
  freshly created reference never `contains`-matches a stored one.
  Documented inconsistency with `==` (content equality).
- **Cross-module function values are rejected, not transported**:
  referencing an imported function, or passing a function reference to
  an imported function, is a compile error (`.nmod` does not serialize
  parameter signatures). Lifting this requires a signature table in the
  module format.
