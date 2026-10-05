# Crash Course: Classes and Inheritance

This page tours classes and inheritance through runnable snippets; each section's prose gives the actual output and exit code, ending with a link into the language specification.

### Classes and Constructors

```nlang
import io;

class Animal {
    public string name;

    public int Animal(string name) {    // the constructor shares the class name
        this.name = name;
        return 0;
    }

    public virtual int legs() { return 0; }

    public string toString() { return name; }
}

class Dog : Animal {
    public int Dog() {
        super("dog");               // call the base constructor
        return 0;
    }

    public int legs() { return 4; } // override
}

int main() {
    Animal a = new Dog();           // base-class reference, virtual dispatch
    io.print(a + " has " + a.legs() + " legs");
    if (a.legs() == 4)
        return 4;
    return 1;
}
```

Output `dog has 4 legs`, exit code 4. Classes are reference types; once
`toString()` is overridden, an object participates in string
concatenation directly.

See also: [Language Specification / Declarations](../language-spec/declarations.md).

### Interface

```nlang
import io;

interface IShape {
    public int Area();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
}

int total(IShape s) {
    return s.Area();              // virtual dispatch
}

int main() {
    Square sq = new Square();
    sq.side = 4;                  // set via the concrete type
    IShape shape = sq;             // upcast to the interface
    io.print(total(shape));        // 16
    if (total(shape) == 16)
        return 16;
    return 1;
}
```

Output `16`; exit code 16. An interface declares method signatures only
(no fields, no bodies). A class conforms with `class X implements IShape`;
a variable of the interface type holds any conforming object, and calls
dispatch virtually to the runtime class. Interface methods must be marked
`public` — the default access is `private`, so an unmarked method is
parsed but not callable.

See also: [Language Specification / Declarations](../language-spec/declarations.md).
