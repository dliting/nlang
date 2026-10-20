# Overload Resolution


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
distance 0 beats a widening hop, and a narrowing argument rejects the
candidate.

```nlang
class V {
    public V(int a, int b = 7) { }
    public V(float a) { }
}
V a = V(1);     // V(int, int) at 0, ahead of V(float)'s int-to-float hop
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
