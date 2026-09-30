# Your First Program in Five Minutes

```nlang
import io;

int main() {
    io.print("hello, NLang");
    return 0;
}
```

`main` is the entry function and its return value is the process exit
code; `io.print` writes one line of text to standard output.

### Path A: in nide

1. Start nide and choose File → New → File..., type the code above, and
   save it as `hello.n`. (Alternatively File → Open → Project... and
   pick `examples/hello_project/hello_project.nproj`.)
2. Choose Build → Build Project; compiler diagnostics appear on the
   Compile Output page of the output window.
3. Choose Run → Start (Ctrl+F5); the program output appears on the Run
   Output page and should show `hello, NLang`.

A project is optional: a standalone `.n` file opened via File → Open
builds and runs too. nide places its build output under
`%TEMP%\nlang-nide\`, and a run after the source changes rebuilds
automatically.

### Path B: the command line

Run in the directory that holds the source (installer users use
`bin\ncc`, `bin\nvm`):

    ncc build hello.n -o hello.nmod    # compile
    nvm hello.nmod                     # run, exit code 0

- `.nmod` is the compiled bytecode module, executed directly by nvm —
  deployment does not need to carry the sources.
- The process exit code is `main`'s return value: view it with
  `echo %ERRORLEVEL%` in cmd or `$LASTEXITCODE` in PowerShell.
- `ncc hello.n` compiles and immediately executes in one step; without
  `-o` the `.nmod` is written to the current working directory (see the
  FAQ below).
