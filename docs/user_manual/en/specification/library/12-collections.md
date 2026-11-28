# 12. Collections

`List<T>`, `Dict<K, V>` and `Set<T>` are the three built-in
container classes: generic classes declared in the `containers`
prelude package (§4.12.1, §4.12.6). The `List` and `Dict` methods
are answered over the built-in run-time channel, while `Set` is
written as source code in the prelude package and its methods
forward to a `Dict` key table (§12.3).
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

Struct elements follow record stabilization (§4.7.4): `set` writes
the value in place into the record that position already owns —
the record's handle never moves, and code that earlier read the
element into a variable never sees the change (the independence of
a by-value copy); every position's record, including a new key's,
is independent of the source from the moment it is established —
a source value and a container element never share a record.

`get` — and with it the subscript sugar — is a **reference-preserving
read**: assigning the result to a value copies it, as any read does,
but binding it to a reference (`int& r = li[2]`, `Point& p = d["k"]`)
aliases that element's storage (§4.7.9). A binding made this way
keeps naming the element through the container's later life:

| Operation after the binding | What the binding names |
| --- | --- |
| `set` onto the same position or key | the same storage — the write is in place, so the binding reads the new value |
| growth (`add`, storing new keys) | the same element — storage never moves on reallocation |
| `removeAt` of an earlier position | the element at its shifted position — the binding follows the element, not the position |
| removal of the bound element or key, or `clear()` | nothing further — the binding detaches, and writes through it no longer reach the container |

The same holds for reads through `get` spelled out (`li.get(2)`,
`d.get("k")`) — the two forms are the same read (§5.7).

## 12.1 List

A `List<T>` (§4.12) is a growable, ordered, index-addressable
sequence of elements of type `T`.

| Method | Signature | Effect | Failure behavior |
| --- | --- | --- | --- |
| `add(value)` | `void add(T value)` | appends `value` | — |
| `get(i)` | `T& get(int i)` | the element at position `i` — a reference-preserving read (see below) | out of range raises `IndexOutOfBoundsException` |
| `set(i, value)` | `void set(int i, T value)` | replaces the element at `i` (a struct value is written in place into the record that position owns, §4.7.4) | out of range raises `IndexOutOfBoundsException` |
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
| `set(key, value)` | `void set(K key, V value)` | stores `value` under `key`; re-storing a key replaces the value in place and keeps the key's position | — |
| `get(key)` | `V& get(K key)` | the value stored under `key` — a reference-preserving read like `List`'s (see §12.1) | no entry for `key` raises `Exception` |
| `getOrDefault(key, defaultValue)` | `V getOrDefault(K key, V defaultValue)` | the value stored under `key`; with no entry, returns `defaultValue` — no entry is created and nothing is raised, in contrast to `get`'s missing-key raise. The result is a copy, not a reference-preserving read | — |
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
The storage rule covers the key side the same way: inserting a new
key gives the entry a private copy of the key record — mutating the
variable used in the insert afterwards never rewrites the stored
entry, and both spellings (`d[k] = v` and `d.set(k, v)`) behave
identically; storing again under an existing key replaces only the
value record and never touches the stored key record.

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

## 12.3 Set

A `Set<T>` (§4.12) holds each element once: `add`ing an
already-equal element changes nothing. Element equality is the
kind-aware equality of §12.1 — the same rule `Dict` uses for its
keys (§12.2): struct elements compare by field value, strings by
content, class and array elements by identity.

`Set<T>` is written as source code in the `containers` prelude
package, and every method forwards to a private `Dict<T, int>` key
table inside it — an element is in the set when it is a key of that
table — so its storage and ordering rules are `Dict`'s key rules
(§12.2); it has no storage semantics of its own.

| Method | Signature | Effect | Failure behavior |
| --- | --- | --- | --- |
| `add(item)` | `bool add(T item)` | appends `item` in insertion order when absent; yields whether an element was added | — |
| `contains(item)` | `bool contains(T item)` | whether `item` is in the set | — |
| `remove(item)` | `bool remove(T item)` | removes `item` when present; yields whether a removal happened | — |
| `count()` | `int count()` | the element count | — |
| `clear()` | `void clear()` | removes every element | — |
| `copy()` | `Set<T> copy()` | a fresh set carrying the same element handles (shallow copy) | — |
| `values()` | `List<T> values()` | a fresh `List<T>` holding every element in insertion order | — |
| `toString()` | `string toString()` | `[e1, e2, ...]` (§11.2) | — |

Three return shapes differ from the matching `Dict` methods, each
on purpose: `add` returns `bool` (`Dict`'s `set` is `void`) because
whether an insertion happened is exactly the question a set
answers; `remove` returns `bool` rather than `Dict`'s `0`/`1`
`int`, because a set stores no value to hand back; and `values()`
answers `Dict.keys()` — likewise a fresh snapshot list in insertion
order — named from the element's point of view, since a set's
members are the elements themselves. `Dict` itself still provides
no `values()` (§12.2).

Iteration goes through the `values()` snapshot:
`foreach (int x in s.values())` (§6.4, §12.2); a `Set` value itself
is not one of the iterable `foreach` sources (§6.4).

With `T` written as `Object`, the boxing rules of `List` apply
(§12.1).

*Informative:* lookup follows `Dict`'s linear scan (§12.2): `add`,
`contains` and `remove` are all O(n), and `values()` copies the
whole key table per call; no O(1) hashtable lookup is provided.
