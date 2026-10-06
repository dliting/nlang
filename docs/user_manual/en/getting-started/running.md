# Three Ways to Run

### 1. In nide (recommended)

1. Start nide (under `bin\` in the installation) and choose
   File → Open → Project..., then pick
   `examples/hello_project/hello_project.nproj`.
2. Double-click main.n in the Solution tree on the left to open it in
   the editor.
3. Choose Build → Build Project, then Run → Start (Ctrl+F5); the
   program runs directly in the embedded system terminal on the Run
   Output page of the output window. F5 instead starts a debug
   session — see [Debugging in nide](debugging.md).

The Run Output page is an embedded system terminal: the program runs
in it directly over a pseudo terminal (PTY) — on Windows, a ConPTY
(Windows pseudo console) — so what you see matches running it in a
system terminal by hand, colors and other terminal escape sequences
included. While the program runs, just type to interact: keystrokes
go to the program verbatim and the program side does the echoing;
after it exits, the keyboard is no longer sent. In the terminal:
Ctrl+C copies when a selection exists, otherwise sends the interrupt
signal to the program; Ctrl+V pastes (multi-line text is flattened to
one line); drag selects, and a selection copies; the wheel scrolls
back through the history. Known limits: the scrollback keeps at most
10000 lines, and ligature fonts do not get their ligatures.

### Configuring nide

Tools → Options opens the settings dialog with three settings:

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
- **Toolbar icon size**: small (32×32) / large (48×48). A change
  takes effect immediately — no restart needed.

### Recent

File → Recent remembers the recently opened solutions (.nsln),
projects (.nproj), and files across sessions — at most 10, in
most-recently-used order. Entries are logged when you
open them, create a file, or save a file under a new name.
A rename updates the entry in place. An entry whose file
no longer exists is hidden from the menu but kept in the record
until a newer entry evicts it; entries sharing a file name get an
automatic directory suffix to disambiguate. "Clear Recent List"
empties the whole list at once.

### Project Properties

Project → Project Properties opens the project dialog with four
fields:

- **Name**: the project name, which becomes the `.nproj` file
  name. Must be non-empty and contain no path separators (`/` or
  `\`). Filled in when creating a project; a saved project cannot
  be renamed.
- **Location**: the project root directory (the `.nproj` file's
  directory). Filled in when creating a project; a saved project
  cannot be moved.
- **Output directory**: where the final `.npkg` goes; empty means
  the project directory itself. Consumed by ncc; the precedence
  is the same as the global build output directory in "Configuring
  nide".
- **Intermediate directory**: a reserved field the current
  toolchain does not consume yet — currently unused.

### 2. The command line

Run from the repository root; installer users run from the installation
root (ncc/nvm live under `bin\`):

    ncc build examples/hello.n -o hello.ncu   # compile
    nvm hello.ncu                             # run (exit code 42)

If PATH is not set up inside the installation, use
`bin\ncc build ...` and `bin\nvm <name>.ncu`.

Multi-file projects pass the `.nproj` with `-p` and the output location
with `-o` (the artifact is a `.npkg` program archive):

    ncc build -p examples/hello_project/hello_project.nproj -o hello_project.npkg
    nvm hello_project.npkg

The complete reference (all flags, default output locations, the ndb
debugger, and ndisasm) is in the
[Command-line Tools](../cli-tools/overview.md) chapter.

### 3. Browse by example

`examples/README.md` lists every example with its topic and expected
exit code.
