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
