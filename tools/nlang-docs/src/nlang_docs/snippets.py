"""Runnable-snippet audit over a markdown page.

Extracts the page's ```nlang fences, materializes each one as a real
source file, compiles it with ncc and runs it with nvm, and compares the
process exit code against what the snippet itself promises. This is the
regression pin for the whole manual: a language change that breaks a
documented snippet (or an edit that drifts the prose's claimed
diagnostic) fails the docs pipeline.

Conventions the derivation relies on (documented here, not guessed at
runtime):
 - a snippet "has main" when it contains `main(`; such a block is a
   runnable program and its exit-code promise is derived from the
   `return <int>;` statements in main's body;
 - with a single literal return that literal is the promise; with
   several (the guide's self-check pattern: `if (ok) return FEATURED;
   return 1;`) the promise is the maximum — the failure sentinel 1 must
   never dominate a featured code;
 - no return in main promises 0; a negative literal names no process
   exit code and is refused; a return whose value is not a literal (a
   computed result, e.g. `return Util.twice(21) + 7;`) promises no
   exit code — the audit still runs the program and requires a clean
   run (nvm reports every runtime failure on stderr);
 - a program whose sources declare a `native` function needs an
   embedding host at run time, so plain nvm cannot run it: the audit
   stops at a successful compile;
 - the error-example convention: a ```text fence whose first non-blank
   line starts with "Error" quotes a compiler diagnostic; it attaches
   to the maximal run of immediately preceding consecutive ```nlang
   fences (prose and blank lines may separate them, any other fence
   breaks the run). That run must fail to compile and every non-blank
   quote line must appear verbatim in ncc's stderr — the manual's
   quoted diagnostics stay pinned to the real compiler output;
 - blocks without main are not runnable: a ```nlang <path> label joins
   them to the preceding program as an extra source file (the guide's
   module section); a bare mainless block is a fragment — counted in
   the summary, not audited, because wrapping a fragment into a
   compilable program would test the wrapper, not the manual.

Only ```nlang fences are audited; other languages (shell transcripts)
are not programs. A page without any ```nlang fence fails — an audit
that checks nothing must not pass vacuously. Programs run with their
scratch directory as the working directory, so a snippet that writes
relative paths cannot litter the repository.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

_FENCE_OPEN = re.compile(r"^```(\S*)(.*)$")
_RETURN = re.compile(r"\breturn\s+(-?\d+)\s*;")
_RETURN_WORD = re.compile(r"\breturn\b")
_MAIN = re.compile(r"\bmain\s*\(")
_NATIVE_DECL = re.compile(
    r"(?m)^[ \t]*(?:(?:public|private|protected)[ \t]+)*native[ \t]+\S")
_ERROR_QUOTE_START = re.compile(r"^\s*Error\b")

#Each snippet compile+run is a sub-second job; a hang means the toolchain
#is broken, and the audit must fail rather than stall the docs build.
_SNIPPET_TIMEOUT_SEC = 30


class SnippetBlock:
    """One ```nlang fence: optional `<path>` label after the language,
    plus the error-example quote lines when the fence belongs to an
    error-example run (attached by parse_blocks)."""

    def __init__(self, line, lang, label, code, error_quote=None):
        self.line = line          #1-based source line, for reports
        self.lang = lang
        self.label = label or None
        self.code = code
        self.error_quote = error_quote


def parse_blocks(text):
    """(blocks, problems) — the ```nlang fences of a markdown page in
    document order, plus every place the audit's scope shrank. Error
    quotes are resolved here (see the module docstring's convention)."""
    lines = text.splitlines()
    problems = []
    fences = []                   #(open line idx, lang, label, body lines)
    i = 0
    while i < len(lines):
        opened = _FENCE_OPEN.match(lines[i])
        if not opened:
            i += 1
            continue
        lang, label = opened.group(1), opened.group(2).strip()
        end = next((j for j in range(i + 1, len(lines))
                    if lines[j].startswith("```")), None)
        if end is None:
            #An unclosed fence swallows the rest of the page in markdown
            #too; report the shrink instead of quietly auditing less.
            problems.append(
                "line %d: fence ```%s is never closed; the rest of the "
                "page is not audited" % (i + 1, lang))
            break
        fences.append((i, lang, label, lines[i + 1:end]))
        i = end + 1
    #Attach each Error-quote to the maximal run of consecutive nlang
    #fences right before it; a fence already claimed by an earlier quote
    #is not re-claimed. One shared list per run: group_errors uses the
    #object identity to tell one run's fences from another's.
    quotes = {}                   #fence index -> quote lines
    for idx, (_, lang, _, body) in enumerate(fences):
        if lang != "text":
            continue
        first = next((x for x in body if x.strip()), "")
        if not _ERROR_QUOTE_START.match(first):
            continue
        quote_lines = [x.strip() for x in body if x.strip()]
        j = idx - 1
        while j >= 0 and fences[j][1] == "nlang" and j not in quotes:
            j -= 1
        for k in range(j + 1, idx):
            quotes[k] = quote_lines
    blocks = [SnippetBlock(f[0] + 1, f[1], f[2], "\n".join(f[3]) + "\n",
                           quotes.get(i))
              for i, f in enumerate(fences) if f[1] == "nlang"]
    return blocks, problems


def expected_exit(code):
    """The exit code a snippet's main promises (None when there is no
    main or its returns are computed).

    See the module docstring for the single/max/0 derivation, the
    refusal of negative literals, and the computed-return rule.
    """
    match = _MAIN.search(code)
    if match is None:
        return None
    open_brace = code.find("{", match.start())
    end = len(code)
    if open_brace != -1:
        #Match the closing brace of main's body (depth counting; snippet
        #bodies are plain code — string/comment braces would need a real
        #lexer, which a doc auditor deliberately is not).
        depth = 0
        for i in range(open_brace, len(code)):
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
    body = code[match.start():end]
    returns = [int(m.group(1)) for m in _RETURN.finditer(body)]
    if min(returns, default=0) < 0:
        raise ValueError(
            "snippet main returns a negative literal; exit-code promises "
            "must be 0-255")
    if not returns:
        #No literal return: a bare `return` word means a computed result
        #(no promise); no return at all promises 0.
        return None if _RETURN_WORD.search(body) else 0
    if len(returns) < len(_RETURN_WORD.findall(body)):
        #Mixed literal and computed returns: the computed path can exit
        #with any value, so no literal is a reliable promise.
        return None
    return max(returns)


class Program:
    """A runnable snippet: one main-bearing entry plus optional helpers.
    Error-example runs reuse this shape and carry their quote lines."""

    def __init__(self, name, line):
        self.name = name
        self.line = line
        self.files = {}           #relative path -> source text
        self.entry = None         #relative path of the main-bearing file
        self.quote = None         #error-example quote lines, else None

    def add(self, relpath, code, has_main):
        self.files[relpath] = code
        if has_main:
            self.entry = relpath


def _label_escapes(label):
    """True when a <path> label would materialize outside the scratch dir."""
    path = Path(label)
    return path.is_absolute() or ".." in path.parts


def group_programs(blocks):
    """(programs, fragments, problems) — labeled mainless fences join the
    preceding program; bare mainless fences are fragments (counted, not
    problems); error-example blocks are left out (audit_doc groups them);
    a <path> label that would escape the scratch dir or duplicate a file
    already in the program is refused, never silently rewritten."""
    programs = []
    fragments = []
    problems = []
    for block in blocks:
        if block.error_quote is not None:
            continue
        has_main = _MAIN.search(block.code) is not None
        label = Path(block.label).as_posix() if block.label else None
        if label is not None and _label_escapes(label):
            problems.append(
                "line %d: <path> label '%s' is absolute or contains '..'; "
                "labels are scratch-relative file paths"
                % (block.line, label))
            continue
        if has_main:
            #A labeled entry keeps its label path: module imports resolve
            #against the materialized layout, so modules/main.n must land
            #at modules/main.n, not flattened to the scratch root.
            relpath = label or "snippet_%02d.n" % (len(programs) + 1)
            program = Program(Path(relpath).stem, block.line)
            program.add(relpath, block.code, True)
            programs.append(program)
        elif programs and label:
            if label in programs[-1].files:
                problems.append(
                    "line %d: duplicate <path> label '%s'; the earlier "
                    "block keeps the name" % (block.line, label))
                continue
            programs[-1].add(label, block.code, False)
        else:
            fragments.append(block)
    return programs, fragments, problems


def group_errors(blocks):
    """(programs, problems) — error-example programs: consecutive
    error-quoted fences become one program (the multi-file module
    examples); labels name the files. Escaped or duplicated labels are
    refused as problems, never silently dropped (an audit that checks
    less must say so)."""
    programs = []
    problems = []
    current = None
    for block in blocks:
        if block.error_quote is None:
            current = None
            continue
        if current is None or block.error_quote is not current.quote:
            current = Program("error_%02d" % (len(programs) + 1), block.line)
            current.quote = block.error_quote
            programs.append(current)
        if block.label and _label_escapes(block.label):
            problems.append(
                "line %d: <path> label '%s' is absolute or contains '..'; "
                "labels are scratch-relative file paths"
                % (block.line, block.label))
            continue
        relpath = (Path(block.label).as_posix() if block.label
                   else "e%d_%02d.n" % (len(programs), len(current.files) + 1))
        if relpath in current.files:
            problems.append(
                "line %d: duplicate <path> label '%s' in an error example; "
                "the earlier block keeps the name" % (block.line, relpath))
            continue
        current.add(relpath, block.code, _MAIN.search(block.code) is not None)
    return programs, problems


_NPROJ = """<?xml version="1.0" encoding="UTF-8"?>
<Project name="{name}">
  <Sources>
{sources}
  </Sources>
</Project>
"""


def _materialize(program, workdir):
    """Write the program's files; multi-file programs get a synthesized
    .nproj (project mode is how cross-directory imports compile). The
    .nproj sits beside the entry file, so module paths resolve exactly
    as the snippet's imports spell them (utils/helper.n next to main.n
    is module utils.helper). An error run without any main uses its
    first file as the layout anchor."""
    for relpath, code in program.files.items():
        path = workdir / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(code, encoding="utf-8")
    if len(program.files) == 1:
        return None
    anchor = program.entry or next(iter(program.files))
    entry_dir = (workdir / anchor).parent
    sources = "\n".join(
        '    <File path="%s"/>' % Path(os.path.relpath(
            workdir / relpath, entry_dir)).as_posix()
        for relpath in sorted(program.files))
    nproj = entry_dir / (Path(anchor).stem + ".nproj")
    nproj.write_text(_NPROJ.format(
        name=Path(anchor).stem, sources=sources), encoding="utf-8")
    return nproj


def _run(cmd, cwd=None):
    return subprocess.run(
        cmd, capture_output=True, timeout=_SNIPPET_TIMEOUT_SEC, cwd=cwd)


def _build_cmd(program, ncc, scratch, nproj):
    artifact = scratch / ("out.npkg" if nproj is not None else "out.ncu")
    if nproj is not None:
        return [ncc, "build", "-p", str(nproj), "-o", str(artifact)], artifact
    entry = program.entry or next(iter(program.files))
    return [ncc, "build", str(scratch / entry), "-o", str(artifact)], artifact


def audit_doc(doc_path, ncc, nvm, workdir):
    """Compile+run every ```nlang program on the page and compile-check
    every error-example run.

    Returns (problems, programs, fragments, error_examples) — the
    human-readable problems (empty = clean), the audited programs, the
    bare mainless fragments and the error-example runs, so callers can
    account for every fence on the page. Every program gets its own
    scratch directory, removed afterwards; the scratch directory is
    also the working directory of the compile+run, so build artifacts
    and any files a snippet writes stay inside it.
    """
    blocks, problems = parse_blocks(
        Path(doc_path).read_text(encoding="utf-8"))
    if not blocks:
        if problems:
            #The page's only fences never closed; that refusal is the
            #whole audit result.
            return problems, [], [], []
        raise ValueError(
            "%s: no ```nlang blocks found; an audit that checks nothing "
            "must not pass" % doc_path)
    programs, fragments, grouping_problems = group_programs(blocks)
    problems += grouping_problems
    errors, error_problems = group_errors(blocks)
    problems += error_problems
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    for program in programs + errors:
        scratch = Path(tempfile.mkdtemp(prefix="snippet_", dir=str(workdir)))
        try:
            if program.quote is None:
                problems += _audit_program(program, ncc, nvm, scratch)
            else:
                problems += _audit_error(program, ncc, scratch)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
    return problems, programs, fragments, errors


def _declares_native(program):
    return any(_NATIVE_DECL.search(code) for code in program.files.values())


def _audit_program(program, ncc, nvm, scratch):
    nproj = _materialize(program, scratch)
    #Project builds pack a .npkg program archive; single-source builds
    #emit a .ncu unit image. The extension picks the loader path in nvm,
    #so each mode must name its own output form.
    build_cmd, artifact = _build_cmd(program, ncc, scratch, nproj)
    built = _run(build_cmd, cwd=scratch)
    if built.returncode != 0:
        return ["line %d (%s): snippet failed to compile: %s"
                % (program.line, program.name,
                   built.stderr.decode("utf-8", "replace").strip()[:300])]
    if _declares_native(program):
        #A native declaration is implemented by the embedding host;
        #plain nvm would throw at the call, so a clean compile is the
        #honest ceiling for this shape.
        return []
    try:
        expected = expected_exit(program.files[program.entry])
    except ValueError as err:
        #A refused exit-code promise is a problem naming the block, not
        #an uncaught exception ending the whole audit.
        return ["line %d (%s): %s" % (program.line, program.name, err)]
    ran = _run([nvm, str(artifact)], cwd=scratch)
    if expected is None:
        #Computed return: the exit value is not a promise, but a clean
        #run is — nvm reports every runtime failure on stderr.
        err = ran.stderr.decode("utf-8", "replace").strip()
        if err:
            return ["line %d (%s): snippet with a computed return reported: %s"
                    % (program.line, program.name, err[:300])]
        return []
    if ran.returncode != expected:
        return ["line %d (%s): snippet exited %s, expected %s"
                % (program.line, program.name, ran.returncode, expected)]
    return []


def _audit_error(program, ncc, scratch):
    """An error-example run must fail to compile, and every quote line
    must appear verbatim in the compiler's stderr (the manual quotes
    the `Error: <message>` part, so substring matching, not line
    equality).

    A multi-file error example is compiled the way sealed modules are
    consumed: each source builds in document order into
    ``imports/<stem>.ncu`` under one shared ``-I`` directory, with each
    source staged in a directory of its own (library source discovery
    scans the search dirs, the working directory *and* the directory of
    the file being compiled — a sibling ``.n`` would be silently
    inlined). The cross-module restrictions these examples demonstrate
    only exist across sealed ``.ncu`` boundaries — a synthesized
    ``.nproj`` inlines the sources and would compile them cleanly. The
    first failing step is the expected failure point; its stderr must
    carry the quote. If every step compiles, the quoted diagnostic no
    longer reproduces."""
    if len(program.files) == 1:
        nproj = _materialize(program, scratch)
        build_cmd, _ = _build_cmd(program, ncc, scratch, nproj)
        built = _run(build_cmd, cwd=scratch)
        if built.returncode == 0:
            return ["line %d (%s): error example compiled successfully; the "
                    "quoted diagnostic no longer reproduces"
                    % (program.line, program.name)]
        err = built.stderr.decode("utf-8", "replace")
    else:
        imports = scratch / "imports"
        imports.mkdir(parents=True, exist_ok=True)
        srcs = {}
        for i, (relpath, code) in enumerate(program.files.items()):
            src = scratch / "src" / str(i) / relpath
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_text(code, encoding="utf-8")
            srcs[relpath] = src
        err = None
        for relpath in program.files:  #document order: producers first
            built = _run(
                [ncc, "build", str(srcs[relpath]),
                 "-o", str(imports / (Path(relpath).stem + ".ncu")),
                 "-I", str(imports)], cwd=scratch)
            if built.returncode != 0:
                err = built.stderr.decode("utf-8", "replace")
                break
        if err is None:
            return ["line %d (%s): error example compiled successfully; the "
                    "quoted diagnostic no longer reproduces"
                    % (program.line, program.name)]
    missing = [q for q in program.quote if q not in err]
    if missing:
        return ["line %d (%s): quoted diagnostic not in compiler output: %s"
                % (program.line, program.name, "; ".join(missing)[:300])]
    return []
