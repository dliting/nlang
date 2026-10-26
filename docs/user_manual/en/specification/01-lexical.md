# 1. Lexical Structure

## 1.1 Source Encoding

A source file MUST be encoded in Unicode Transformation Format (UTF-8).
A leading byte order mark (BOM) MUST be accepted and ignored. Source
text that is not valid UTF-8 is a compile-time error.

## 1.2 Comments

A line comment starts with `//` and ends at the line break. A block
comment starts with `/*` and ends at the first following `*/`;
block comments do not nest and MAY span lines. A comment is
treated as whitespace: it never forms a token and never separates
tokens that would otherwise join (see §1.6 for the one name-then-
`<` rule that spans comments). An unterminated block comment is a
compile-time error.

## 1.3 Identifiers

An identifier starts with a letter or underscore and continues with
letters, digits or underscores. Identifiers are restricted to the
American Standard Code for Information Interchange (ASCII) repertoire
and are case-sensitive. An identifier MUST NOT be a reserved word
(§1.4).

## 1.4 Reserved Words

The following words are reserved and MUST NOT be used as
identifiers:

| Reserved word |
|---|
| `as` |
| `assert` |
| `bool` |
| `break` |
| `byte` |
| `case` |
| `catch` |
| `char` |
| `class` |
| `const` |
| `continue` |
| `default` |
| `do` |
| `double` |
| `else` |
| `elseif` |
| `enum` |
| `false` |
| `finally` |
| `float` |
| `for` |
| `foreach` |
| `func` |
| `if` |
| `implements` |
| `import` |
| `in` |
| `int` |
| `interface` |
| `long` |
| `native` |
| `null` |
| `out` |
| `private` |
| `protected` |
| `public` |
| `return` |
| `short` |
| `state` |
| `static` |
| `string` |
| `struct` |
| `super` |
| `switch` |
| `this` |
| `throw` |
| `true` |
| `try` |
| `ubyte` |
| `uint` |
| `ulong` |
| `ushort` |
| `using` |
| `virtual` |
| `void` |
| `while` |

`func` is both a reserved word and the keyword spelling of the
function type (chapter 4 when written); its type-argument behavior is
specified in §1.6.

## 1.5 Literals

### 1.5.1 Integer literals

An integer literal is decimal (`0`–`9` digits) or hexadecimal
(`0x` or `0X` prefix). Integer literals are unsigned: a leading `-`
is the unary minus operator, not part of the literal. The literal's
type is selected by value: at most 2147483647 it is `int`; at most
9223372036854775807 it is `long`; otherwise it is `ulong`. A value
beyond 18446744073709551615 is a compile-time error.

### 1.5.2 Floating-point literals

A floating-point literal has a fractional form (digits, `.`, at
least one digit — the digits before the dot MAY be absent) or an
exponent form (`digits` `e` or `E` optional sign digits), and
optionally an `f` or `F` suffix. An unsuffixed floating-point
literal has type `double`; an `f`- or `F`-suffixed literal has type
`float`.

### 1.5.3 Character literals

A character literal is one Unicode scalar value between single
quotes, or one escape sequence denoting one scalar value. The
escapes `\n`, `\r`, `\t`, `\'` and `\\` are defined. The escape
`\uXXXX` denotes the Basic Multilingual Plane (BMP) code point with
that number; a surrogate code point written alone is a
compile-time error, and an adjacent high-surrogate `low-surrogate`
pair (`\uD800`–`\uDBFF` followed by `\uDC00`–`\uDFFF`) denotes the
non-BMP scalar value it pairs to. Any other character between the
quotes is a compile-time error.

### 1.5.4 String literals

A string literal is text between double quotes. It MUST NOT
contain a raw line break. The escapes `\n`, `\r`, `\t`, `\"`,
`\\`, `\'`, `\0`, `\a`, `\b`, `\f`, `\v` and `\uXXXX` (with the
surrogate rules of §1.5.3) are defined; any other escape is a
compile-time error, and so is an unterminated string. A string
literal lexes as one token; the interpolation form `${identifier}`
inside a string is interpreted at the expression level (chapter 5).

## 1.6 Operators and Punctuation

The multi-character operators are:

`<<` `<<=` `>>` `>>=` `<=` `>=` `!=` `==` `&&` `||`
`%=` `*=` `+=` `-=` `/=`

Single-character punctuation forms a token of its own. Which token
sequences are legal where is defined by the grammar (chapter 2): a
character sequence that is lexed but appears in no production (such
as `++`) is rejected as a syntax error.

Type-argument brackets: a `<` that follows one of the built-in
generic names `List`, `Dict`, `Channel`, `Future` or the keyword
`func` — with only whitespace or comments in between — MUST be
read as opening a type-argument list rather than as the comparison
operator. Inside an open type-argument list, `>>` and `>>=` close
one nesting level each and the remainder is rescanned, so
`List<List<int>>` and `List<List<int>>=` parse. Any other `<` is
the comparison operator.

Array type suffixes: `[]` denotes a one-dimensional array type, and
`[`, one or more commas, `]` denotes a rectangular array type of
rank equal to the comma count plus one (chapter 4 when written).
