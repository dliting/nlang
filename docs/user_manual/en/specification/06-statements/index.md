# 6. Statements

NLang's control-flow statements share the rules this chapter fixes. The
usage detail of `if`, `switch` and `assert` is on
[Control Flow](control-flow.md), the loop forms — `for`, `while`,
`do-while` and `foreach` — are on [Loops](loops.md), and
`try`/`catch`/`throw`/`finally` is on
[Exceptions](../08-exceptions/index.md).

## 6.1 Blocks and Local Declarations

The syntax of statements is §2.5. A paragraph `{ ... }` is a
statement whose declarations have block scope (§3.5); a paragraph
constrains visibility only, and the storage of a redeclared name
follows the sharing rule of §3.5.

A local declaration `T x;` binds `x` to the default value of `T`
(§4). A declaration with an initializer `T x = e;`
evaluates `e` and stores the value into `x`; the value must be
assignable to `T` (§4.4). Binding and evaluation happen at the
declaration statement, in statement order (§3.5). A declaration
with several declarators declares each name in turn; the
evaluation order of the initializers is unspecified (§0.4).

`const T x = e;` requires an initializer. After the declaration
`x` cannot be assigned to, by simple or compound assignment
(§6.2): a compile-time error. The constraint is shallow — storing
into an element of a `const` array, or writing a field through a
`const` class reference, is allowed. `const` applies to local
declarations and to formal parameters; a parameter takes its
value from the argument and declares no initializer. On a field
`const` is a syntax error.

A lone `;` is the empty statement: it does nothing, and an
implementation SHOULD warn that it is ignored. An expression is
not a statement: only a call may stand alone as a statement
(§2.5), so `5;` is a syntax error.

### Const local

```nlang
const int X = 5;
const string Greeting = "hello";
```

Local variables marked `const` must be initialized at declaration and cannot
subsequently be assigned or compound-assigned. Function parameters may
carry `const` too (the callee may not reassign them); `const` on a
class/struct field is a syntax error. See
[Variable Declaration](../03-program-structure/declarations.md) for the general form.

**Const is shallow (Java-`final`-style)**: `const` prevents rebinding the *name*
but does not freeze the referenced object's state. Member mutation through a
const local is allowed:

```nlang
const Foo f = Foo();
f.x = 5;            // OK — f itself is not reassigned
f = Foo();      // ERROR — cannot reassign const local
const Point p = q;
p.x = 5;            // OK — only p's binding is const
```

For class fields and array elements this means: a `const` reference still
permits writing through it. Deep/immutability-style const is not supported.

## 6.2 Assignment

The target of an assignment — the lvalue — is a local variable or
parameter, a field selected by a member expression (§4.7, §4.8),
an array subscript, or a `List` or `Dict` subscript (§5.7).
Parameters are assignable like local variables.

Simple assignment `x = e;` evaluates `e` and stores the value into
the lvalue; the value must be assignable to the type of the lvalue
(§4.4). The store follows the value model of chapter 4: an lvalue
of struct type stores a copy (§4.7), while an array-typed, class or
interface lvalue stores a reference (§4.10, §4.8).

Compound assignment `x op= e;` with `op` one of `+ - * / % << >>`
(§2.5) is equivalent to `x = x op e;` (§5.2, §5.3) except that the
lvalue is evaluated exactly once. Two differences from the binary
operator:

- for `<<=` and `>>=`, the shift count is checked against the
  declared type of the lvalue and the result is stored in that
  type: the lvalue is not promoted (§5.2);
- for a `string` lvalue only `+=` is defined; the other compound
  operators are compile-time errors (§5.3).

A compound assignment whose lvalue is a `Dict` subscript raises a
catchable run-time error at the read of the current value when the
key is absent (§4.12, chapter 8).

Compound assignment is a statement, not an expression: it has no
value, and `(x += 1)` in an expression is a syntax error.
Assignment to a `const` local is a compile-time error (§6.1).

### Compound assignment

```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;  x <<= y ;  x >>= y ;
```

Read-modify-write shorthand for `x = x op y`. Supported left-values: local
variables, class fields (`this.f += y`), struct fields (`pt.x += y`), and
subscript elements (`arr[i] += y`, `li[i] += y`, `d[k] += y`). The
left-value is evaluated **only once**: the receiver of a member left-value
and the base/index of a subscript left-value are not re-evaluated —
`make().x += 5` calls `make()` once, and the read and the write land on the
same object (the expansion `make().x = make().x + 5` would call `make()`
twice and store into a different object); `arr[f()] += 1` calls `f()` once.

The shift pair `<<=` / `>>=` fixes the result type to the left value's
declared type (no promotion to int) and follows the same count-argument
rules as the binary shift — see [Operators](../05-expressions/operators.md) "Shift".

For string elements only `+=` (concat) is legal; the other six operators
are rejected with `operator not supported on string`. A string base is not
supported (`string does not support subscript access`, see [Common
Errors](../reference/common-errors.md) "Assignment targets"). `d[k] += v` throws when
the key is missing (same as reading `d[k]`).

## 6.3 Selection

