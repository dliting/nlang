# Compilation Pipeline

Once the compiler front end has produced the abstract syntax tree (AST), `VmBackend` translates
it into independent unit images (`CompiledModule`), one per translation
unit: type and function registration come first, then bytecode
generation for every function. One image per source file; cross-unit
references stay in placeholder slots plus import records for the
load-time linker to resolve. This page lists the stages in order and
explains why one of them (`ResolveStructClassRefs`) has to run as its
own pass.

```text
AST → VmBackend::GenerateUnits → unit image (.ncu), one per translation unit
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout

run time: loader discovers and loads the closure (.ncu / .npkg)
          → linker merges by qualified name → the one runtime module
```

### Whole-tree pre-checks

Before any image is built, two checks span units and are not judgeable
from inside the per-unit loop: an executable unit set with two `main`
candidates is refused, and natives declared across multiple segments are
refused. Either refusal fails the build.

### Per-unit phases (GenerateUnits)

1. **RegisterBuiltinClasses** — root class records for the builtin reference kinds
2. **RegisterStructs** — register struct types, resolve fieldStructIndices
3. **RegisterClasses** — register class types, resolve superClassIdx,
   fieldClassIndices, fieldStructIndices
4. **ResolveStructClassRefs** — resolve struct fieldClassIndices and
   field type descriptors (classes are not yet registered during
   RegisterStructs, so this resolution runs here)
5. **RegisterArrayTypes** — discover and register the array types used by the unit
6. **RegisterEnums** — collect each enum's value-name table
7. **RegisterFunctions** — register function signatures
8. **PopulateClassMethods** — map class methods and constructors to function indices
9. **PopulateStructMethods** — the struct constructor counterpart
10. **RegisterInterfaces** — build the interface method tables; they index the function records just created, so the pass must finish before body emission can append placeholder function records
11. **GenerateAllBytecode** — emit bytecode for all functions

After the loop, entry collection routes the program entry to the one
unit that owns it.

### Why ResolveStructClassRefs is separate

Structs can contain class-typed fields, but `RegisterStructs` runs before
`RegisterClasses`, so the class indices are not assignable yet. The
solution: RegisterStructs keeps a parallel list of the field-type AST
nodes (`m_structFieldTypes`), and the separate pass after
RegisterClasses resolves the class-typed fields through `ClassSlotFor`
— which also allocates placeholder entries for cross-unit field types —
and builds each field's type descriptor once both the struct and class
tables are complete.
