# 7.3 Overload Resolution

Top-level functions of one bare pool may overload: same name,
different parameter lists (§3.7). Class and struct members overload
their methods and constructors the same way (§4.7, §4.8).

At a call site the compiler selects among the candidates whose
parameter count can match the argument list, with defaults filling
the unbound formals (§7.2). A candidate is viable when every
argument is assignable to its parameter by an implicit conversion
(§4.4); an argument that would need a narrowing conversion makes
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
(§3.9).

### Overload selection

*Informative:* the distance score decides between the candidates that
survive the viability test; the example below reports the ambiguity.

```nlang
int foo(int a) { return 100; }
int foo(int a, int b = 0) { return 200; }
foo(5, 10);   // OK: second overload (2 args match 2 formals)
foo(5);       // Error: ambiguous (both overloads accept 1 arg)
```

### Constructor overloads

*Informative:* the candidate set is every constructor of the type — a
class contributes its non-static members named after the class, a
struct contributes its declared constructors — and the call binds to
the candidate with the smallest binding distance (a tie is
ambiguous).

```nlang
class Shape {
    public int tag;
    public Shape() { this.tag = 0; }
    public Shape(int r) { this.tag = 10 + r; }
    public Shape(string name) { this.tag = 2; }
}
Shape a = Shape();    // binds Shape()
Shape b(3);           // binds Shape(int)
Shape c = Shape("n"); // binds Shape(string) — a string literal does not
                      // convert to int, so only the string ctor matches
```

Default parameters take part in the distance: an exact type match at
distance 0 beats one level of widening, and a narrowing argument rejects
the candidate.

```nlang
class V {
    public V(int a, int b = 7) { }
    public V(float a) { }
}
V a = V(1);     // V(int, int) at 0, ahead of V(float)'s int-to-float widening
V b = V(1.5f);  // V(float) — a float argument does not narrow to int
```

A trailing default does not break a tie: `W(1)` binds both `W(int)` and
`W(int, int b = 5)` at distance 0, a real ambiguity —

```nlang
class W {
    public W(int a) { }
    public W(int a, int b = 5) { }
}
W w = W(1);   // Error: ambiguous constructor call to class "W":
              //           multiple constructors match the argument list
```

— and when no candidate accepts the argument list:

```nlang
class U {
    public U(int a) { }
}
U u = U("s");   // Error: no constructor of class "U" matches the argument list
```

For a struct the word in the diagnostic is `struct`
(`no constructor of struct "S" matches the argument list`,
`ambiguous constructor call to struct "S": ...`).

### Cross-module overloads

*Informative:* overloads of an imported free function, interface static
method, or enum method resolve exactly as within a unit: the compiler
picks the best match by parameter type at the call site, and the linker
connects that selected overload to its own record in the producing unit.
Two same-name overloads with the same parameter count but different
parameter types therefore link without ambiguity — the call site has
already decided which one runs.

```nlang lib.n
int score(int v) { return v + 1; }
int score(double v) { return 100; }
```

```nlang
import lib;

int a = lib.score(1);     // int overload    -> 2
int b = lib.score(2.5);   // double overload -> 100
```

The compile-time ambiguity rule is unchanged: when an argument converts to
the parameter type of two overloads at equal distance, the resolver reports
the ambiguity at the call site, whether or not the overloads live in another
module.
