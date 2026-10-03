# Crash Course: Functions and Modules
### Functions

```nlang
import io;

void divmod(int a, int b, out int q, out int r) {
    q = a / b;
    r = a - q * b;
}

int add(int a, int b = 10) {
    return a + b;
}

int main() {
    int q = 0;
    int r = 0;
    divmod(17, 5, out q, out r);    // q=3, r=2
    io.print("17 = 5*" + q + " + " + r);
    if (add(q) == 13 && r == 2)     // b defaults to 10
        return 13;
    return 1;
}
```

Output `17 = 5*3 + 2`, exit code 13. `out` parameters let one call bring
back several results; parameters can carry default values (named
arguments are supported too); functions can be overloaded, recurse, and
return `void`.

See also: [Language Specification / Functions](../language-spec/functions.md).

### Function values

```nlang
import io;

using Op = Func<int, int>;

int doubleIt(int x) { return x * 2; }
int apply(Op f, int x) { return f(x); }

int main() {
    int a = apply(doubleIt, 21);   // 42 — free function reference
    if (a == 42)
        return 42;
    return 1;
}
```

Output: none; exit code 42. A function can be passed around as a value:
`Func<Ret, Args...>` names a function type, and a free function or a bound
method (`c.foo`) binds to it. Function values are stored in variables,
fields, and collections (`List<Func<int, int>>`, `Func<int, int>[]`).

See also: [Language Specification / Function Types and Delegates](../language-spec/function-types-and-delegates.md).

### Native functions

`calc.n`:

```nlang calc.n
native int natAdd(int a, int b);          // no body — the host implements it

int quad(int x) { return natAdd(x, x); }  // a NLang function can wrap a native
```

Output: none — this snippet shows the declaration side only (it must
compile; actually calling the native needs a host-side implementation).
A `native` function has no body — the host process supplies the
implementation: a free native is keyed `<package>.<name>`, and the
command-line tools locate `nlang_<package>.dll` on the search path by
that package, while an embedding host registers implementations by name.
The standard library's `io.print` is exactly such a native (declared
`native void print(string)` in `stdlib/io.n`, implemented in
`nlang_io.dll`). The call dispatches straight to the implementation with
no NLang frame; NLang-side wrappers like `quad` are ordinary functions.

See also: [Language Specification / Functions](../language-spec/functions.md).

### Modules

Multiple `.n` files in a project form modules by relative path: files in
the same directory see each other naturally; other directories (or
external `.ncu`/`.npkg` modules) require an explicit `import` and
module-path-qualified calls.

`main.n`:

```nlang modules/main.n
import io;
import utils.helper;

int main() {
    io.print(twice(21));            // same-directory helper.n: bare call, no import
    io.print(utils.helper.answer());// subdirectory: module-path-qualified after import
    if (utils.helper.answer() == 42)
        return 42;
    return 1;
}
```

`helper.n` (same directory as main.n):

```nlang modules/helper.n
int twice(int x) { return x * 2; }
```

`utils/helper.n` (module path `utils.helper`):

```nlang modules/utils/helper.n
int answer() { return 42; }
```

After the project file lists the three sources under `Sources`, build
and run (the artifact is a `.npkg` program archive):

    ncc build -p modules.nproj -o modules.npkg
    nvm modules.npkg                # output 42, 42; exit code 42

The standard library packages (`io`/`math`/`fs`) also require an `import`
before use — that is the `import io;` at the top of every snippet on
this page that calls `io.print`; omitting it produces the compile error
`Package 'io' is not imported`.

`import` also supports recursive wildcards: `import utils.*;` imports
`utils/` and all of its nested subdirectories in one go (calls still
spell out the fully qualified `utils.helper.f()`). Repeated imports —
and any mix of wildcard and exact imports — are idempotent with union
semantics.

For the complete visibility matrix (who needs an import, which call
forms are allowed) and all six error messages, see the Import
Declaration section of the Language Specification.

See also: [Language Specification / Declarations](../language-spec/declarations.md),
[Standard Library](../language-spec/standard-library.md).
