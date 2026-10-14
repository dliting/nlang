# Known Limitations


- **User-defined generics**: `class Foo<T> { ... }` is not supported. Only
  built-in generic classes (`List<T>`, `Dict<K,V>`) are recognized.
- **`as` from string to a numeric type is forbidden**: `"5" as int` is a
  compile error (``Invalid cast: `String as Int32` is not allowed``). The
  equivalent capability is covered by the string method family
  (`toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar`, see
  [string methods](stdlib-string.md)); this is a deliberate scope
  narrowing, not a defect.
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
  `null`. Every other use is rejected at compile time with an explicit
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
  are rejected with an explicit error, `foreach` can iterate them into an array-typed
  loop variable (array-valued sources may be used directly and are
  evaluated once), and `Dict` keyed on array types uses handle
  identity.
- **Jagged arrays (`T[][]`)**: multi-dimensional array declarations are
  rejected at compile time ("jagged arrays (T[][]) are not supported") —
  at locals, fields, parameters, return types, `for`/`foreach` loop
  variables, and as generic type arguments. The VM has no
  multi-dimensional array layout; declare flat arrays or use
  `List<List<T>>`-style containers instead.
- **Bare `{...}` at non-inference positions**: a bare brace literal
  needs a typed target — as a method-call receiver (`{}.length()`) or a
  named argument it does not infer; bind a typed local first. See
  [Collection Initializers](collection-initializers.md).
- **Bare `[]` empty init**: the empty literal is `{}` (`List<T> l = {};`,
  `Dict<K,V> d = {};`, `int[] a = {};`); a sized empty array is
  `int[0]`. The lexer tokenizes `[]` as a single token used by the
  array-type suffix rule, so bare `[]` is not the empty form.
- **Right-shift operator (`>>`, `<<`)**: not implemented (no grammar
  production, no opcode). `>>` outside generic context is a compile
  error; inside generic closing position it is split into `'>'` tokens
  (see Nested generics under `List<T>` in
  [Built-in Generic Classes](builtin-generic-classes.md)).
- **Bare init list as a named argument**: positional arguments infer
  their target from the parameter type; named arguments do not — use a
  typed local or the anchored `Type{...}` form.
- **Sized-array declaration: size arm on the first declarator only**:
  `int a, b[3];` is a syntax error — declare `b` separately.
- **Direct-init paren arm on the first declarator only**:
  `Point a, b(2);` is a syntax error — declare `b` separately.
- **Mixed declarators take the array type**: in `int a[3], b;` the
  sibling `b` is a null `int[]` (array-typed), not an int.
- **Partial initialization is the bracket form at the declaration**:
  `int b[100] = [7, 8];` zero-fills the remainder; the brace form at a
  sized declaration (`int b[100] = {7, 8};`) is a syntax error.
- **Constructor calls do not apply default parameter values**:
  `Point(5)` against `Point(int a, int b = 3)` reports an arity error.
- **Generic instantiation has no anchored head**: `List<int>{60}` does
  not parse — give the literal a typed declaration (see
  [Collection Initializers](collection-initializers.md)).
- **Nested bare braces in a brace literal do not parse**: use bracket
  nesting (`[[1,2],[3,4]]`).
- **Bare list in an array-return position is rejected**: `return [1, 2];`
  from an `int[]` function is a type error (container returns infer).
- **Bare collection literals are not foreach sources**: `foreach (int x in
  [1, 2, 3])` fails type inference ("Collection initializer requires an
  explicit type or an LHS context to infer the target type."); the brace
  form is a syntax error. Bind the literal to a typed local first.
- **Generic-element fixed arrays are declaration-only**:
  `List<int> a[2];` declares the array; there is no expression-position
  creation twin.
- **Bare same-class-name call inside a class body is not a constructor
  call**: it reports a method-needs-receiver style error; construct via
  `Point(2, 3)` in expression position.
- **`using` alias declarations belong to the file prologue**: a `using`
  placed after another top-level declaration is a syntax error.
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
- **Eager materialization cost**: `Point[n]` allocates n+1 heap
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
  (`p0`, `p1`, ...) because the `.ncu` format does not carry formal
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
- **No lambda expressions / closures**: only references
  to named functions and methods exist. Bound method references carry
  receiver state and cover the common callback scenarios.
- **`List<func<...>>.contains` / `indexOf` use identity comparison**: two
  references to the same function are distinct heap records, so a
  freshly created reference never `contains`-matches a stored one.
  Documented inconsistency with `==` (content equality).
- **Cross-module function values are rejected, not transported**:
  referencing an imported function, or passing a function reference to
  an imported function, is a compile error (function signatures stay
  outside the `.ncu` type-descriptor grammar, which carries data types
  only). Lifting this requires extending the grammar to `func`
  signatures.
- **`const`-led top-level functions**: `const int f(){}` does not
  parse, because the `const` entry goes down the same grammar path as
  const statements: the prefix `const int f(` is indistinguishable
  from `const int x =`. `const int x = 5;` works; there is currently no
  legal spelling for a top-level const function (function bodies are
  unaffected).
- **Type-line hijack of the first direct-init argument at top level**:
  when the first argument of `Point p(base.x);` / `Point p(a<b);`
  starts with a qualified name or a generic form, the parser reads it
  as a type and reports a clear syntax error. Workaround: assignment
  initialization, `Point p = Point(base.x);` (function bodies are
  unaffected).
- **Top-level empty-parameter asymmetry**: `Foo f();` at top level
  fails with a clear syntax error, because the empty-parameter form
  must belong to a function definition: the parser has to choose
  between an empty-param function head and a zero-argument direct
  initialization. Workaround: construct with `Foo f = Foo();`,
  forward-declare with `func Foo f();`. Inside a function body,
  `Foo f();` is a zero-argument direct initialization and keeps
  working.
