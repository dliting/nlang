# 28. The `.npkg` Package Archive

A *package archive* is the distribution form of a build: it packs
the build's members — a unit image of chapter 27 for each package
of the build (§3.9.1), plus the native host libraries the build
ships. A unit member is named by its package path; a native
member by its file name (§28.3). A program archive carries an
*entry record*; a library archive omits one, as the standard
library's thirteen domain packages do (§3.9.4). §28.1 sets the
scope and the format version; §28.2 fixes the container; §28.3
defines the entry record and the member identity; §28.4 fixes the
member checksums; §28.5 states the reading and consumption
contract.

## 28.1 Scope and Version

**Format version.** This chapter describes format version
**1.1**, recorded in the header as major `1`, minor `1`
(§28.2). The reader accepts major `1` with a minor not above
its own — a newer reader reads an older minor, never the
reverse — and rejects every other version (§28.5). A format
change bumps the version and replaces this chapter wholesale;
the specification describes only the current version.

**What this chapter fixes.** The container layout (§28.2), the
entry record and member identity (§28.3), the member checksums
(§28.4) and the reading contract (§28.5).

**What it does not fix.** The unit image stored inside each
member — chapter 27 — and the link-closure semantics that
consume the extracted images (§3.9.3).

**Determinism.** Packing the same set of members twice
produces byte-identical archives: members are sorted by name
and the header records no timestamps.

## 28.2 Container Layout

**Conventions.** As §27.2: little-endian two's-complement
integers and length-prefixed byte strings; the width notation
`u8`, `u16`, `u32` and `u64` is the same.

**Header.** The archive begins with:

| Field | Width | Content |
|---|---|---|
| magic | 8 bytes | `NLANGPKG` |
| version major | u16 | `1` |
| version minor | u16 | `1` |
| package name | u16 length + bytes | the artifact name recorded by the build (the output module name); it names the archive in diagnostics |
| flags | u32 | written `0`; no flag is defined and the reader ignores the field |
| signature algorithm | u16 | `0` = unsigned; the signature block is reserved |
| signature offset | u64 | `0` |
| signature length | u64 | `0` |
| member count | u32 | |
| entry | — | a zero byte marks a library (no entry record); otherwise the entry record: a u16 length and the entry unit's package path, then a u16 length and the entry function's name (§28.3) |

**Member table.** Immediately after the header, one entry per
member, in ascending byte-wise order of the member's name:

| Field | Width | Content |
|---|---|---|
| module path | u16 length + bytes | the member's name: the package path of the unit a unit member holds (§3.9.1), or the file name of a native member (§28.3) |
| kind | u8 | `0` = a unit image; `1` = a native host library |
| offset | u64 | the absolute file offset of the member's image bytes |
| length | u64 | the image byte count |
| checksum | u64 | §28.4 |

**Blobs.** The writer lays the member images out after the
table in table order, each beginning at its recorded offset; a
reader follows the recorded offsets and does not require the
images to be adjacent. A unit member's bytes are a complete unit
image (chapter 27); a native member's bytes are the library file
exactly as the build shipped it.

## 28.3 Entry Record and Member Identity

**Entry record.** A program archive names its entry: the entry
unit's package path and the entry function's name (`main`
today, §3.2, §9.3). A library archive carries the zero marker
instead. Within the archive, only the entry-owning member image
keeps a non-empty entry key (§27.2); the archive's entry record
is the authoritative entry.

**Member identity.** One member per name: the packer rejects a
duplicate name. A unit member's name is the package path of the
unit it holds, so a reference to a package path resolves to
exactly one member. A native member's name is its library file
name — the name the host platform maps the library under — and
no import resolves to it (§28.5).

## 28.4 Member Checksums

A member's checksum is a 64-bit hash of its exact image bytes,
computed by the FNV-1a recurrence: start from the offset basis
14695981039346656037; for each byte, XOR the byte into the hash
and multiply by the prime 1099511628211 (2^40 + 435), all
modulo 2^64. The checksum detects accidental corruption; it is
not a security primitive — the reserved signature block of
§28.2 is the place for one. Extraction verifies it, and a
mismatch is an error naming the archive and the member (§28.5).

## 28.5 Reading and Consumption

A reader validates the container before using it:

- **magic.** Header bytes other than the magic → reject.
- **truncation.** A header or member-table entry that runs
  past the end of the file → reject.
- **version.** A major other than `1`, or a minor above the
  reader's own → reject.
- **member bounds.** A member whose recorded offset or length
  does not lie within the file → reject, naming the member.

Extraction of a member verifies its checksum (§28.4); a module
path that is not a member of the archive is an error.

At compile time, an import resolves against a package's unit
members exactly as §3.9.2 describes: a module path takes the
member of that name, and a wildcard takes the union of matching
members. A native member answers no import — its name is a file
name, not a module path, and the compiler never reads its bytes.

At run time, the loader takes unit member images by module path
as the import closure demands them (§3.9.3). The operating
system cannot map a library straight out of an archive, so
opening a package also extracts each native member to a
per-package directory under the per-user package cache (the
`NLANG_PKG_CACHE_ROOT` environment variable relocates the
root), and the load appends that directory to the native search
path (§7.5) — a loose library beside the executable or on an
earlier layer still outranks the packed copy. A cached copy
whose bytes already match the member is left alone; one that
differs is replaced. A package whose declarations stay outside
a program's closure is therefore extracted but never loaded.
