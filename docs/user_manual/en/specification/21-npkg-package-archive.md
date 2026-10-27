# 21. The `.npkg` Package Archive

A *package archive* is the distribution form of a build: it
packs one *member* — a unit image of chapter 20 — per package
of the build, each member named by its package path (§12.1). A
program archive carries an *entry record*; a library archive
omits one, and the standard library ships as `stdlib.npkg`
(§12.4). §21.1 sets the scope and the format version; §21.2
fixes the container; §21.3 defines the entry record and the
member identity; §21.4 fixes the member checksums; §21.5
states the reading and consumption contract.

## 21.1 Scope and Version

**Format version.** This chapter describes format version
**1.0**, recorded in the header as major `1`, minor `0`
(§21.2). The reader accepts major `1` with a minor not above
its own — a newer reader reads an older minor, never the
reverse — and rejects every other version (§21.5). A format
change bumps the version and replaces this chapter wholesale;
the specification describes only the current version.

**What this chapter fixes.** The container layout (§21.2), the
entry record and member identity (§21.3), the member checksums
(§21.4) and the reading contract (§21.5).

**What it does not fix.** The unit image stored inside each
member — chapter 20 — and the link-closure semantics that
consume the extracted images (§12.3).

**Determinism.** Packing the same set of members twice
produces byte-identical archives: members are sorted by module
path and the header records no timestamps.

## 21.2 Container Layout

**Conventions.** As §20.2: little-endian two's-complement
integers and length-prefixed byte strings; the width notation
`u8`, `u16`, `u32` and `u64` is the same.

**Header.** The archive begins with:

| Field | Width | Content |
|---|---|---|
| magic | 8 bytes | `NLANGPKG` |
| version major | u16 | `1` |
| version minor | u16 | `0` |
| package name | u16 length + bytes | the artifact name recorded by the build (the output module name); it names the archive in diagnostics |
| flags | u32 | written `0`; no flag is defined and the reader ignores the field |
| signature algorithm | u16 | `0` = unsigned; the signature block is reserved |
| signature offset | u64 | `0` |
| signature length | u64 | `0` |
| member count | u32 | |
| entry | — | a zero byte marks a library (no entry record); otherwise the entry record: a u16 length and the entry unit's package path, then a u16 length and the entry function's name (§21.3) |

**Member table.** Immediately after the header, one entry per
member, in ascending byte-wise module-path order:

| Field | Width | Content |
|---|---|---|
| module path | u16 length + bytes | the package path of the member (§12.1) |
| offset | u64 | the absolute file offset of the member's image bytes |
| length | u64 | the image byte count |
| checksum | u64 | §21.4 |

**Blobs.** After the table, the member images follow in table
order, each beginning at its recorded offset. A member's bytes
are a complete unit image (chapter 20).

## 21.3 Entry Record and Member Identity

**Entry record.** A program archive names its entry: the entry
unit's package path and the entry function's name (`main`
today, §3.2, §10.3). A library archive carries the zero marker
instead. Within the archive, only the entry-owning member image
keeps a non-empty entry key (§20.2); the archive's entry record
is the authoritative entry.

**Member identity.** One member per package path: the packer
rejects a duplicate path. A member's module path is the package
path of the unit it holds, so a reference to a package path
resolves to exactly one member.

## 21.4 Member Checksums

A member's checksum is a 64-bit hash of its exact image bytes,
computed by the FNV-1a recurrence: start from the offset basis
1469598103934665603; for each byte, XOR the byte into the hash
and multiply by the prime 1099511628211 (2^40 + 553771), all
modulo 2^64. The checksum detects accidental corruption; it is
not a security primitive — the reserved signature block of
§21.2 is the place for one. Extraction verifies it, and a
mismatch is an error naming the archive and the member (§21.5).

## 21.5 Reading and Consumption

A reader validates the container before using it:

- **magic.** Header bytes other than the magic → reject.
- **truncation.** A header or member-table entry that runs
  past the end of the file → reject.
- **version.** A major other than `1`, or a minor above the
  reader's own → reject.
- **member bounds.** A member whose recorded offset or length
  does not lie within the file → reject, naming the member.

Extraction of a member verifies its checksum (§21.4); a module
path that is not a member of the archive is an error.

At compile time, importing an archive as an external artifact
(§12.2) takes the member that supplies the declarations: the
entry-record member for a program archive, or the sole member
for a library archive. An archive with several members and no
entry record is refused rather than silently reduced to one
member — the consumer would compile against part of the
surface and miss the rest only at run time.

At run time, the loader extracts member images by module path
as the import closure demands them (§12.3).
