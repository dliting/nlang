# NLang Example Index

Each example focuses on one topic and compiles and runs on its own. The
three ways to run are described in
`docs/user_manual/en/getting-started/running.md` (nide Help menu → NLang
Getting Started).

Command line (run from the repository root; installer users run from the
installation root, ncc/nvm live under `bin\`):

    ncc build examples/<name>.n -o <name>.ncu
    nvm <name>.ncu

Without PATH set up inside the installation, use `bin\ncc build ...` and
`bin\nvm <name>.ncu`.

`stdlib_io` needs one line of stdin: `nvm stdlib_io.ncu < examples/stdlib_io.stdin`.
The exit code of a successful run is in the "Exit code" column (Windows
keeps it within 0-255).

| Example | Demonstrates | Exit code |
|---|---|---|
| hello.n | smallest program | 42 |
| add.n | functions and return values | 42 |
| class_basic.n | class fields/constructor/methods/toString | 3 |
| inheritance_super.n | extends/super()/override/virtual dispatch | 4 |
| exceptions.n | throw/try/catch/finally/built-in exceptions/user subclass | 5 |
| list_tour.n | `List<T>`/add/contains/indexOf/foreach | 6 |
| dict_tour.n | `Dict<K,V>`/containsKey/keys()/foreach | 7 |
| array_initlist.n | fixed-length arrays/`[...]` initializers/length | 8 |
| interpolation_tour.n | `"${}"` interpolation/implicit toString conversion | 9 |
| cast_operator.n | `as` unboxing and reference downcast (numeric conversions go through implicit assignment) | 10 |
| const_assert.n | const locals/assert/compound assignment | 11 |
| out_params.n | `out` parameters for multiple return values | 12 |
| switch_tour.n | switch/enum branches | 0 |
| enum_methods.n | enum methods and `this` | 0 |
| delegates_tour.n | function/method delegates | 7 |
| aliases_tour.n | type aliases | 0 |
| stdlib_string.n | string methods | 0 |
| stdlib_math.n | standard library `math` package | 0 |
| stdlib_io.n | io console and file content (needs stdin) | 0 |
| stdlib_fs.n | fs paths/directories/metadata | 0 |
| hello_project/ | multi-file project (open the `.nproj` in nide) | 0 |
| libdemo/ | flat search-path library (build the library unit first; consumers `import` it and it loads at run time) | 0 |
| embed_host/ | minimal C++ embedding host (built by CMake; run `embed_host <artifact.ncu|.npkg>`) | 0 |
