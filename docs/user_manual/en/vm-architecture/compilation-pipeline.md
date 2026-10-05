# Compilation Pipeline

Once the compiler front end has produced the abstract syntax tree (AST), `VmBackend` translates
it into independent unit images (`CompiledModule`), one per translation
unit: struct, class, and function registration come first, then bytecode
generation for every function. One image per source file; cross-unit
references stay in import slots for the load-time linker to resolve.
This page lists the stages in order and explains why one of them
(`ResolveStructClassRefs`) has to run as its own pass.

```text
AST → VmBackend → unit image (.ncu), one per translation unit
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout

run time: loader discovers and loads the closure (.ncu / .npkg)
          → linker merges by qualified name → the one runtime module
```

### Compilation Phases (GenerateStatements)

1. **RegisterStructs** — register struct types, resolve fieldStructIndices
2. **RegisterClasses** — register class types, resolve superClassIdx,
   fieldClassIndices, fieldStructIndices
3. **ResolveStructClassRefs** — resolve struct fieldClassIndices (deferred
   because classes aren't registered during RegisterStructs)
4. **RegisterFunctions** — register function signatures
5. **PopulateClassMethods** — map class methods to function indices
6. **GenerateAllBytecode** — emit bytecode for all functions

### Why ResolveStructClassRefs is separate

Structs can contain class-typed fields, but `RegisterStructs` runs before
`RegisterClasses`. At that point, `FindClass` returns -1 because classes
aren't registered yet. The solution: store type names in `m_structFieldTypeNames`
during RegisterStructs, then resolve class indices in a separate pass after
RegisterClasses.
