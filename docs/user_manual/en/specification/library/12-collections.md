# 12. Collections

`List<T>` and `Dict<K, V>` are the two built-in container classes:
generic classes declared in the `containers` prelude package
(§4.12.1, §4.12.6).
This chapter defines their method surface — every method with its
signature, effect and failure behavior — how elements and keys are
matched, and how a container instantiated with `Object` mixes value
types. The type-parameter rules, construction and the subscript
sugar stay with generics (§4.12.2–§4.12.5, §4.12.7).

Calling any method through a `null` container reference raises a
catchable `NullPointerException` with message
`NLang VM: null reference error`, as through any other class
reference (§4.8.10, §8.3). A container store or retrieval moves
values by the categories of §9.1. The subscript forms `li[i]` and
`d[k]` are sugar over `get` and `set` (§5.7, §4.12.7), and
container methods take named arguments under the ordinary rule
(§7.2).

## 12.1 List

A `List<T>` (§4.12) is a growable, ordered, index-addressable
sequence of elements of type `T`.

| Method | Signature | Effect | Failure behavior |
| --- | --- | --- | --- |
| `add(value)` | `void add(T value)` | appends `value` | — |
| `get(i)` | `T get(int i)` | the element at position `i` | out of range raises `IndexOutOfBoundsException` |
| `set(i, value)` | `void set(int i, T value)` | replaces the element at `i` | out of range raises `IndexOutOfBoundsException` |
| `length()` | `int length()` | the element count | — |
| `removeAt(i)` | `void removeAt(int i)` | removes the element at `i`, shifting later elements down | out of range raises `IndexOutOfBoundsException` |
| `indexOf(value)` | `int indexOf(T value)` | the first position matching `value`, `-1` if none | — |
| `contains(value)` | `bool contains(T value)` | whether some position matches `value` | — |
| `copy()` | `List<T> copy()` | a fresh list carrying the same element handles (shallow copy; reading a container field of a const aggregate yields this copy, §4.8.12) | — |
| `clear()` | `void clear()` | removes every element | — |
| `toString()` | `string toString()` | `[e1, e2, ...]` (§11.2) | — |

`indexOf` and `contains` match by kind-aware equality:

- scalar primitive elements compare value bits, with `NaN` not
  equal to itself;
- `string` elements compare content (§4.3);
- `func` elements compare handle content — two references to the
  same function match (§4.5);
- struct elements compare by field value (§4.12.5) — a struct built
  with equal fields `contains`-matches a stored one;
- class and array elements compare identity — a user
  `equals` override is not consulted (§4.11.2).

The same matching decides `Dict` keys (§12.2).

With `T` written as `Object`, a list holds a mix of value types:
primitives and `string` box implicitly at `add`, class instances
store their references, and an element pulled out has static type
`Object` — unbox it with `as` to use it as a concrete type
(§4.11.4). `contains` and `indexOf` compare boxed primitive
elements by value.

```nlang
int main() {
    List<Object> bag = {};
    bag.add(42);                     // int boxed in
    bag.add("hi");                   // string alongside
    int back = bag.get(0) as int;    // 42
    if (back == 42 && bag.contains(42)) {
        return 42;
    }
    return 1;
}
```

## 12.2 Dict

A `Dict<K, V>` (§4.12) maps keys of type `K` to values of type `V`.

| Method | Signature | Effect | Failure behavior |
| --- | --- | --- | --- |
| `set(key, value)` | `void set(K key, V value)` | stores `value` under `key`; re-storing a key replaces the value and keeps the key's position | — |
| `get(key)` | `V get(K key)` | the value stored under `key` | no entry for `key` raises `Exception` |
| `containsKey(key)` | `bool containsKey(K key)` | whether an entry for `key` exists | — |
| `remove(key)` | `int remove(K key)` | removes the entry for `key`; yields `1` if one was removed, `0` if the key was not found | — |
| `clear()` | `void clear()` | removes every entry | — |
| `copy()` | `Dict<K, V> copy()` | a fresh dict carrying the same entry handles (shallow copy; reading a container field of a const aggregate yields this copy, §4.8.12) | — |
| `count()` | `int count()` | the entry count | — |
| `keys()` | `List<K> keys()` | a fresh `List<K>` of the keys in insertion order | — |
| `toString()` | `string toString()` | `{k1: v1, k2: v2, ...}`, in insertion order (§11.2) | — |

Key matching is the kind-aware equality of §12.1. The built-in
containers accept a `struct` type argument, and a struct key
therefore compares by field value (§4.12.5); a `func` key compares
handle content — two references to one function are the same key.

`keys()` is the ordering guarantee for the whole dictionary
surface: the returned list holds every key in insertion order —
storing a value under a key already present does not move the key
(§6.4) — and the list is a copy, so later `set` and `remove` calls
do not affect it. `foreach (K k in dict)` iterates the keys the
same way, through one `keys()` snapshot (§6.8), and `toString`
renders the entries in the same order (§11.2). `values()` is not
provided; iterate the keys and call `get`.

With `K` or `V` written as `Object`, the boxing rules of `List`
apply (§12.1).

*Informative:* every `set`, `get`, `containsKey` and `remove` on a
`Dict` scans the entries linearly — acceptable for typical small
scripts; no O(1) hashtable lookup is provided.
