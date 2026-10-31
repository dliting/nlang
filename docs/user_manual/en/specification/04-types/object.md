# 4.11 Object and Boxing

## 4.11.1 The root class

`Object` is the root class. Every class that declares no parent
implicitly derives from `Object`; naming `Object` as the parent
explicitly is a compile-time error. A value of type `Object` is a
reference to any class instance — including a `List` or `Dict`
instance — or `null`. `Object` is a reference type: assignment and
parameter passing copy the reference, and the default value is
`null` (§4.8.1).

No other type has a path into `Object`: array, struct, interface
and function values are not assignable to `Object` and not
convertible to it with `as` (§4.4). A scalar primitive, `string` or
enumeration value enters `Object` only through boxing (§4.11.3).

*Informative:* `Object` is the only class without a parent, and the
built-in classes — `List`, `Dict`, the stream classes and
`Exception` with its built-in subclasses — share this one hierarchy
with user classes. Its declaration lives in the prelude package of
the standard library, which every build imports automatically.

## 4.11.2 Protocol methods

`Object` declares the protocol members `equals`, `getHashCode` and
`toString`. The canonical signatures, which an override must match
exactly, are:

| Method | Canonical signature |
|--------|---------------------|
| `equals` | `equals(Object): int` |
| `getHashCode` | `getHashCode(): int` |
| `toString` | `toString(): string` |

The protocol methods are virtual and follow the overriding rules of
§4.8.9: a subclass overrides one by declaring a same-name,
same-signature method, with no keyword. The defaults compare object
identity: `equals` yields `1` when receiver and argument denote the
same object and `0` otherwise, and `getHashCode` yields an `int`
equal for equal objects. `equals` returns `int`, not `bool`, so its
result enters a condition only through an explicit comparison
(`if (a.equals(b) != 0)`); writing `if (a.equals(b))` is a
compile-time error.

```nlang
class Point {
    public int x;
    public int y;
    int getHashCode() {              // overrides Object.getHashCode
        return this.x * 31 + this.y;
    }
}
```

The default `toString()` renders `ClassName@heapLocation` (for
instance `Point@1a`). Both the `as string` conversion and the
implicit conversion at string positions call `toString` (§4.4,
§4.5).

A method whose name and signature match a protocol member is
callable outside the class even when declared `private` or with the
default access: protocol dispatch looks the method up by name and
does not check visibility. The access modifiers are accepted on a
protocol declaration but produce no encapsulation; ordinary members
remain checked (§4.8.2).

`string` carries the protocol with value semantics:
`string.equals(string)` compares content and `string.getHashCode()`
hashes content, which makes strings usable as `Dict` keys without a
wrapper class (§4.3, §4.12.6).

`==` on class references is unaffected by an `equals` override: it
compares identity (§4.8.10), and value equality requires an
explicit `equals` call.

*Informative:* keeping `equals` a separate, opt-in method lets user
classes define value equality without breaking the identity tests
the rest of the codebase relies on. The string protocol calls are
intrinsified to value semantics (content comparison, a content
hash).

## 4.11.3 Boxing

A scalar primitive, `string` or enumeration value entering an
`Object` position is *boxed*: the value is wrapped in a heap object
that the `Object` reference then denotes. Boxing is implicit at the
entry point — an initializer, an assignment or an argument — and
there are no named wrapper types. An enumeration boxes by its
integer value.

```nlang
Object o = 5;            // int boxed
Object f = 3.14;         // double boxed
Object b = true;         // bool boxed
Object s = "hi";         // string boxed

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 boxed at the call site
```

Boxing happens only at the moment a value enters an `Object`
position: an `int` local or arithmetic stays a raw value. `null` is
not boxed — `Object o = null` allocates no heap object; the null
reference is the value 0 itself.

*Informative:* the runtime representation is a tagged boxed slot
(first slot the type tag, second slot the value bits). Boxed slots
hold no references and are skipped by the garbage collection mark
phase.

## 4.11.4 Unboxing and downcasting

Reading a boxed value back out requires the `as` operator: `as`
unboxes a primitive from `Object` and downcasts an `Object` to a
class type (§4.5). An enumeration read back from `Object` yields
the `int` it boxed. The full `as` rules — unboxing, class
downcasts and the conversions that are compile-time errors — are on
[Type Casts](casts.md).
