# 3. Program Structure and Name Resolution

## 3.1 Compilation Units

A program is compiled from one or more `.n` source files. Each file
is one compilation unit (§0.4): the file itself together with the
library units its imports pull in (chapter 12). All units of one
compilation — the project's own sources and the library sources
inlined through the library search path — are merged into a single
program scope: every top-level declaration joins one namespace
tree, and declarations in different units see each other under the
rules of §3.6.

The body of a compilation unit is ordered (§2.2): first the import
list, then the using directives, then the top-level members. An
`import` or `using` directive after the first top-level member is a
syntax error.

The top-level members are:

- type declarations: `enum`, `struct`, `class` and `interface`
  (chapter 4 and chapter 8);
- top-level functions, bare or modifier-led (§2.2, chapter 7);
- top-level statements (§3.2).

There are no top-level variables. A variable declaration written at
file scope is a top-level statement, not a global declaration, and
follows §3.2.

## 3.2 Program Entry Points

The top-level statements of a compilation unit form its *script
body*. When the unit is compiled as part of a program, the compiler
synthesizes an entry function `int main()` whose body is the script
body in source order; the unit's declarations keep their program
scope as usual. The synthesized entry is an ordinary zero-parameter
function returning `int`.

- It is a compile-time error if a compilation unit contains both a
  script body and an explicit `main()` function: the script body
  would be unreachable inside a second entry, so neither form is
  silently dropped.
- It is a compile-time error if an imported unit contains top-level
  statements: an imported unit contributes declarations only, and
  its statements would otherwise run at an unpredictable point.

A build has at most one entry function. If several units of one
build each supply an entry — an explicit `main()` or a script body —
it is a compile-time error. A build whose units supply no entry
produces a library module with no entry function (chapter 12).

Execution of a program begins by calling its entry function; the
value the entry returns becomes the process exit status
(chapter 10).

## 3.3 Import Directives

An import directive names a *package path*: a dotted name denoting
a module of the program being compiled, a standard library package,
or an external compiled module (chapter 12). The grammar is §2.2;
the two forms are:

```nlang
import utils.helper;      // one module or package
import utils.*;           // wildcard: the whole subtree
```

An import has two effects for the importing compilation unit:

- the imported unit's declarations join the program scope (§3.1);
- the named path is *opened* in the unit's *import gate*, which
  governs qualified access to that path's members (§3.6).

A project unit automatically opens the paths of all project units
in the same directory; no import is needed for qualified access to
them. A library unit inlined through the search path opens nothing
automatically: it sees only what it imports explicitly.

The wildcard form `import p.*;` opens `p` and every module path
below `p`, recursively, among the project's own units. It is a
compile-time error if the wildcard matches no project unit, and a
compile-time error if `p` names a library package — a library
package is a single unit, so `import p;` is its only meaningful
form. The wildcard never reaches external compiled modules, whose
names are single-segment (chapter 12).

An import of a path that resolves to neither a project unit, a
library package nor an external module is a compile-time error.
Importing the same path more than once, from one unit or several,
is idempotent.

## 3.4 Using Directives

A using directive follows the import list and precedes the
top-level members (§3.1). The grammar is §2.2; the two forms are:

```nlang
using Buffer = FileStream;   // alias form
using utils;                 // scope-opening form
```

The alias form `using Name = Type;` binds `Name` as another name
for the named type throughout the compilation unit. Chapter 4
defines type aliases and their expansion. It is a compile-time
error if an alias name is declared twice in one compilation unit,
or collides with a top-level declaration of that unit, a built-in
type name, a built-in class name or a library package name in the
library index. An alias may share its name with a declaration of
another compilation unit: the alias is expanded within its own
unit before the units are merged.

The scope-opening form `using Name;` names a package: if `Name`
resolves to a non-package or resolves to nothing, it is a
compile-time error.

*Informative:* in the current implementation no bare identifier
denotes a package — packages are reachable only through import
paths and qualified names — so the scope-opening form is always
rejected today. The grammar keeps the form for a future proposal
that gives package names a bare spelling.

## 3.5 Scopes

A *scope* is the region of a program in which a name denotes its
declaration. NLang has three kinds of scope:

- the *program scope*: the top-level members of every compilation
  unit of the build (§3.1);
- the *function scope* of each function: its parameters and the
  declarations of its body's outermost block;
- the *block scope* of each braced paragraph (§2.5): the
  declarations written directly in it.

A declaration is visible from its declaration statement to the end
of the enclosing scope; referring to a local name before its
declaration is a compile-time error. A local declaration statement
`T x = e;` binds `x` at the statement and evaluates `e` at that
point in statement order (chapter 6).

Within one function, storage is bound to the name, not to the
declaration: a block that redeclares a name already declared in an
enclosing or preceding sibling block reuses the same storage, and a
write through the inner declaration is observable through the outer
name after the inner block ends. Redeclaring a name with a
different type is accepted; the value observable through the outer
name afterwards is unspecified (§0.4).

*Informative:* block scoping constrains visibility only. A future
proposal may give each block its own storage; until then, the
sharing rule above is observable behavior.

## 3.6 Name Lookup

A *bare identifier* is an identifier written without a qualifying
path. In value position, a bare identifier resolves by a walk from
the innermost scope outwards: the enclosing blocks, then the
function scope, then the program scope. The first binding found
wins; §3.7 governs when an inner binding shadows an outer one.

At program scope, visibility depends on what the name denotes:

- a *top-level function* is visible bare only if it is a built-in
  function or it is declared by a unit in the same directory as the
  referring unit. A function declared in another directory is not
  bare visible; the diagnostic names the module that declares it.
- a *type* (class, struct, enum or interface) declared at top level
  is visible program-wide, whatever directory its unit lives in.
- if the walk finds no binding, a bare identifier may still resolve
  to a built-in class name; §4.9, §4.10, §9.3 and chapter 11
  enumerate the built-in classes.

In type position, the same walk runs restricted to type bindings: a
value binding does not hide a type binding.

A *qualified name* `p.m` reaches the members of the module or
package `p` only through the import gate of the referring
compilation unit (§3.3): the path `p` must be opened, by an
explicit import or by the same-directory rule. Member lookup inside
`p` does not continue into outer scopes. Chapter 12 defines module
naming, path resolution and the gate in full.

Overload selection among same-named functions is defined in
chapter 7.

## 3.7 Shadowing and Redeclaration

An inner binding shadows an outer binding of the same name: within
the inner scope's visibility region, the bare name denotes the
inner declaration. Shadowing changes visibility only; storage
follows §3.5.

- A local declaration may shadow a top-level function name.
- It is a compile-time error if a local declaration reuses a name
  that denotes a type visible at the declaration point.
- Two top-level declarations of the same kind and name in one bare
  pool conflict and are a compile-time error, except that
  top-level functions may overload: same name, different parameter
  lists (chapter 7). The *bare pool* of a unit is the set of units
  whose top-level functions it sees bare (§3.6).
- The same function name or the same type name in different bare
  pools is accepted: the declarations keep separate identities
  under their qualified paths (chapter 12). A function name and a
  type name that collide anywhere in the program conflict,
  whatever pools they live in.