The condition of an `if` is a `bool` expression (§5.1). The
`else` of an `if` binds to the nearest enclosing `if` that has no
`else` yet (§2.7); an `else if` chain is a nested `if`, and there
is no `elseif` keyword. A local declaration as the unbraced body
of a control-flow statement is a compile-time error: wrap it in a
paragraph (§6.1).

`switch` evaluates its discriminant once. The discriminant type
must be an integer type, a floating-point type, `char`, `string`,
or an enumeration (§4.2, §4.3, §4.6); a `bool`, class, struct,
array, or `null` discriminant is a compile-time error.

The syntax of `switch` is §2.5: case clauses first, then an
optional `default` clause last; a `default` clause in any other
position is a syntax error. A case clause carries one or more
comma-separated labels; each label is an expression. At run time
the labels are evaluated in clause order, top to bottom, and
within a clause left to right; the first label that equals the
discriminant under the equality of §5.2 runs its clause, and the
remaining labels of that clause and of later clauses are not
evaluated. For a floating-point discriminant, `NaN` matches no
label and `−0.0` matches a label of `0.0` (§5.2). A case label's
type must belong to the same family as the discriminant's type;
the families are the integer types with enumerations and `char`,
the floating-point types, and `string`. There is no conversion
between families: an integer label on a floating-point
discriminant is a compile-time error. A `null` label is a
compile-time error. A constant label outside the value range of
the discriminant type is a
compile-time error. Two labels with the same constant value, in
the same clause or in different clauses, are a compile-time
error; non-constant labels may repeat, and the first match wins.

When the statement list of a clause completes, control passes to
the end of the `switch`: execution never falls through from one
clause to the next. A `break` in a clause body exits the `switch`
immediately (§6.4); a `continue` in a clause body applies to the
innermost enclosing loop and is a compile-time error when there is
none.

### Condition typing

`if`/`while`/`do-while`/`for`/`assert` conditions must be **`bool`** —
comparisons and predicates already produce bool (see
[Operators](../05-expressions/operators.md), [Primitive Types](../04-types/primitives.md)). int, string,
float, char, class, struct, and array conditions are compile errors
(`if condition must be bool, not "Int32"`). NLang is a strict-bool
language with no C-style "non-zero is true": `if (1)` and `if (count)`
are both illegal — write `if (count != 0)`; likewise `if (s != "")`,
`if (obj != null)`.

### Unbraced body

A control-flow statement's unbraced (bare) body cannot be a variable
declaration — `if (c) int x = 1;` reports `a local declaration cannot be the
unbraced body of a control-flow statement`. Use a braced body instead.

## 6.4 Iteration

The conditions of `while`, `do-while` and `for` are `bool`
expressions (§5.1). A `while` tests its condition before each
iteration; a `do-while` runs its body once before the first test.
A `for` runs its initialization clause once before the loop, tests
its condition before each iteration, and runs its step clause
after each completion of the body; the condition clause is
required, and the initialization and step clauses may be empty
(§2.5). A declaration in the initialization clause is a local
declaration in the enclosing scope (§6.1). The step clause is
restricted to the forms of §2.5.

`break` terminates the innermost enclosing loop or `switch`
immediately; in a `for`, the step clause does not run. `continue`
jumps to the condition test of the innermost enclosing loop; in a
`for`, the step clause runs first. A `break` outside a loop or
`switch`, and a `continue` outside a loop, are compile-time
errors.

`foreach (T x in src) body` iterates the elements of `src`. The
source must be an array, a `List`, a `Dict` or a `string` (§4.10,
§4.3, §4.12): any other type is a compile-time error. The source
is evaluated exactly once, before the first iteration. The
declared type of the loop variable must match the element type of
the source exactly, including array dimensions (§4.10). The
iteration order is:

- an array: its elements in index order;
- a `List`: its elements in insertion order;
- a `Dict`: its keys in insertion order — storing a value under a
  key already present does not move the key;
- a `string`: its code points in order, and the loop variable must
  be `char` (§4.2).

The loop variable is a local declaration in the enclosing scope
(§6.1): it is visible after the loop, holding the value of the
last element stored, or the default value of its type when the
body never ran. An element of struct type is copied into the
variable at each iteration (§4.7). Mutating the source container
while iterating it is unspecified: the element count is captured
at loop entry (§0.4). A `null` source raises a catchable
null-dereference error (§4.8, chapter 8).

## 6.5 Assertions

`assert(c);` requires a `bool` expression (§5.1). When the value
is `false`, the statement raises a catchable `AssertionException`
whose message reads `assertion failed` (chapter 8). An assertion
is evaluated on every execution of the statement: no build mode
strips it. An uncaught `AssertionException` terminates the program
with exit code 1 (chapter 9).

## 6.6 Return

`return e;` ends the function and yields `e`; `e` must be
assignable to the return type (§4.4). The brace form
`return { ... };` is an initializer whose target is the return
type (§5.8). `return;` is defined only for a `void` function: a
value in a `void` function, and a missing value in a function with
a return type, are compile-time errors.

Reaching the end of a `void` function returns without a value.
Reaching the end of a function with a return type returns the
default value of that type (§4).
