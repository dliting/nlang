# 26. The json Package

The `json` package reads and writes JSON documents (RFC 8259).
Parsing turns text into a tree of values that the program inspects
through the `JsonValue` base class and its `kind` discriminator; the
`JsonObject` and `JsonArray` builders let a program assemble a document
directly, and `serialize` renders any value back to text. The package
is written in NLang itself — `stdlib/json.n` holds the whole parser and
serializer as readable source — and it declares one exception class of
its own, `JsonException` (§8.3, chapter 20).

## 26.1 A tour

The tour is the shipped example `examples/json_tour.n`; its output
follows the program.

```nlang
// The json package tour: parse, read through kind(), edit the DOM,
// serialize compact and indented, and catch a syntax error. The manual's
// json chapter reproduces this file verbatim as its tour; keep them in
// step when either changes.
import io;
import json;

int main() {
    string text = "{\"books\": [{\"title\": \"The Art\", \"year\": 1999, \"available\": true}, {\"title\": \"Compilers\", \"year\": 2006, \"available\": false}], \"count\": 2}";
    JsonValue doc = json.parse(text);
    JsonObject root = doc.getObject();
    JsonArray books = root.get("books").getArray();
    int i = 0;
    while (i < books.length()) {
        JsonObject book = books.get(i).getObject();
        io.print(book.get("title").getString() + " (" + book.get("year").getInt() + ")");
        i = i + 1;
    }
    // DOM edits keep insertion order: the second book gains a rating and
    // drops its availability flag.
    JsonObject second = books.get(1).getObject();
    second.put("rating", 4);
    second.remove("available");
    io.print(root.serialize());
    io.print(root.serialize(2));
    // A syntax error arrives as a catchable JsonException carrying the
    // byte offset where the document went wrong.
    try {
        json.parse("{\"year\": 199");
    } catch (JsonException e) {
        io.print("rejected: " + e.message);
    }
    io.print("ok");
    return 0;
}
```

```text
The Art (1999)
Compilers (2006)
{"books":[{"title":"The Art","year":1999,"available":true},{"title":"Compilers","year":2006,"rating":4}],"count":2}
{
  "books": [
    {
      "title": "The Art",
      "year": 1999,
      "available": true
    },
    {
      "title": "Compilers",
      "year": 2006,
      "rating": 4
    }
  ],
  "count": 2
}
rejected: json: unexpected end of input at byte offset 12
ok
```

## 26.2 Documents and kinds

A parsed document is a tree of `JsonValue` nodes, one node per JSON
value. Each node answers `kind`, and `kind` says which accessor reads
it:

| `kind()` | the value holds | read it with |
| --- | --- | --- |
| `JsonKind.Null` | the JSON `null` | — |
| `JsonKind.Bool` | `true` or `false` | `getBool` |
| `JsonKind.String` | text | `getString` |
| `JsonKind.Number` | an integer or a decimal number | `getLong`, `getInt`, `getDouble` (§26.4) |
| `JsonKind.Array` | an ordered list of values | `getArray` |
| `JsonKind.Object` | key-value pairs | `getObject` |

Checking `kind` and then calling the matching accessor is the intended
way to consume a document — a document's shape is data, so a program
that walks one asks each node what it is before reading it. An
accessor for the wrong kind raises `JsonException` with the message
`json: expected <kind>, got <kind>`.

| Member of `JsonValue` | Meaning |
| --- | --- |
| `kind()` | which of the six kinds the value holds |
| `getString()`, `getBool()`, `getLong()`, `getInt()`, `getDouble()` | the value as that type |
| `getObject()`, `getArray()` | the value as a `JsonObject` or `JsonArray` (§26.5) |
| `serialize()` | the compact JSON text of the value (§26.7) |
| `serialize(spaces)` | indented text for `spaces > 0`, compact otherwise (§26.7) |
| `toString()` | the same text as `serialize()` (§26.7) |

