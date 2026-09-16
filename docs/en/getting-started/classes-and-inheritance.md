# Crash Course: Classes and Inheritance
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
