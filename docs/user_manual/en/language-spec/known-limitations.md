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
- **Bare init list as a named argument**: positional arguments infer
  their target from the parameter type; named arguments do not — use a
  typed local or the anchored `Type{...}` form.
- **Sized-array declaration: size arm on the first declarator only**:
  `int a, b[3];` is a syntax error — declare `b` separately.
- **Direct-init paren arm on the first declarator only**:
  `Vec a, b(2);` is a syntax error — declare `b` separately (`Vec` on
  this page stands for a class-with-constructor example; the struct
  `Point` is covered in [Struct](struct.md) — structs have no
  constructors).
- **Mixed declarators take the array type**: in `int a[3], b;` the
  sibling `b` is a null `int[]` (array-typed), not an int.
- **Partial initialization is the bracket form at the declaration**:
  `int b[100] = [7, 8];` zero-fills the remainder; the brace form at a
  sized declaration (`int b[100] = {7, 8};`) is a syntax error.
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
  `Vec(2, 3)` in expression position.
- **`using` alias declarations belong to the file prologue**: a `using`
  placed after another top-level declaration is a syntax error.
- **`List<struct>` value semantics**: adding the same struct variable
  to a List twice shares the underlying heap slot (reference semantics
  at the boxing layer). Use separate struct instances for distinct
  elements.
- **Eager materialization cost**: `Point[n]` allocates n+1 heap
  slots at creation (array + one struct per element).
- **Default parameters on interface methods**: defaults on interface
  method declarations are legal and resolve from the call site's static
  type — an interface-typed call with an omitted argument is filled with
  the interface declaration's default (even when the implementing method
  declares a different one); an implementing method's defaults do not
  participate in interface dispatch — when the interface declaration
  carries no default, the call must supply all arguments. Override and
  implement signature comparison ignores default values (a declaration
  with a default and an implementation without one still satisfy the
  contract).
- **Method inheritance of defaults**: derived class overrides do not
  inherit default values from the base class method. Each override
  declares its own defaults independently.
- **Cross-module base classes stay outside override-signature checking**:
  class stubs of imported modules carry no members (with `.ncu`-only
  imports), so subclass override-signature checking is idle in that
  direction; when the imported module's source is present, the check
  applies normally.
- **Members of imported classes are unreachable through `.ncu`
  imports**: the class stub carries no members — field reads (`w.w`)
  and method calls (plain non-virtual ones included, `w.doubled()`)
  fail with not-found/resolution errors, and construction is not wired
  up either (it reports a backend internal error). The class name is
  still usable as a type: declaring a variable of it and receiving
  instances returned by the producing module's own functions both
  work; imports with the source present are unaffected.
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
- **`const`-led top-level functions**: `const int f(){}` does not
  parse, because the `const` entry goes down the same grammar path as
  const statements: the prefix `const int f(` is indistinguishable
  from `const int x =`. `const int x = 5;` works; there is currently no
  legal spelling for a top-level const function (function bodies are
  unaffected).
- **Type-line hijack of the first direct-init argument at top level**:
  when the first argument of `Vec p(base.x);` / `Vec p(a<b);`
  starts with a qualified name or a generic form, the parser reads it
  as a type and reports a clear syntax error. Workaround: assignment
  initialization, `Vec p = Vec(base.x);` (function bodies are
  unaffected).
- **Top-level empty-parameter asymmetry**: `Foo f();` at top level
  fails with a clear syntax error, because the empty-parameter form
  must belong to a function definition: the parser has to choose
  between an empty-param function head and a zero-argument direct
  initialization. Workaround: construct with `Foo f = Foo();`,
  forward-declare with `func Foo f();`. Inside a function body,
  `Foo f();` is a zero-argument direct initialization and keeps
  working.
- **The `implements qualified-name.interface` form is not accepted by
  the grammar** (e.g. `implements lib.IF`): `import` first, then use
  the unqualified name.
- **A class body cannot be empty**: `class B : A { }` is a syntax
  error; declare at least one member.
- **Concurrency: main's return ends the program**: virtual threads still
  parked or queued when `main` returns are dropped silently — there is no
  implicit wait-for-all; establish completion order explicitly with
  `thread.join`. If the run queue drains while main is still unfinished,
  the VM reports a deadlock error. See [Concurrency](concurrency.md).
- **Concurrency: bytecode runs serially under the GIL**: the VM holds a
  global interpreter lock, so exactly one virtual thread executes
  bytecode at a time. Multi-carrier parallelism comes only from that
  lock being released during blocking native calls (typically overlapping
  input/output waits with other virtual threads); CPU-bound virtual
  threads do not run in parallel. The scheduler's budget of about 1000
  back edges (loop iterations) between yield points rotates contexts
  fairly. Set the carrier count with `VmExecutor::SetCarrierThreadCount`
  (embedding hosts, default 1) or `nvm --carriers=N` (tool, at most 4 by
  default).
- **Concurrency: pausing the debugger freezes every context**: a paused
  debugger holds the GIL and every virtual thread freezes with it; `ndb`
  pins a single carrier to keep stepping and breakpoints deterministic.
- **Concurrency: no `await`/`Future`**: `thread.join` is the only waiting
  primitive; `thread.spawn` returns no result value (a spawned context's
  return value cannot be retrieved — pass results through a channel or a
  shared field).
- **Concurrency: the debugger sees the main context only**: debug hooks
  fire for main alone, and the call-stack and locals views reflect
  main's frames; spawned threads execute straight past breakpoints and
  statement steps.
- **Concurrency: a spawned thread's failure surfaces only through
  `join`**: uncaught exceptions propagate to neither the host nor main —
  the failed context is contained, and joiners receive a rethrow of the
  original exception object; with no joiner, the failure is silent.
- **Concurrency: `Channel<T>` is unbounded, `Mutex` is non-reentrant**:
  the channel buffer has no capacity limit (`send` never parks); the
  mutex cannot be reentered — locking it again while holding it parks
  until the deadlock report.
- **Concurrency: no parking inside nested evaluation windows**: inside
  `toString` evaluation and the embedding host's `call()` window,
  `yield`/`sleep`/`join` on an unfinished target/`lock` on a contended
  mutex/`recv` on an empty channel/`spawn` are all rejected with a
  "cannot suspend" runtime error. Blocking native calls are exempt: they
  do not park the virtual thread, they release the GIL for their
  duration, so other carriers keep running.
- **Concurrency: concurrent blocking is capped by the carrier pool**:
  a native call declared blocking under version 5 of the native
  application binary interface (ABI) releases the GIL for its duration,
  so other carriers keep running other virtual threads;
  blocking native calls in flight at once are bounded by the carrier
  pool size, degrading to queued waiting at worst. A third-party native
  that does not declare its blocking section holds the GIL for the whole
  call and every carrier stalls — adding the blocking declaration
  restores the concurrency. Output-redirection callbacks run while
  holding the GIL as well; keep them short.