Two `JsonValue` variables are `==` only when they name the same object;
`==` never compares document structure (§26.6).

## 26.3 Parsing

```nlang
JsonValue doc = json.parse(text);
```

`parse` accepts exactly the grammar of RFC 8259 and nothing more:

- The literals are the lowercase `true`, `false` and `null`; `True`,
  `NULL`, `NaN` and `Infinity` are errors.
- Numbers are JSON numbers: an optional `-`, digits, an optional
  fraction and an optional exponent. There is no leading `+`, no
  hexadecimal, no underscores, no trailing commas, no comments, and no
  single-quoted strings.
- Any JSON value may stand alone at the top level, including a bare
  number, a string or `null`; more text after the top-level value is
  an error.
- A string holds printable American Standard Code for Information Interchange
  (ASCII) characters (`0x20`–`0x7E`) written as-is — the delete character
  `0x7F` passes through as-is too — any of the escape pairs `\"` `\\`
  `\/` `\b` `\f` `\n` `\r` `\t`, a `\uXXXX` escape, or any raw byte at or
  above `0x80` (already Unicode Transformation Format (UTF-8); the package
  neither requires nor forbids any particular non-ASCII sequence). A control
  byte below `0x20` inside a string must be escaped.
- Two `\u` escapes carrying a surrogate pair decode to one astral
  character; a lone surrogate half decodes to the replacement
  character U+FFFD and the rest of the text keeps its meaning.
- Duplicate keys inside one object are accepted: the value written
  later wins, and the key keeps the position of its first occurrence
  in `names` (§26.6).

Every parse failure raises `JsonException` with a message of the form
`json: <what> at byte offset <n>`; the offset is the zero-based index
of the offending byte in the source text, so it survives re-reading
the file:

```
json: unexpected end of input at byte offset 12
json: unexpected data after top-level value at byte offset 4
json: nesting deeper than 200 at byte offset 200
```

## 26.4 Numbers

JSON spells numbers one way; NLang reads them as the two types it
has, and the accessors decide what a number answers:

- A token written without a fraction or exponent is held as a 64-bit
  signed integer (`long`); anything else is held as a `double`.
- `getLong` answers when the number is an exact whole value inside
  `-9223372036854775808 … 9223372036854775807`. A decimal token with
  an integral value answers too — `3e2` reads back as `300`. A
  `double` holding that same whole value answers the same. A
  `JsonException` from `getLong` names what failed: `json: number out
  of long range` for a value outside the range, and `json: number has
  a fractional part` for a value that has a fraction to begin with.
- `getInt` answers the same question inside the 32-bit `int` range,
  raising `json: number out of int range` outside it and the
  fractional-part message above for a value with a fraction.
- `getDouble` answers for every number. An integer token too large
  for `long` comes back as the nearest `double` — above `2^53` a
  `double` cannot name every whole number, so a 30-digit token stays
  exact in its text but rounds through `getDouble`.
- A number parsed from text keeps the exact characters it was written
  with: `1e2`, `0.10`, `-0` and a 30-digit integer all serialize with
  their original spelling. A number the program built from a `long`
  or a `double` prints in the shortest form that reads back to the
  same value (`0.1 + 0.2` prints `0.30000000000000004`).
- `JsonNumber.fromToken(text)` builds a number while preserving the
  supplied JSON spelling. It rejects invalid number tokens such as
  `01`, `+1` and `1e+` with `json: invalid number`; use `JsonNumber(v)`
  when the original spelling does not matter.
- An exponent that pushes a number past `double` range is rejected
  where it is found: `1e999` raises `JsonException` with
  `json: number out of range at byte offset <n>`. A number too small
  to name reads as `0.0`. The grammar accepts both spellings — RFC
  8259 allows them — so the check is the parser's own range rule, not
  a grammar rule.

## 26.5 Building documents

