---
name: Bug report
about: Something compiles wrong, runs wrong, or crashes
title: ''
labels: bug
assignees: ''
---

**What happened?**

A clear description of the wrong behavior (diagnostic text, wrong output,
crash...).

**Minimal reproduction**

The smallest `.n` source (and project layout, if more than one file is
involved):

```n
// paste here
```

The exact commands you ran and the output you got:

```text
ncc build foo.n
nvm foo.nmod
```

**Tool and version**

Which tools are involved — ncc / nvm / ndisasm / nide — and the NLang release
tag or commit you built from (run from a source tree? which compiler?).

**Expected behavior**

What you believe should have happened instead, and where that is documented
(docs page or README section, if it is).

**Environment**

- OS and version:
- How you obtained the tools: release zip / NSIS installer / built from source
