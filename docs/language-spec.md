# NLang Language Specification

## Overview

NLang is a statically-typed scripting language with C-like syntax, designed for
embedding in game engines. It compiles to bytecode executed by a register-based VM.

Key design goals:
- Familiar C-family syntax for low learning curve
- Value semantics for structs, reference semantics for classes (like C#)
- Deterministic memory layout for engine integration
- Mark-sweep garbage collection for class objects

## Types

### Primitive Types

| Type   | Size  | Description          |
|--------|-------|----------------------|
| int    | 4 bytes | 32-bit signed integer |
| float  | 4 bytes | 32-bit IEEE 754 float |
| string | 4 bytes | Reference to string pool entry |

### Composite Types

| Type   | Semantics | Storage          | Description              |
|--------|-----------|------------------|--------------------------|
| enum   | Value     | int32            | Named integer constants  |
| struct | Value     | Heap index (copy-on-assign) | Value-typed aggregate |
| class  | Reference | Heap index       | Reference-typed object   |

### Null

Class-typed variables can be null (represented as heap index 0). Accessing
fields or methods on null throws a runtime error (fail-fast, unlike EN's
safe-null behavior).

## Type Semantics

### Struct: Value Semantics

Structs follow value semantics throughout the language:

- **Assignment**: `s2 = s1` creates a deep copy. `s2` is an independent
  instance — modifying `s2` does not affect `s1`.
- **Parameter passing**: Struct arguments are deep-copied into the callee's
  local frame. The callee operates on its own copy.
- **Return value**: A struct return value is deep-copied to the caller's
  result slot.
- **Class field**: When a struct is a class field, the class owns an
  independent deep copy. Assigning `obj.s = s1` deep-copies `s1` into the
  class's field slot.

**Shallow copy of class references within structs**: When a struct contains a
class-typed field, the class reference (heap index) is copied as-is during
struct copy. Both the original and the copy refer to the same class object on
the heap. This is consistent with C#'s behavior for struct fields of reference
type.

Example:
```
class Inner { public int x; }
struct Wrapper { public Inner ref; }

int main() {
    Inner obj = new Inner();
    obj.x = 10;
    Wrapper a;
    a.ref = obj;
    Wrapper b = a;       // shallow copy: b.ref == a.ref (same object)
    b.ref.x = 99;        // modifies the shared Inner object
    return a.ref.x;      // returns 99, not 10
}
```

### Class: Reference Semantics

Classes follow reference semantics:

- **Assignment**: `obj2 = obj1` copies the reference (heap index). Both
  variables point to the same object.
- **Parameter passing**: Class arguments pass the reference. The callee can
  modify the object's fields, and the caller sees the changes.
- **Return value**: Returns the reference. No copy is made.
- **Struct field**: When a class is a struct field, the struct stores the
  reference (heap index). Struct copy shallow-copies this reference.

### Summary Table

| Operation          | struct          | class           |
|--------------------|-----------------|-----------------|
| Assignment         | Deep copy       | Copy reference  |
| Parameter passing  | Deep copy       | Pass reference  |
| Return value       | Deep copy       | Return reference|
| As class field     | Deep copy owned | Store reference |
| As struct field    | Deep copy owned | Store reference |

## Declarations

### Variable Declaration

```
int x = 5;
float y = 3.14;
string s = "hello";
Color c = Color.Red;       // enum
Point pt;                   // struct (zero-initialized)
Node n = new Node();        // class (heap-allocated)
Node n2;                    // class (null)
```

Struct variables are zero-initialized (all fields = 0). Class variables
default to null.

### Enum Declaration

```
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

Enum values are int32 at runtime. Members can be explicit or auto-incremented.

### Struct Declaration

```
struct Point {
    int x;
    int y;
}
```

Structs can contain:
- Primitive fields (int, float, string)
- Enum fields (stored as int32)
- Struct fields (deep-copied, owned by the containing struct)
- Class fields (reference, shallow-copied)

Structs cannot contain methods. Use classes for behavior.

### Class Declaration

```
class Node {
    public int value;
    public Node next;

    Node(int v) {
        this.value = v;
    }

    public int getValue() {
        return this.value;
    }
}

class SpecialNode : Node {
    public int extra;
}
```

Classes support:
- Fields with access modifiers (public/private/protected)
- Constructors (optional, named after the class)
- Methods (implicit `this` parameter)
- Single inheritance (`class Child : Parent`)
- Virtual methods (`virtual` keyword, dynamic dispatch)
- Method overriding in subclasses

**Inheritance layout**: Object memory layout is `[classIdx, ancestor_fields..., parent_fields..., own_fields...]`.
`classIdx` at slot[0] identifies the runtime class for virtual dispatch.

**Constructor behavior**: Only the direct class's constructor is called.
Ancestor constructors are NOT automatically invoked (NLang has no `super()`
syntax). Fields inherited from ancestors are zero-initialized.

## Expressions

### Arithmetic

```
a + b    a - b    a * b    a / b    a % b
```

Integer division truncates toward zero. Division/modulo by zero throws a
runtime error.

### Comparison

```
a == b   a != b   a < b   a > b   a <= b   a >= b
```

Returns 1 (true) or 0 (false). String equality compares content.

### Logical

```
a && b   a || b   !a
```

Non-zero is truthy. Both operands are evaluated (no short-circuit).

### Member Access

```
obj.field          // field read
obj.field = value  // field write
obj.method(args)   // method call
```

For class objects, `obj` must be non-null (runtime null check).

### Object Creation

```
Node n = new Node();
Node n = new Node(42);
```

Allocates on the heap, calls constructor if present.

### Type Casts

```
int x = 5;
float y = (float)x;
int z = (int)y;
```

Explicit casts between int and float. Implicit widening (int→float) is
allowed in some contexts.

## Statements

### Control Flow

```
if (cond) { ... }
if (cond) { ... } else { ... }

while (cond) { ... }
do { ... } while (cond);
for (init; cond; fini) { ... }

break;
continue;
return;
return expr;
```

### Switch

```
switch (value) {
    case 1: ...
    case 2: ...
    default: ...
}
```

Switch values are int32 (including enum values). Break exits the switch.
Fall-through is supported (no automatic break between cases).

### Null Check

Accessing a field or method on a null class reference throws a runtime error
(fail-fast). This differs from EN's "safe null" behavior (skip + default).

## Functions

```
int add(int a, int b) {
    return a + b;
}
```

- Parameters: int/float/string/enum passed by value, struct passed by value
  (deep copy), class passed by reference
- Return type: int, float, string, enum, struct (deep copy), class (reference)
- Recursion: supported, with a depth limit (default 1000)

## Memory Management

### Heap Layout

All struct and class objects share a single heap (`m_structHeap`). Each heap
slot is a `vector<int32>`:

- **Struct slot**: `[field0, field1, ...]` (no type header)
- **Class slot**: `[classIdx, field0, field1, ...]` (slot[0] = runtime type ID)

Heap index 0 is a sentinel (null/invalid). Valid indices start at 1.

### Garbage Collection

NLang uses a mark-sweep garbage collector for class objects:

1. **Trigger**: GC runs at safepoints when `m_gcPending` is set and heap size
   exceeds the threshold. Safepoints are function entry and loop back-edges.
2. **Mark phase**: Precise scan via `LocalDescriptor` — only slots with
   `typeKind == RTK_Class || RTK_Struct` are scanned. No conservative
   byte-scanning (decoupled from stack frame physical layout). Marking uses
   an iterative worklist (not recursive) to avoid stack overflow on deep
   object chains.
3. **Sweep phase**: Unmarked class objects are freed. Class-owned struct
   fields (value semantics) are freed with their owning class. Class fields
   (reference semantics) are freed independently by GC if unreachable.
4. **Free list**: Swept slots are added to a free list. New allocations
   prioritize reuse of free slots.

**Design decisions** (documented in VmExecutor.h):
- Safepoint-triggered, not allocation-point-triggered (avoids tracking
  tempSlot/tempSlot2 in MarkPhase)
- Precise scan via LocalDescriptor, not conservative byte scan (decouples
  GC from stack frame physical layout)
- `m_slotStructIdx` parallel array for struct type identification (structs
  have no type header; smaller change than adding one)

### Struct Lifetime

Struct objects are not individually garbage-collected. Their lifetime is tied
to their owner:
- Local struct variables: live until the function returns (or the variable is
  reassigned, at which point the old struct's owned nested structs are freed
  by deep-copy logic)
- Class-owned struct fields: freed when the owning class object is swept by GC
- Struct-owned nested structs: freed recursively with their parent

## Exit Code Convention

The process exit code is 8-bit (0-255) on Windows. Test expected values must
not exceed 255. For tests requiring larger computations, use modular arithmetic
or return a derived value that fits in the exit code range.
