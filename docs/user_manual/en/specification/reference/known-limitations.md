# Known Limitations

This page collects the language-level limitations visible to user programs, with workable alternatives; see [the VM's known limitations](../../vm-architecture/known-limitations.md) for the virtual-machine side.

### Types, arrays and collections

- **`as` from string to a numeric type is forbidden**: `"5" as int` is a
  compile error (``Invalid cast: `String as Int32` is not allowed``). The
  equivalent capability is covered by the string method family
  (`toInt`/`toLong`/`toFloat`/`toDouble`/`toBool`/`toChar`, see
  [string methods](../library/17-string-methods.md)); this is a deliberate scope
  narrowing, not a defect.

- **Array values in scalar contexts**: an array value has exactly three
  legal destinations — its own array type (the element type must match
  exactly; between bracket spellings the rank must match too, while a
  bracket-free `Array<T>` position admits any rank through the boundary
  check, §4.10.7), `string` targets in **every** position, and `Object`
  (§4.11.1); `null` always converts to an
  array target; comparisons are identity-only; every other use is
  rejected at compile time with an explicit error.

  Details: `string` targets cover whole values and element slots alike
  — string locals and fields, returns, concatenation, string
  parameters (`f(arr)`), `io.print(arr)`, array subscripts
  (`string[] sa; sa[0] = arr`) and container stores (`List<string>`
  subscript and `.add`) — all via runtime toString coercion
  (`"[1, 2]"`); `==` / `!=` work only against another array or `null`.
  The rejection list: scalar, class and interface targets,
  array-operand `as` casts (`ia as int` — an array target like
  `o as int[]` is the legal form, §4.5), non-array parameters, numeric binary
  operands (`arr + 1`), relational or cross-type comparisons
  (`arr < arr2`, `arr == 5`), condition positions (`if (arr)`), and
  `switch` discriminants. Cross-element and
  shared-representation array conversions are rejected alike:
  `string[] b = ia`, the covariant upcast `Base[] ba = da`, and
  `enum[]` ↔ `int[]` interconversion all fail with "an array value
  only converts to the same array type". Elements pulled out of
  `List<T[]>` / `Dict<K,T[]>` follow the same rule; the compiler sees
  that they are arrays — the array face methods work on them
  (`li[0].rank()`), `switch`-discriminant use
  (`switch (li.get(0))`) is rejected with an explicit error,
  `foreach` can iterate them into
  an array-typed loop variable (array-valued sources may be used
  directly and are evaluated once), and `Dict` keyed on array types
  uses handle identity.

- **Jagged arrays (`T[][]`)**: a jagged shape — an array used as the
  element type of another array — is rejected at compile time ("jagged
  arrays (T[][]) are not supported") in the bracket form and in generic
  clothes (`Array<Array<int>>` is the same shape, §4.10.1): at locals,
  fields, parameters, return types, `for`/`foreach` loop variables, and
  as generic type arguments. True multi-dimensional shapes are
  supported (`int[,]`, §4.10); it is the jagged nesting that has no
  layout — use the multi-dimensional form, flat indices, or
  `List<List<T>>`-style containers instead.

- **Bare `{...}` as a method-call receiver**: a bare brace literal needs
  a typed target; as a receiver (`{}.length()`) there is none, so it
  stays rejected — bind a typed local first. Named arguments do infer
  (`count(items = {1, 2})`). See
  [Collection Initializers](../05-expressions/initializers.md).

- **Empty array literal**: the empty literal is `{}` for `List<T>` and
  `Dict<K,V>` targets (`List<T> l = {};`, `Dict<K,V> d = {};`); an array
  target takes no brace form (`int[] a = {};` is rejected — array
  initializers use the bracket form), so an empty array is `int[] a = [];`.

- **Partial initialization is the bracket form at the declaration**:
  `int b[100] = [7, 8];` zero-fills the remainder; the brace form at a
  sized declaration (`int b[100] = {7, 8};`) is a syntax error.

- **Generic-element fixed arrays are declaration-only**:
  `List<int> a[2];` declares the array; there is no expression-position
  counterpart.

- **`using` alias declarations belong to the file prologue**: a `using`
  placed after another top-level declaration is a syntax error.

- **`List<struct>` element matching compares identity**: a struct
  element is stored and retrieved as an independent value copy
  (§4.7.4 — mutating the source after `add` never shows through),
  but `contains` / `indexOf` (and `Dict` keys, §12.2) compare struct
  elements by reference identity, so a struct built with equal fields
  never `contains`-matches a stored one. Find struct elements by
  iterating and comparing fields.

- **Creation cost**: `Point[n]` allocates n+1 heap
  objects at creation (the array itself + one struct per element).

- **`List<func<...>>.contains` / `indexOf` use identity comparison**: two
  references to the same function are distinct heap records, so a
  freshly created reference never `contains`-matches a stored one.
  Documented inconsistency with `==` (content equality).

### Classes, interfaces and modules

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
  module boundary**: an imported struct carries its type name but no field layout — reading
  or writing `s.x` on an imported struct is a compile error. Construction
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

- **`super()` chains only to the direct parent**: there is no syntax for
  invoking a grandparent constructor directly; each ctor forwards to its
  immediate parent.

- **First direct-init argument parsed as a type at top level**:
  when the first argument of `Vec p(base.x);` / `Vec p(a<b);` starts with
  an identifier followed by `.` or `<`, the parser reads the identifier
  as a type (a qualified or generic name) and reports a clear syntax
  error. Only the first argument is affected — call expressions
  (`Vec p(f());`), subscripts (`Vec p(g[0]);`) and every later argument
  parse fine, and function bodies are unaffected. Workaround: assignment
  initialization, `Vec p = Vec(base.x);`.

- **The `implements qualified-name.interface` form is not accepted by
  the grammar** (e.g. `implements lib.IF`): `import` first, then use
  the unqualified name.

- **Types and global fields have no import gate**: function visibility
  follows this file's import set (§3.3), but class, struct and enum
  type names and global fields are reachable by their bare name
  program-wide, whatever package declares them (§3.6). A cross-module
  visibility gate for types and fields is future work; until then a
  program's type names form one shared namespace.

### Functions and statements

- **Parameter count ceiling**: functions with more than 64 parameters
  (a sanity ceiling) trigger a compile-time error. The
  frame size is otherwise dynamic — each function's frame is sized by
  what the body actually uses.

- **No control flow in finally bodies**: `break` / `continue` / `return` /
  `throw` inside a `finally` body is a compile error (a finally body must
  not swallow the in-flight control flow or exception).

- **No lambda expressions / closures**: only references
  to named functions and methods exist. Bound method references carry
  receiver state and cover the common callback scenarios.

- **`const`-led top-level functions**: `const int f(){}` does not
  parse, because the `const` entry goes down the same grammar path as
  const statements: the prefix `const int f(` is indistinguishable
  from `const int x =`. `const int x = 5;` works; there is currently no
  legal spelling for a top-level const function (function bodies are
  unaffected).

### Concurrency

- **Concurrency: main's return ends the program**: virtual threads still
  suspended or queued when `main` returns are dropped silently — there is no
  implicit wait-for-all; establish completion order explicitly with
  `thread.join`. If the run queue drains while main is still unfinished,
  the VM reports a deadlock error. See [Concurrency](../10-concurrency/index.md).

- **Concurrency: storage is locked per data category, same-category
  operations serialize**: heap and string allocation proceeds per
  carrier with no lock, object field reads and writes take no lock,
  while containers (List/Dict), synchronization primitives, streams,
  and string maintenance each hold their own lock — same-category
  operations exclude each other, different categories overlap.
  Pure-compute segments and object field access run in parallel across
  carriers, but a dense loop leaning on containers or string
  maintenance still serializes on its domain lock, and concurrent
  reads and writes of a shared object's fields (shared through
  `spawn` arguments or a channel) are a data race — a transitional
  value may be read; single-value state takes
  [AtomicInt](../library/18-concurrency-classes/atomic.md), multi-field invariants take a
  `Mutex`.
  Each virtual thread runs about 1000 loop iterations before yielding
  once, which keeps the rotation fair. Set the carrier count
  with `VmExecutor::SetCarrierThreadCount` (embedding hosts, default
  1) or `nvm --carriers=N` (tool, at most 4 by default). See [Carriers and
  Parallelism](../10-concurrency/carriers.md).

- **Concurrency: multi-carrier scheduling order is unspecified**: a
  single carrier (the library default) is fully deterministic — the
  same program produces the same interleaving on every run; with
  multiple carriers, the scheduler may take runnable contexts in any
  order (preferring the one it last ran, for cache locality), so the
  interleaving is not guaranteed and program
  correctness rests only on the `join`, channel, and mutex
  happens-before edges. See [Concurrency](../10-concurrency/index.md).

- **Concurrency: a debugger pause is all-stop**: while the debugger
  is stopped, no virtual thread starts executing — an in-flight
  segment finishes and then the carrier stops, so no virtual
  thread makes progress for the rest of the pause; a timer that falls
  due during the pause may already be queued, but everything waits for
  the resume before it starts executing (time-frozen semantics), and
  the call stacks and locals of suspended contexts remain inspectable
  while stopped. `ndb` pins a single carrier to keep stepping and
  breakpoints deterministic; multi-context inspection is covered in
  [ndb](../../cli-tools/ndb.md).

- **Concurrency: no `await` keyword**: functions are not async-colored
  (see the model section in [Concurrency](../10-concurrency/index.md));
  `Future.get()` is an honest blocking wait, and `thread.join` waits on
  a valueless `Thread`.

- **Concurrency: multi-way waiting stays in the `Future` domain**: a
  `Future` can be completed by hand through a `Promise`
  ([Promise](../library/18-concurrency-classes/promise.md)), and `Future.selectAny` provides
  the "whichever decides first" multi-way wait; but channels have no
  "whichever is ready" `recv` variant — to receive from many sources
  whichever comes first, wrap each source with a `Promise` and call
  `selectAny` over the array of `Future` values.

- **Concurrency: a spawned thread's failure never propagates to the
  host or main**: uncaught exceptions are contained in the failed
  context. Waiting sides observe the failure through `join`/`Future.get`
  (a rethrow of the original exception object); non-blocking observation
  is available through `thread.isFailed`/`thread.failure` (see
  [Probing a task's failure](../library/18-concurrency-classes/thread.md#probing-a-tasks-failure)).
  A failure nobody ever waited for or retrieved is not silently lost: at
  run end the VM prints one stderr line per such task (task id, exception
  type and message).

- **Concurrency: `Mutex` is non-reentrant**: the mutex cannot be
  reentered — locking it again while holding it suspends until the
  deadlock report; composed or recursive methods take
  [ReentrantMutex](../library/18-concurrency-classes/reentrant-mutex.md). See
  [Mutex](../library/18-concurrency-classes/mutex.md).

- **Concurrency: suspension inside carrier-less host windows**: inside
  re-entrant evaluation that runs **during a run** — your `toString`
  override called while a collection is formatted for output, a host
  callback firing mid-run — the wait points suspend normally, exactly
  like anywhere else. Only an embedding host's `call()` window
  **outside a run** has no scheduler carrier to serve the wait, so
  there
  `yield`/`sleep`/`join` on an unfinished target/`Future.get` on an
  unfinished target/`lock` on a contended mutex/`acquire` on a
  zero-permit semaphore/`recv` on an empty
  channel/`send` on a full bounded channel/`spawn` are all rejected with
  a "cannot suspend" runtime error. Blocking native calls are exempt:
  the calling virtual thread suspends and releases its locks for the
  wait, so other carriers keep running.

- **Concurrency: a thread-blocking native call is capped by the
  carrier pool**: a native call declared blocking suspends the calling
  virtual thread and releases its locks for the duration, so other
  carriers keep running other virtual threads. A blocking native that
  waits by holding its carrier thread is capped by the pool size,
  degrading to queued waiting at worst; a blocking native that parks
  on a readiness event (`io.parkForIo`, §14.5) hands its carrier back
  for the wait, so in-flight waits are not capped by the pool — every
  `net` socket method parks this way (chapter 21). A native that does
  not declare blocking must not wait under
  the native declaration contract — a third-party native that waits
  anyway occupies only the carrier executing it, and every other
  carrier runs on; adding the blocking declaration lets the calling
  virtual thread suspend and release its locks too. Output-redirection callbacks take no category lock — concurrent
  output serializes through the console mutex, so keep the callbacks
  short to reduce queueing. See [Carriers and
  Parallelism](../10-concurrency/carriers.md).

### Network

- **ICMP (Internet Control Message Protocol) needs platform permission
  setup**: on Linux the ICMP sockets behind `net.ping` and `IcmpSocket`
  are gated by the sysctl
  `net.ipv4.ping_group_range`; unless that range covers the user or
  group (many distributions ship it disabled), opening one raises
  `SocketException` with the platform's text. On Windows `IcmpSocket`
  raw sockets need administrator rights, while `net.ping` needs none.
  See [ICMP](../library/21-network/icmp.md).

- **IPv6 is exercised on loopback only**: the shipped test suite binds
  `::` and connects `[::1]`; the dual-stack path on real IPv6 routes is
  implemented but untested. Every `net` address parameter accepts IPv6
  addresses (the bracket form `"[::1]"` is accepted and stripped).
