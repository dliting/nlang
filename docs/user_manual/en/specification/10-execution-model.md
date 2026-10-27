# 10. Execution Model and Memory Semantics

## 10.1 Value and Reference Semantics

The two value categories of §4.1 govern every boundary at which a
value moves: assignment and initialization (§4.1), argument
passing (§7.2), the return value (§6.6), a subscript read or
write (§5.7), a field read or write (§4.6, §8.1), and a container
store or retrieval (chapter 13).

At a boundary, a value of a value type is copied: after the move,
source and target are independent, and a later change to one is
invisible through the other. A struct copy copies every field; a
field of a reference type copies its reference, so the two struct
values then share that one object (§4.6).

At a boundary, a value of a reference type copies the reference:
source and target denote the same object, and a change made
through one is visible through the other. Every operation that
yields a reference — a cast (§5.5), a call result, a container
read — follows the same rule: it denotes the same object, never a
copy.

A `string` value is a handle to immutable text: a boundary copies
the handle, and immutability makes the sharing unobservable
(§4.4). A scalar primitive, `string` or enumeration value entering
an `Object` position is boxed there — wrapped in a heap object
(§4.9) — and unboxing reads the value back out (§5.5).

Two operations move values differently:

- an `out` argument aliases the caller's variable for the
  duration of the call and writes the final value back on return
  (§7.2);
- binding a method to a function value captures a reference to
  the receiver object, which keeps that object reachable for as
  long as the function value lives (§7.4, §10.2).

## 10.2 Lifetime and Garbage Collection

Objects — class instances, struct records, arrays, function
values and strings — live in an implementation-managed heap. The
language offers no deallocation operation: a program never frees
an object, and reclamation runs no user code (there is no
finalizer or destructor).

An object is *reachable* if it can be found from the roots by
following references: the live slots of active function frames
(parameters, locals, temporaries and staging slots), the fields
of reachable objects, the elements of reachable containers, the
captured receivers of live function values, and references held
by the host. The implementation makes two promises:

- a reachable object is never reclaimed;
- an unreachable object MAY be reclaimed at any time; when
  reclamation happens is *unspecified* (§0.4).

Because reclamation runs no user code and its timing is
unspecified, a program cannot observe when — or whether — an
object is reclaimed. A struct record is owned by its holder (a
local slot, a field, or an element of an array or container) and
becomes unreachable together with it.

What happens when the implementation cannot obtain memory for a
new object is *unspecified*.

## 10.3 Program Entry and Exit

A program has one entry function: an explicit `int main()` or the
`int main()` synthesized from a script body (§3.2). Execution
begins by calling it; it takes no parameters.

The value the entry returns becomes the process exit status. The
implementation passes the entry's 32-bit return value to the host
unchanged; how the host presents it to an observer is host
behavior, not language behavior: a POSIX shell truncates the
value to its low 8 bits (a `return 300;` is observed as `44`),
while cmd, PowerShell and Python observe the full unsigned 32-bit
value (a `return -1;` is observed as `4294967295`). A program
whose exit status other tools inspect should stay within `0` to
`255`.

Reaching the end of the entry function returns the default value
of `int`, so the program exits with status `0` (§6.6). An
uncaught exception ends the program with status `1` (§9.1), and a
host-level run-time error (§7.1) likewise ends the program with
status `1`.

## 10.4 Unspecified Behavior

§0.4 defines *unspecified* and *implementation-defined*. This
section collects the open points of the execution model:

- the garbage collector's algorithm and the timing of
  reclamation (§10.2);
- the behavior on memory exhaustion (§10.2);
- the text of exception messages not fixed by a section (§9.3);
- the form of the host's report for an uncaught exception or a
  host-level error (§9.1, §7.1).

A chapter that leaves a behavior open says so with one of these
terms; elsewhere the behavior is specified.
