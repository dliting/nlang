# 7. Functions and Methods

## 7.1 Declaration Forms

The syntax of a function or method declaration is §2.2 at top
level and §2.4 for class, struct, enum and interface members. A
declaration gives a return type or `void`, a name, zero or more
formal parameters, and a body. A method declaration with no return
type names a constructor (chapter 8).

A top-level function is *bare* when it carries no access modifier
and no flags (§2.2). Within its own unit every top-level function
is visible bare. Across units, the bare visibility of §3.6 reaches
only bare functions: a function carrying an access modifier is not
visible bare from another unit, and a bare name for it there is a
compile-time error. The three access modifiers behave alike in
this respect; the qualified name still reaches the function through
the import gate (§3.6, chapter 12). The `static` and `virtual`
flags are accepted at top level and have no effect: they govern
class members (chapter 8). Only `native` carries meaning at top
level (§7.5).

A body written `;` declares a function without a body: a native
function (§7.5) at top level, or an interface member with no
default implementation (chapter 8).

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
and no `catch` clause handles it (chapter 9).

## 7.2 Parameters and Arguments

A formal parameter belongs to the function scope (§3.5) and is
initialized from the corresponding argument at each call. The
initialization follows the value model of chapter 4: a struct
formal receives a copy (§4.6), while a class, interface, array or
function formal refers to the same object as the argument (§4.3,
§4.7, §4.11). The argument must be assignable to the parameter
type (§4.12). Parameters are assignable like local variables
(§6.2).

A formal may declare a default value `T p = e` at any position
(§2.4). The default expression is resolved in the scope of the
declaration and evaluated at the call site, at each call that
omits the argument; it may reference earlier formals of the same
declaration. The value must be assignable to `T` (§4.12). Defaults
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

## 7.3 Overload Resolution

Top-level functions of one bare pool may overload: same name,
different parameter lists (§3.7). Class and struct members overload
their methods and constructors the same way (chapter 8).

At a call site the compiler selects among the candidates whose
parameter count can match the argument list, with defaults filling
the unbound formals (§7.2). A candidate is viable when every
argument is assignable to its parameter by an implicit conversion
(§4.12); an argument that would need a narrowing conversion makes
the candidate not viable. Each viable candidate scores the sum of
the conversion distances of its arguments: an exact match
contributes zero, and each implicit conversion step contributes one
hop. The candidate with the smallest total is selected. A tie
between two or more candidates at the smallest total is an
ambiguity, and a call with no viable candidate is an error: both
are compile-time errors.

A default parameter does not break a tie: a candidate that leaves
a formal to its default scores the same as a candidate that binds
that formal exactly, so `f(1)` against `f(int)` and
`f(int, int = 5)` is ambiguous.

A constructor call resolves among the constructors of the type by
the same distance rule; its arguments are matched positionally, so
named and `out` arguments are not accepted (§7.2).

Overloads of an imported function resolve at the call site against
the imported signatures, exactly as within a unit; the linker
connects the selected overload to its record in the producing unit
(chapter 12).

## 7.4 Function Values and Delegates

The function type `func<R, P1, ..., Pn>` and its identity rule are
§4.11; the reference forms — a free function, a static method
through its type name, an instance method bound to a receiver, and
a virtual or interface method late-bound at each call — are §5.9.

A function reference is formed only where a function type is
expected: in a local declaration, an initializer, an assignment, a
return, an argument, an array element or a brace entry (§5.8).
With no expected type the reference is a compile-time error. The
denoted signature must match the expected function type exactly
(§4.11): no variance, no conversion of the parameters or the
result. A function whose parameters carry defaults cannot be
referenced: defaults are filled only at a direct call (§7.2). Enum
methods and native instance methods cannot be referenced, and a
virtual or interface method with an `out` parameter cannot be
referenced (§7.2).

A bound reference captures its receiver when the value is formed;
state changes made through the reference persist in the receiver
object. A virtual or interface reference dispatches against the
receiver's runtime class at each call (§5.9, chapter 8).

A function value is called with `v(a1, ..., an)` (§5.6); a
function-typed field is called as `obj.cb(a1, ..., an)`. A
function-typed local shadows the function of the same name (§3.7).
Named arguments are not accepted on a call through a function
value (§7.2); `out` arguments write back after the call returns
(§7.2). Calling a null function value is a catchable run-time
error (§5.6); forming a bound reference on a null receiver raises a
catchable run-time error at the formation site.

Two function values compare equal under `==` when they denote the
same function and, for a bound reference, carry equal receivers;
`!=` is the complement, and `== null` is allowed. Ordering
comparisons are compile-time errors. A function value renders
through `toString` (§5.3, chapter 8).

A function value does not enter `Object` (§4.11). It cannot be a
`Dict` key, a `switch` discriminant, or the target of the
serialization methods (chapter 13).

## 7.5 Native Functions

A declaration whose flags include `native` and whose body is `;`
(§2.2, §2.4) has no body: the host supplies the implementation at
run time. A native function otherwise behaves as an ordinary
function: default arguments are filled at the call site before
dispatch (§7.2), including across module boundaries.

A free native function is keyed by `<package>.<name>`, where
`<package>` is the module path of the declaring unit (chapter 12)
and `<name>` is its name. A class-member native keeps its bare
name; for an instance method the receiver is the first host
argument. On the first call the host loads the implementation for
the package; for a free native of a single-segment package the
loader searches for `nlang_<package>.dll` on the host search path.
A unit whose package name has more than one segment may not
declare native functions: a compile-time error. A native function
has no `out` parameters (§7.2).

If the host cannot find the implementation, the call raises a
host-level run-time error (§7.1). The host is responsible for
matching the declared signature: a mismatch between the declaration
and the host implementation is not a type error and yields
unspecified results (§0.4). The calling convention is chapter 23.
