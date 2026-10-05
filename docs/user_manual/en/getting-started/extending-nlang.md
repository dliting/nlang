# Extending NLang

The main extension point in NLang is the library: put reusable functions and types in a directory and it becomes an `import`-able package; add a native implementation when you need the operating system or existing C++ code. How-to in [Developing Libraries](../libraries/developing-libraries.md); the mechanism as a whole in [Library Mechanism](../vm-architecture/library-mechanism.md).

One tip: a library does not have to be installed system-wide — `-I` adds any directory to the import search path, see [ncc](../cli-tools/ncc.md).

Choose between the two shapes as needed: a pure NLang library is source only, ready to modify on arrival; a mixed library adds a native implementation alongside the source.
