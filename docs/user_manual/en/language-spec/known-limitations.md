# Known Limitations


- **User-defined generics**: `class Foo<T> { ... }` is not supported.
  Only built-in generic classes (`List<T>`, `Dict<K,V>`, `Channel<T>`,
  `Future<T>`, `func<...>`) are recognized.
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
- **Bare `{...}` as a method-call receiver**: a bare brace literal needs
  a typed target; as a receiver (`{}.length()`) there is none, so it
  stays rejected — bind a typed local first. A bare bracket literal
  infers its element type from the entries (`[1, 2].length()`). Named
  arguments do infer (`count(items = {1, 2})`). See
  [Collection Initializers](collection-initializers.md).
- **Empty array literal**: the empty literal is `{}` for `List<T>` and
  `Dict<K,V>` targets (`List<T> l = {};`, `Dict<K,V> d = {};`); an array
  target takes no brace form (`int[] a = {};` is rejected — array
  initializers use the bracket form), so an empty array is `int[] a = [];`.
  At a target-less position the empty bracket literal has no element
  type to infer (`[].length()` is rejected).
- **Partial initialization is the bracket form at the declaration**:
  `int b[100] = [7, 8];` zero-fills the remainder; the brace form at a
  sized declaration (`int b[100] = {7, 8};`) is rejected with the named
  diagnostic `array initializers use '[...]'; the brace form '{...}' is
  reserved for collection, struct, and class initializers`.
- **Generic-element fixed arrays are declaration-only**:
  `List<int> a[2];` declares the array; there is no expression-position
  creation twin.
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
- **Instance fields of imported structs are inaccessible across the
  module boundary**: the struct stub carries no fields — reading or
  writing `s.x` on an imported struct is a compile error. Construction
  (`lib.Vec(3, 4);`) and passing the value to the producing module's
  own functions work, and the struct's field defaults still apply at
  allocation.
- **Enums cannot be imported across modules**: an `enum` is not
  exported as a module member — a qualified reference (`lib.Color.Red`)
  reports `Type 'Color' is not a member of package 'lib'.`
- **Cross-module overloads with the same parameter count**: imported
  free functions, interface static methods, and enum methods are
  matched by name and parameter count; two same-name overloads with
  the same parameter count but different parameter types fail at link
  time with an explicit ambiguity error instead of silently
  mislinking.
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
  when the first argument of `Vec p(base.x);` / `Vec p(a<b);` starts with
  an identifier followed by `.` or `<`, the parser reads the identifier
  as a type head (a qualified or generic type) and reports a clear syntax
  error. Only the first argument is affected — call expressions
  (`Vec p(f());`), subscripts (`Vec p(g[0]);`) and every later argument
  parse fine, and function bodies are unaffected. Workaround: assignment
  initialization, `Vec p = Vec(base.x);`.
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
- **Concurrency: multi-carrier scheduling order is unspecified**: a
  single carrier (the library default) is the deterministic tier — the
  same program produces the same interleaving on every run; with
  multiple carriers, wake affinity reorders dequeuing by each context's
  last carrier, so the interleaving is not guaranteed and program
  correctness rests only on the `join`, channel, and mutex
  happens-before edges. See [Concurrency](concurrency.md).
- **Concurrency: a debugger pause is all-stop**: a paused debugger
  holds the GIL and every virtual thread freezes with it; timer
  expirations that fall due during the pause are migrated only after
  the resume (time-frozen semantics), and the frames and locals of
  parked contexts remain inspectable while stopped. `ndb` pins a single
  carrier to keep stepping and breakpoints deterministic; multi-context
  inspection is covered in [ndb](../cli-tools/ndb.md).
- **Concurrency: no `await` keyword**: functions are not async-colored
  (see the model section in [Concurrency](concurrency.md));
  `Future.get()` is an honest blocking wait, and `thread.join` waits on
  a valueless `Thread`.
- **Concurrency: `Future` comes only from `spawn`**: there is no manual
  completion source (you cannot construct an unfinished `Future` in the
  current context and fill it yourself), and no `select`-style
  multi-way wait — for "whichever is ready first", spawn one waiter per
  source and converge through a channel.
- **Concurrency: a spawned thread's failure surfaces only through
  `join`**: uncaught exceptions propagate to neither the host nor main —
  the failed context is contained, and joiners receive a rethrow of the
  original exception object; with no joiner, the failure is silent.
- **Concurrency: `Mutex` is non-reentrant, `Semaphore` waits by
  yielding**: the mutex cannot be reentered — locking it again while
  holding it parks until the deadlock report; the standard-library
  `Semaphore`'s `acquire` takes turns via `thread.yield` while the
  permit count is zero (the kernel has no condition-variable primitive
  yet), so a tight permit supply raises the yield count without
  producing wrong behavior.
- **Concurrency: no parking inside nested evaluation windows**: inside
  `toString` evaluation and the embedding host's `call()` window,
  `yield`/`sleep`/`join` on an unfinished target/`Future.get` on an
  unfinished target/`lock` on a contended mutex/`recv` on an empty
  channel/`send` on a full bounded channel/`spawn` are all rejected with
  a "cannot suspend" runtime error. Blocking native calls are exempt:
  they do not park the virtual thread, they release the GIL for their
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
