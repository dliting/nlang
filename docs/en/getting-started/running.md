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

### 2. The command line

Run from the repository root; installer users run from the installation
root (ncc/nvm live under `bin\`):

    ncc build examples/hello.n -o hello.nmod   # compile
    nvm hello.nmod                             # run (exit code 42)

If PATH is not set up inside the installation, write
`bin\ncc build ...` and `bin\nvm <name>.nmod`.

Multi-file projects pass the .nproj with `-p` and the output location
with `-o`:

    ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
    nvm hello_project.nmod

### 3. Browse by example

`examples/README.md` lists every example with its topic and expected
exit code.
