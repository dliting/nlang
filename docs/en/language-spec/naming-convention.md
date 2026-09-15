# Naming Convention (Phase 8e-9-pre)


NLang adopts **camelCase** for methods and **PascalCase** for types — a hybrid
of Java-style casing and C#-style accessor naming. This combination provides
visual distinction between types and methods (`MyClass.myMethod()` reads
unambiguously) and aligns with the largest developer audience (Java + JS + C++).

| Category | Style | Examples |
|----------|-------|----------|
| Types (class / struct / enum / interface) | PascalCase | `MyClass`, `List<T>`, `Color` |
| Methods — action / command (side effects or multi-arg) | camelCase, bare verb | `add(x)`, `clear()`, `readInt()`, `run()` |
| Methods — pure accessor (no side effects, no args, returns value) | camelCase with `get`/`set` prefix | `getHashCode()` (reserved for future property feature) |
| Methods — predicate (returns bool) | camelCase, bare word | `equals(o)`, `contains(x)` |
| Free functions | camelCase | `print(s)`, `assert(c)` |
| Variables / parameters / locals | camelCase | `firstName`, `itemCount` |
| Entry-point function `main` | lowercase (sole exception) | `int main()` |
| Enum values | PascalCase | `Color.Red`, `Day.Monday` |
| Generic type parameters | single uppercase letter | `T`, `K`, `V` |
| Private fields | camelCase, no prefix | `class Foo { int count; }` |

**Rationale**:
- PascalCase types + camelCase methods → `MyClass.myMethod()` makes the
  type-vs-method distinction immediate; `MyClass.MyMethod()` is ambiguous.
- Covers Java + JS + C++ conventions (the largest common denominator).
- `main` exception preserves the universal C/C++/Java entry-point convention.
- `getXxx` / `setXxx` prefix retained on accessors: reserves namespace for a
  future property feature (`obj.hashCode` desugaring to `getHashCode()` /
  `setHashCode(v)`). Only `getHashCode` currently uses this form; other
  accessors (`length`, `count`, `position`, `keys`) use bare camelCase and
  may be upgraded to `getXxx` when properties land.
- Predicates do not use `isXxx` / `hasXxx` prefixes — `equals` and
  `contains` are clear on their own.

**Built-in method migration** (Phase 8e-9-pre): all built-ins renamed from
PascalCase to camelCase. Notable: `Length→length`, `Add→add`, `Equals→equals`,
`GetHashCode→getHashCode`, `ReadInt→readInt`, `WriteString→writeString`,
`Keys→keys`, `ContainsKey→containsKey`.
