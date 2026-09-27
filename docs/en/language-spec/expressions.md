# Expressions


An expression produces a value. NLang is statically typed: every expression
has a type, and the compiler checks each operation against it.

### Expression pages

| Expression                                        | Page |
|---------------------------------------------------|------|
| operators (`+ - * / %`, comparison, logical)      | [Operators](operators.md) |
| casts (`(T)`, `as`, coercion to string)            | [Type Casts](type-casts.md) |
| collection initializers (`[...]`, `new Type{...}`) | [Collection Initializers](collection-initializers.md) |

String interpolation (`${...}`), escape sequences, and string comparison are
string-literal features: see [String](string.md).

### Member access

```nlang
obj.field          // field read
obj.field = value  // field write
obj.method(args)   // method call
```

For class objects, `obj` must be non-null (runtime null check — see
[Class](class.md) "Null check"). A struct has no methods; a field is read or
written through the member expression (see [Struct](struct.md)). Enum members
are constants, not members of a value (see [Enum](enum.md)).

### Object creation

```nlang
Node n = new Node();
Node n = new Node(42);
```

`new` allocates on the heap and calls the constructor if present. See
[Class](class.md) "Constructors". Array creation is `new T[n]` — see
[Array](array.md).
