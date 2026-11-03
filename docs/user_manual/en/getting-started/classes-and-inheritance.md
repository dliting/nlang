# Crash Course: Classes and Inheritance

This page tours classes and inheritance through runnable snippets; each section's prose gives the actual output and exit code, ending with a link into the language specification.

### Classes and Constructors

```nlang
import io;

class Animal {
    public string name;

    public Animal(string name) {    // the constructor shares the class name
        this.name = name;
    }

    public virtual int legs() { return 0; }

    public string toString() { return name; }
}

class Dog : Animal {
    public Dog() {
        super("dog");               // call the base constructor
    }

    public int legs() { return 4; } // override
}

int main() {
    Animal a = Dog();           // base-class reference, virtual dispatch
    io.print(a + " has " + a.legs() + " legs");
    if (a.legs() == 4)
        return 4;
    return 1;
}
```

Output `dog has 4 legs`, exit code 4. Classes are reference types; once
`toString()` is overridden, an object participates in string
concatenation directly. The line between reference and value semantics
is drawn in [Specification / Types](../specification/04-types/index.md).

See also: [Specification / Class](../specification/04-types/class.md).

### Static Members

```nlang
import io;

class Registry {
    public static int count = 0;       // one slot on the class itself

    public static void note() {
        Registry.count = Registry.count + 1;
    }
}

int main() {
    Registry.note();                   // first use runs the initializer
    Registry.note();
    io.print("counted " + Registry.count);
    if (Registry.count == 2)
        return 2;
    return 1;
}
```

Output `counted 2`, exit code 2. A `static` field is one storage
slot on the class — every method and every instance addresses the
same slot through the class name. Statics initialize lazily: the
initializer runs on the first use of any static member, exactly
once, no matter how many objects exist. `static const` adds
read-only after initialization. A function body can also declare
`static int last = 0;` — a static local with the same lazy,
once-per-declaration-site behavior.

See also: [Specification / Class](../specification/04-types/class.md),
[Specification / Statements](../specification/06-statements/index.md).

### Interface

```nlang
import io;

interface IShape {
    int Area();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
}

int total(IShape s) {
    return s.Area();              // virtual dispatch
}

int main() {
    Square sq = Square();
    sq.side = 4;                  // set via the concrete type
    IShape shape = sq;             // upcast to the interface
    io.print(total(shape));        // 16
    if (total(shape) == 16)
        return 16;
    return 1;
}
```

Output `16`; exit code 16. An interface declares instance method
signatures — no instance fields, no instance bodies — and may also carry
`public static` methods with bodies and `static const` constants on the
interface type itself. A class conforms with `class X implements IShape`;
a variable of the interface type holds any conforming object, and calls
dispatch virtually to the runtime class. Interface instance members
default to `public` (the opposite of class members); any other modifier
(`private`, `virtual`, ...) is rejected.

See also: [Specification / Interface](../specification/04-types/interface.md).
