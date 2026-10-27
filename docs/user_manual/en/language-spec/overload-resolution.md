# Overload Resolution

The normative rules are in [specification §7.3](../specification/07-functions.md).
When multiple overloads exist, the compiler selects the best match by
computing a type-distance score. If two or more overloads match with equal
distance, the call is ambiguous and a compile error is reported.

```nlang
int foo(int a) { return 100; }
int foo(int a, int b = 0) { return 200; }
foo(5, 10);   // OK: second overload (2 args match 2 formals)
foo(5);       // Error: ambiguous (both overloads accept 1 arg)
```

### Constructor overloads

Classes and structs overload constructors the same way they overload
methods: the candidate set is every constructor of the type — a class
contributes its non-static members named after the class, a struct
contributes its declared constructors — and the call binds to the
candidate with the smallest binding distance (a tie is ambiguous).
Arguments are matched positionally, so a constructor call takes no named
or `out` arguments.

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

Overloads of an imported free function, interface static method, or enum
method resolve exactly as within a unit: the compiler picks the best match
by parameter type at the call site, and the linker connects that selected
overload to its own record in the producing unit. Two same-name overloads
with the same parameter count but different parameter types therefore link
without ambiguity — the call site has already decided which one runs.

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
