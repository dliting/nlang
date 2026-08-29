"""Runnable-snippet audit over a markdown page.

Extracts the page's ```nlang fences, materializes each one as a real
source file, compiles it with ncc and runs it with nvm, and compares the
process exit code against what the snippet itself promises. This is the
regression pin for the getting-started guide: a language change that
breaks a documented snippet (or an edit that drifts the prose's claimed
exit code) fails the docs pipeline.

Conventions the derivation relies on (documented here, not guessed at
runtime):
 - a snippet "has main" when it contains `main(`; such a block is a
   runnable program and its exit-code promise is derived from the
   `return <int>;` statements in main's body;
 - with a single return that literal is the promise; with several (the
   guide's self-check pattern: `if (ok) return FEATURED; return 1;`)
   the promise is the maximum — the failure sentinel 1 must never
   dominate a featured code, and a future snippet violating that fails
   the audit loudly instead of silently passing;
 - no return in main promises 0; a negative literal names no process
   exit code and is refused;
 - blocks without main are not runnable: a ```nlang <path> label joins
   them to the preceding program as an extra source file (the guide's
   module section); a bare mainless block is skipped and reported, so
   nothing disappears from the accounting silently.

Only ```nlang fences are audited; other languages (shell transcripts)
are not programs. A page without any ```nlang fence fails — an audit
that checks nothing must not pass vacuously.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

_FENCE_OPEN = re.compile(r"^```(\S*)(.*)$")
_RETURN = re.compile(r"\breturn\s+(-?\d+)\s*;")
_MAIN = re.compile(r"\bmain\s*\(")

#Each snippet compile+run is a sub-second job; a hang means the toolchain
#is broken, and the audit must fail rather than stall the docs build.
_SNIPPET_TIMEOUT_SEC = 30


class SnippetBlock:
    """One ```nlang fence: optional `<path>` label after the language."""

    def __init__(self, line, lang, label, code):
        self.line = line          #1-based source line, for reports
        self.lang = lang
        self.label = label or None
        self.code = code


def parse_blocks(text):
    """(blocks, problems) — the ```nlang fences of a markdown page in
    document order, plus every place the audit's scope shrank."""
    blocks = []
    problems = []
    lines = text.splitlines()
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
        if lang == "nlang":
            blocks.append(SnippetBlock(
                i + 1, lang, label,
                "\n".join(lines[i + 1:end]) + "\n"))
        i = end + 1
    return blocks, problems


def expected_exit(code):
    """The exit code a snippet's main promises (None when there is no main).

    See the module docstring for the single/max/0 derivation and the
    refusal of negative literals.
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
    if not returns:
        return 0
    if min(returns) < 0:
        raise ValueError(
            "snippet main returns a negative literal; exit-code promises "
            "must be 0-255")
    return max(returns)


class Program:
    """A runnable snippet: one main-bearing entry plus optional helpers."""

    def __init__(self, name, line):
        self.name = name
        self.line = line
        self.files = {}           #relative path -> source text
        self.entry = None         #relative path of the main-bearing file

    def add(self, relpath, code, has_main):
        self.files[relpath] = code
        if has_main:
            self.entry = relpath


def _label_escapes(label):
    """True when a <path> label would materialize outside the scratch dir."""
    path = Path(label)
    return path.is_absolute() or ".." in path.parts


def group_programs(blocks):
    """(programs, skipped, problems) — labeled mainless fences join the
    preceding program; bare mainless fences are reported as skipped; a
    <path> label that would escape the scratch dir or duplicate a file
    already in the program is refused, never silently rewritten."""
    programs = []
    skipped = []
    problems = []
    for block in blocks:
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
            skipped.append(block)
    return programs, skipped, problems


_NPROJ = """<?xml version="1.0" encoding="UTF-8"?>
<Project name="{name}" namespace="{name}">
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
    is module utils.helper)."""
    for relpath, code in program.files.items():
        path = workdir / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(code, encoding="utf-8")
    if len(program.files) == 1:
        return None
    entry_dir = (workdir / program.entry).parent
    sources = "\n".join(
        '    <File path="%s"/>' % Path(os.path.relpath(
            workdir / relpath, entry_dir)).as_posix()
        for relpath in sorted(program.files))
    nproj = entry_dir / (Path(program.entry).stem + ".nproj")
    nproj.write_text(_NPROJ.format(
        name=Path(program.entry).stem, sources=sources), encoding="utf-8")
    return nproj


def _run(cmd, **kwargs):
    return subprocess.run(
        cmd, capture_output=True, timeout=_SNIPPET_TIMEOUT_SEC, **kwargs)


def audit_doc(doc_path, ncc, nvm, workdir):
    """Compile+run every ```nlang program on the page.

    Returns (problems, programs, skipped) — the human-readable problems
    (empty = clean), the audited programs and the skipped bare blocks,
    so callers can account for every fence on the page. Every program
    gets its own scratch directory, removed afterwards; the .nmod files
    stay inside it, so the source tree never gains build artifacts.
    """
    blocks, problems = parse_blocks(
        Path(doc_path).read_text(encoding="utf-8"))
    if not blocks:
        if problems:
            #The page's only fences never closed; that refusal is the
            #whole audit result.
            return problems, [], []
        raise ValueError(
            "%s: no ```nlang blocks found; an audit that checks nothing "
            "must not pass" % doc_path)
    programs, skipped, grouping_problems = group_programs(blocks)
    problems += grouping_problems
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    for program in programs:
        scratch = Path(tempfile.mkdtemp(prefix="snippet_", dir=str(workdir)))
        try:
            problems += _audit_program(program, ncc, nvm, scratch)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
    for block in skipped:
        problems.append(
            "line %d: ```nlang block has no main and no <path> label to "
            "join a program with; not runnable, please label or inline it"
            % block.line)
    return problems, programs, skipped


def _audit_program(program, ncc, nvm, scratch):
    nproj = _materialize(program, scratch)
    if nproj is not None:
        build_cmd = [ncc, "build", "-p", str(nproj),
                     "-o", str(scratch / "out.nmod")]
    else:
        build_cmd = [ncc, "build", str(scratch / program.entry),
                     "-o", str(scratch / "out.nmod")]
    built = _run(build_cmd)
    if built.returncode != 0:
        return ["line %d (%s): snippet failed to compile: %s"
                % (program.line, program.name,
                   built.stderr.decode("utf-8", "replace").strip()[:300])]
    try:
        expected = expected_exit(program.files[program.entry])
    except ValueError as err:
        #A refused exit-code promise is a problem naming the block, not
        #an uncaught exception ending the whole audit.
        return ["line %d (%s): %s" % (program.line, program.name, err)]
    ran = _run([nvm, str(scratch / "out.nmod")])
    if ran.returncode != expected:
        return ["line %d (%s): snippet exited %s, expected %s"
                % (program.line, program.name, ran.returncode, expected)]
    return []
