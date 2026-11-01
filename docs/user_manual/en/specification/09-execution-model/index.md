# 9. Execution Model

This chapter fixes how values move at the boundaries of the
language, how long objects live, how a program enters and exits,
and which behaviors are left open. Value and reference semantics
(§9.1) and the open points (§9.4) are on this page; object
lifetime and reclamation (§9.2) have their own page, as do program
entry and exit (§9.3).

## 9.1 Value and Reference Semantics

The two value categories of §4.1 govern every boundary at which a
value moves: assignment and initialization (§4.1), argument
passing (§7.2), the return value (§6.6), a subscript read or
write (§5.7), a field read or write (§4.7.4, §4.8.1), and a container
store or retrieval (chapter 13).

At a boundary, a value of a value type is copied: after the move,
source and target are independent, and a later change to one is
invisible through the other. A struct copy copies every field; a
field of a reference type copies its reference, so the two struct
values then share that one object (§4.7).

At a boundary, a value of a reference type copies the reference:
source and target denote the same object, and a change made
through one is visible through the other. Every operation that
yields a reference — a cast (§5.5), a call result, a container
read — follows the same rule: it denotes the same object, never a
copy.

A `string` value is a handle to immutable text: a boundary copies
the handle, and immutability makes the sharing unobservable
(§4.3). A scalar primitive, `string` or enumeration value entering
an `Object` position is boxed there — wrapped in a heap object
(§4.11) — and unboxing reads the value back out (§5.5).

Two operations move values differently:

- an `out` argument aliases the caller's variable for the
  duration of the call and writes the final value back on return
  (§7.2);
- binding a method to a function value captures a reference to
  the receiver object, which keeps that object reachable for as
  long as the function value lives (§7.4, §9.2).

## 9.4 Unspecified Behavior

§0.4 defines *unspecified* and *implementation-defined*. This
section collects the open points of the execution model:

- the garbage collector's algorithm and the timing of
  reclamation (§9.2);
- the behavior on memory exhaustion (§9.2);
- the text of exception messages not fixed by a section (§8.3);
- the form of the host's report for an uncaught exception or a
  host-level error (§8.1, §7.1).

A chapter that leaves a behavior open says so with one of these
terms; elsewhere the behavior is specified.

### Where to look

- **How long an object lives, and what the collector guarantees** —
  [Memory](memory.md).
- **The entry function and the process exit status** — [Program Entry
  and Exit](exit-codes.md).
- **How contexts are scheduled** — chapter 10.