```nlang
JsonObject root = JsonObject();
JsonArray ports = JsonArray();
ports.add(8080);
ports.add(8443);
root.put("name", "nlang");
root.put("ports", ports);
root.put("debug", false);
io.print(root.serialize());
// {"name":"nlang","ports":[8080,8443],"debug":false}
```

| Builder | Members |
| --- | --- |
| `JsonArray()` | `add(v)` — `v` is `JsonValue`, `string`, `int`, `long`, `double` or `bool`; `get(i)`, `set(i, v)`, `removeAt(i)`, `length()` |
| `JsonObject()` | `put(name, v)` — the same six forms of `v`; `get(name)`, `has(name)`, `remove(name)`, `names()`, `count()` |
| `JsonString(s)`, `JsonBool(b)`, `JsonNumber(v)`, `JsonNull()` | one leaf value (`JsonNumber` takes a `long` or a `double`) |

Objects keep insertion order: `names` lists the keys in the order
their values arrived, and a document built this way serializes with
the same ordering it was given. Nothing sorts keys; if an output order
matters, add the keys in that order.

## 26.6 Editing

Containers are editable at any time, before or after a parse.
`set(i, v)` and `removeAt(i)` index from `0` like every NLang
container, and an index outside the container fails the way `List`
fails (§12). On the object side `get(name)` for a key the document
does not carry raises `JsonException` with `json: object has no key
"<name>"`; a program that is not sure asks `has(name)` first:

```nlang
if (root.has("tls")) {
    int port = root.get("tls").getInt();
}
```

A key holding `null` and a key that is absent are different states:
`has` answers `true` for the first and `false` for the second, and
`kind` of the first is `JsonKind.Null`. `remove(name)` answers
whether the key was there; `removeAt(i)` shifts the remaining
elements down.

Two separately parsed copies of one document are not `==` — `==`
names the same object, never the same text or shape (§26.2). For a
structural comparison, compare the serializations.

The tree carries no locking and no read-only views: one context
builds and edits the document, then hands the finished tree to
another context and stops touching it — the same ownership-transfer
discipline NLang expects of every mutable value passed between
threads. Two contexts editing one tree at the same time race on
plain data; keeping them apart is the program's job.

## 26.7 Serializing

`serialize()` renders the compact form: no whitespace beyond what the
strings themselves carry. `serialize(spaces)` with `spaces > 0`
indents instead — every item on its own line, each level deeper by
`spaces` characters, keys separated from their values by `": "` —
and `spaces` of `0` or less renders compact, the same as
`serialize()`. Empty array and object render `[]` and `{}` even in
indented mode, and no form appends a trailing newline. `toString()`
renders exactly what `serialize()` renders, so a value drops into
text directly:

```nlang
string line = "config: " + doc;   // the value serializes into the text
```

Escaping on output is minimal: `"` and `\` take a backslash; bytes
below `0x20` use `\b` `\f` `\n` `\r` `\t` where those exist and
`\u00xx` (lowercase hex) otherwise; every other byte passes through
unchanged, so non-ASCII text stays literal UTF-8.

Two failures end a serialization, both as `JsonException`: nesting
deeper than 200 containers (`json: nesting deeper than 200`) and a
number that is not finite (`json: cannot serialize non-finite double`
— `nan`, `inf` and `-inf` have no JSON spelling). Parsing and
serializing are stable in both directions: text produced by
`serialize` re-parses to a document that serializes back to the same
bytes, and canonical compact text re-serializes unchanged.

## 26.8 Limitations

- `parse` works on a whole document held in a string; there is no
  streaming parse of a file or socket. Read the text first (§14,
  §16).
- The nesting limit is 200 containers in each direction, parse and
  serialize. Documents deeper than that are rare; the limit keeps
  recursion bounded either way.
- Non-`long` numbers (`getDouble`) carry `double` precision: whole
  numbers above `2^53` round. Keep such a number as a string if full
  precision matters; parsed numbers keep their exact source text
  (§26.4).
- Objects order keys by insertion only; there is no sorted-output
  mode (§26.5).
