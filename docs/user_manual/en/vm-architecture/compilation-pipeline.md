# Compilation Pipeline

Once the compiler front end has produced the AST, `VmBackend` translates
it into a `CompiledModule`: struct, class, and function registration
come first, then bytecode generation for every function. This page lists
the stages in order and explains why one of them
(`ResolveStructClassRefs`) has to run as its own pass.

```text
AST → VmBackend → CompiledModule (.nmod)
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout
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
