# Three Ways to Run

### 1. In nide (recommended)

1. Start nide (under `bin\` in the installation) and choose
   File → Open → Project..., then pick
   `examples/hello_project/hello_project.nproj`.
2. Double-click main.n in the Solution tree on the left to open it in
   the editor.
3. Choose Build → Build Project, then Run → Start (Ctrl+F5); the
   program output appears on the Run Output page of the output window.
   F5 instead starts a debug session — see
   [Debugging in nide](debugging.md).

### Configuring nide

Tools → Options opens the settings dialog with two groups:

- **Language**: follow the system language / 中文 / English. A change
  **takes effect after restarting nide** (a restart notice pops up
  immediately on save). The Help menu picks the documentation tree
  for the current language, falling back to the other tree when a
  page is missing.
- **Global build output directory**: when left empty, the default
  location applies — a `nlang-nide` subdirectory of the user's temp
  directory (which is what the input box's placeholder shows; **the
  placeholder is a hint only, and the field value is not expanded as
  environment variables** — do not type a literal `%TEMP%\xxx` into
  the box). Once set it applies from the next build on, with this
  precedence: for projects the `.nproj`'s `outputDir` > this
  directory > the project directory; standalone `.n` files use this
  directory directly.

### 2. The command line

Run from the repository root; installer users run from the installation
root (ncc/nvm live under `bin\`):

    ncc build examples/hello.n -o hello.nmod   # compile
    nvm hello.nmod                             # run (exit code 42)

If PATH is not set up inside the installation, use
`bin\ncc build ...` and `bin\nvm <name>.nmod`.

Multi-file projects pass the .nproj with `-p` and the output location
with `-o`:

    ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
    nvm hello_project.nmod

The complete reference (all flags, default output locations, the ndb
debugger, and ndisasm) is in the
[Command-line Tools](../cli-tools/overview.md) chapter.

### 3. Browse by example

`examples/README.md` lists every example with its topic and expected
exit code.
