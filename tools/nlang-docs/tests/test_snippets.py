"""Getting-started snippet audit: block extraction, exit-code
derivation, program grouping, and the compile+run gate itself."""
import os
import sys
import types
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs.cli import main as cli_main  # noqa: E402
from nlang_docs.snippets import (  # noqa: E402
    audit_doc, expected_exit, group_programs, parse_blocks)

#Snippet binaries are the build tree's ncc/nvm; the ctest wiring exports
#them so this suite exercises real compiles. Without them only the
#pure-parsing tests run (skip says so, instead of silently passing).
NCC = os.environ.get("NLANG_NCC")
NVM = os.environ.get("NLANG_NVM")
needs_tools = pytest.mark.skipif(
    not (NCC and NVM and os.path.isfile(NCC) and os.path.isfile(NVM)),
    reason="ncc/nvm not provided via NLANG_NCC/NLANG_NVM")


def test_parse_blocks_keeps_labels_and_skips_other_fences():
    doc = "text\n```text\nnot nlang\n```\n```nlang\nint main() { return 7; }\n```\n"
    blocks, problems = parse_blocks(doc)
    assert problems == []
    assert len(blocks) == 1
    assert blocks[0].lang == "nlang"
    assert blocks[0].label is None
    assert "return 7;" in blocks[0].code

    labeled, _ = parse_blocks(
        "```nlang modules/main.n\nint main() { return 0; }\n```\n")
    assert labeled[0].label == "modules/main.n"


def test_parse_blocks_reports_unclosed_fence():
    #An unclosed fence swallows the rest of the page in markdown, so the
    #audit must say its scope shrank instead of quietly auditing less.
    doc = ("```nlang\nint main() { return 7; }\n```\n"
           "prose\n```text\nnever closed\n")
    blocks, problems = parse_blocks(doc)
    assert len(blocks) == 1
    assert len(problems) == 1
    assert "line 5" in problems[0]
    assert "not audited" in problems[0]


def test_expected_exit_single_guard_pattern_and_no_main():
    assert expected_exit("int main() { return 7; }") == 7
    #Doc convention: `return 1;` is the failure sentinel, the featured
    #code sits in the self-check guard — several returns derive to the max.
    assert expected_exit(
        "int main() { if (x == 30) { return 30; } return 1; }") == 30
    assert expected_exit("int main() { io.print(1); }") == 0
    #helpers without main are skipped by the audit, not derived to 0
    assert expected_exit("int twice(int x) { return x * 2; }") is None
    #a negative return cannot name a process exit code — refuse it
    with pytest.raises(ValueError):
        expected_exit("int main() { return -1; }")
    #a return whose value is not a literal promises no exit code
    assert expected_exit("int f() { return 1; }\nint main() { return f(); }") is None
    assert expected_exit(
        "int main() { if (x) { return 5; } return compute(); }") is None


def test_grouping_labeled_helpers_join_previous_program():
    doc = (
        "```nlang modules/main.n\n"
        "import utils.helper;\nint main() { return 42; }\n"
        "```\n"
        "```nlang modules/helper.n\n"
        "int twice(int x) { return x * 2; }\n"
        "```\n"
        "```nlang modules/utils/helper.n\n"
        "int answer() { return 42; }\n"
        "```\n")
    blocks, _ = parse_blocks(doc)
    programs, fragments, problems = group_programs(blocks)
    assert problems == []
    assert fragments == []
    assert len(programs) == 1
    assert set(programs[0].files) == {
        "modules/main.n", "modules/helper.n", "modules/utils/helper.n"}
    assert programs[0].entry == "modules/main.n"


def test_grouping_bare_helpers_without_program_are_fragments():
    doc = "```nlang\nint twice(int x) { return x * 2; }\n```\n"
    blocks, _ = parse_blocks(doc)
    programs, fragments, problems = group_programs(blocks)
    assert programs == []
    assert fragments == [blocks[0]]
    assert problems == []


def _labeled_block_doc(label, code="int twice(int x) { return x * 2; }\n"):
    return "```nlang %s\n%s```\n" % (label, code)


def test_grouping_refuses_escaping_labels(tmp_path):
    #A <path> label becomes a file under the scratch dir; one that is
    #absolute or climbs out with '..' would write outside it, so the
    #grouping refuses it instead of rewriting the author's path.
    absolute = str(Path(tmp_path.anchor or "/") / "escape.n")
    cases = [(absolute, "escape.n"),
             ("modules/../helper.n", "'modules/../helper.n'"),
             ("..\\helper.n", "helper.n")]
    for label, fragment in cases:
        doc = (_labeled_block_doc("modules/main.n",
                                  "int main() { return 0; }\n")
               + _labeled_block_doc(label))
        programs, fragments, problems = group_programs(parse_blocks(doc)[0])
        assert len(programs) == 1
        assert fragments == []
        assert len(problems) == 1, label
        assert "line 4" in problems[0]
        assert fragment in problems[0]
    #The guard covers entries too, not just helpers.
    doc = _labeled_block_doc(absolute, "int main() { return 0; }\n")
    programs, fragments, problems = group_programs(parse_blocks(doc)[0])
    assert programs == []
    assert fragments == []
    assert len(problems) == 1
    assert "line 1" in problems[0]


def test_grouping_refuses_duplicate_labels():
    doc = (_labeled_block_doc("modules/main.n", "int main() { return 0; }\n")
           + _labeled_block_doc("modules/helper.n")
           + _labeled_block_doc("modules/helper.n"))
    programs, fragments, problems = group_programs(parse_blocks(doc)[0])
    assert len(programs) == 1
    assert set(programs[0].files) == {
        "modules/main.n", "modules/helper.n"}
    assert fragments == []
    assert len(problems) == 1
    assert "line 7" in problems[0]
    assert "modules/helper.n" in problems[0]
    #Duplicating the entry's own label is the same refusal.
    doc = (_labeled_block_doc("modules/main.n", "int main() { return 0; }\n")
           + _labeled_block_doc("modules/main.n"))
    programs, fragments, problems = group_programs(parse_blocks(doc)[0])
    assert len(programs) == 1
    assert set(programs[0].files) == {"modules/main.n"}
    assert fragments == []
    assert len(problems) == 1
    assert "line 4" in problems[0]


def test_error_quote_attaches_to_preceding_nlang_run():
    doc = ("```nlang lib.n\nint g() { return 1; }\n```\n"
           "```nlang main.n\nimport lib;\nint main() { return g(); }\n```\n"
           "\n```text\nError: boom here\n```\n"
           "```nlang\nint main() { return 0; }\n```\n")
    blocks, problems = parse_blocks(doc)
    assert problems == []
    assert blocks[0].error_quote == ["Error: boom here"]
    assert blocks[1].error_quote == ["Error: boom here"]
    assert blocks[2].error_quote is None


def test_error_quote_not_attached_across_other_fence():
    doc = ("```nlang\nint main() { return 0; }\n```\n"
           "```bash\nls\n```\n"
           "```nlang\nint main() { return 1; }\n```\n"
           "```text\nError: only the last block\n```\n")
    blocks, _ = parse_blocks(doc)
    assert blocks[0].error_quote is None
    assert blocks[1].error_quote == ["Error: only the last block"]


def test_bare_mainless_blocks_are_fragments_not_problems():
    doc = "```nlang\nint twice(int x) { return x * 2; }\n```\n"
    blocks, _ = parse_blocks(doc)
    programs, fragments, problems = group_programs(blocks)
    assert programs == []
    assert problems == []
    assert len(fragments) == 1


GOOD_DOC = "```nlang\nimport io;\nint main() { return 7; }\n```\n"
WRONG_DOC = "```nlang\nimport io;\nint main() { if (2 + 2 == 5) { return 7; }\n" \
            "return 1; }\n```\n"
COMPILE_ERROR_DOC = "```nlang\nint main() { return nosuchfn(); }\n```\n"
NEGATIVE_DOC = "```nlang\nint main() { return -1; }\n```\n"


@needs_tools
def test_audit_green_doc_passes(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(GOOD_DOC, encoding="utf-8")
    problems, programs, fragments, errors = audit_doc(
        doc, NCC, NVM, tmp_path / "work")
    assert problems == []
    assert len(programs) == 1
    assert fragments == []
    assert errors == []


@needs_tools
def test_audit_failed_expectation_names_the_block(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(WRONG_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert len(problems) == 1
    assert "exited 1" in problems[0] and "expected 7" in problems[0]


@needs_tools
def test_audit_compile_error_is_reported(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(COMPILE_ERROR_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert len(problems) == 1
    assert "compile" in problems[0]


@needs_tools
def test_audit_negative_return_is_a_located_problem(tmp_path):
    #expected_exit refuses negative literals; the refusal must surface as
    #a problem naming the block, not as an uncaught exception.
    doc = tmp_path / "page.md"
    doc.write_text(NEGATIVE_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert len(problems) == 1
    assert "line 1" in problems[0]
    assert "negative" in problems[0]


@needs_tools
def test_audit_multifile_project_and_fragments(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(
        "`main.n`:\n```nlang modules/main.n\n"
        "import utils.helper;\nint main() { if (utils.helper.answer() == 42)"
        " { return 42; } return 1; }\n```\n"
        "`utils/helper.n`:\n```nlang modules/utils/helper.n\n"
        "int answer() { return 42; }\n```\n", encoding="utf-8")
    problems, programs, fragments, _ = audit_doc(
        doc, NCC, NVM, tmp_path / "work")
    assert problems == []
    assert len(programs) == 1
    assert fragments == []


ERROR_DOC = ("```nlang\nimport nosuch;\nint main() { return 0; }\n```\n"
             "\n```text\nError: Module 'nosuch' not found. Check the project "
             "Sources list or -I import path.\n```\n")
ERROR_MISMATCH_DOC = ("```nlang\nimport nosuch;\nint main() { return 0; }\n```\n"
                      "\n```text\nError: a diagnostic that never fires\n```\n")
ERROR_COMPILES_DOC = ("```nlang\nint main() { return 0; }\n```\n"
                      "\n```text\nError: stale quote\n```\n")
NATIVE_DOC = ("```nlang\nnative int natAdd(int a, int b);\n"
              "int main() { return natAdd(20, 22); }\n```\n")
COMPUTED_DOC = ("```nlang\nint f() { return 5; }\nint main() { return f(); }\n```\n")
COMPUTED_CRASH_DOC = ("```nlang\nint main() {\n    int[] a = [1, 2, 3];\n"
                      "    return a[10];\n}\n```\n")


@needs_tools
def test_audit_error_example_passes(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(ERROR_DOC, encoding="utf-8")
    problems, programs, fragments, errors = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert problems == []
    assert programs == [] and fragments == []
    assert len(errors) == 1


@needs_tools
def test_audit_error_example_quote_mismatch_is_located(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(ERROR_MISMATCH_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert len(problems) == 1
    assert "line 1" in problems[0]
    assert "not in compiler output" in problems[0]
    assert "a diagnostic that never fires" in problems[0]


@needs_tools
def test_audit_error_example_that_compiles_is_reported(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(ERROR_COMPILES_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert len(problems) == 1
    assert "compiled successfully" in problems[0]


ERROR_MULTI_DOC = (
    "```nlang lib.n\nint addCast(float a, int b = a as int) {\n"
    "    return b;\n}\n```\n"
    "\n```nlang main.n\nimport lib;\n\nint main() {\n"
    "    return lib.addCast(1.5f);\n}\n```\n"
    "\n```text\nError: imported function 'lib.addCast' has a default for "
    "parameter 1 outside the cross-module subset\n```\n")


@needs_tools
def test_audit_error_example_multi_file_uses_sealed_ncu_path(tmp_path):
    #The cross-module default-expression subset restriction only fires
    #across sealed .ncu boundaries; a .nproj build inlines the sources
    #and would compile this example cleanly, so the audit must use the
    #sealed sequential recipe (the F-001 shape from the manual).
    doc = tmp_path / "page.md"
    doc.write_text(ERROR_MULTI_DOC, encoding="utf-8")
    problems, _, _, errors = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert problems == []
    assert len(errors) == 1


@needs_tools
def test_audit_native_block_is_compile_only(tmp_path):
    #plain nvm has no host registration: running would exit 1, so the
    #audit must stop at a successful compile.
    doc = tmp_path / "page.md"
    doc.write_text(NATIVE_DOC, encoding="utf-8")
    problems, programs, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert problems == []
    assert len(programs) == 1


@needs_tools
def test_audit_computed_return_clean_run_passes(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(COMPUTED_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert problems == []


@needs_tools
def test_audit_computed_return_crash_is_reported(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(COMPUTED_CRASH_DOC, encoding="utf-8")
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert len(problems) == 1
    assert "Runtime error" in problems[0]


@needs_tools
def test_audit_runs_programs_in_scratch_cwd(tmp_path, monkeypatch):
    #a snippet that writes a relative path must not litter the invoking
    #directory: nvm runs with the scratch dir as its working directory.
    doc = tmp_path / "page.md"
    doc.write_text("```nlang\nimport io;\nint main() {\n"
                   "    io.writeFile(\"marker.txt\", \"x\");\n    return 0;\n}\n```\n",
                   encoding="utf-8")
    cwd = tmp_path / "cwd"
    cwd.mkdir()
    monkeypatch.chdir(cwd)
    problems, _, _, _ = audit_doc(doc, NCC, NVM, tmp_path / "w")
    assert problems == []
    assert not (cwd / "marker.txt").exists()


def test_cli_missing_doc_fails_cleanly(tmp_path, capsys):
    #A mistyped --doc path must be a clean error, not a traceback from
    #the file read deep inside the audit.
    missing = tmp_path / "nope.md"
    assert cli_main(["snippets", "--doc", str(missing)]) == 1
    assert "not found" in capsys.readouterr().err


def test_audit_empty_doc_fails_not_vacuous(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text("no code here\n", encoding="utf-8")
    with pytest.raises(ValueError):
        audit_doc(doc, "ignored", "ignored", tmp_path / "work")


def test_stage_audits_every_page_in_the_docs_tree(tmp_path, monkeypatch):
    #Full-tree audit: pages sit in nested chapter directories, so the
    #stage must recurse past getting-started/ into language-spec/ and
    #the docs root. One plain program page, one page whose audit also
    #yields a fragment and an error example, plus one prose page: the
    #summary line must account for every fence kind.
    from nlang_docs import cli
    docs = tmp_path / "docs"
    (docs / "getting-started").mkdir(parents=True)
    (docs / "language-spec").mkdir(parents=True)
    (docs / "getting-started" / "a.md").write_text(
        "```nlang\nint main() { return 0; }\n```\n", encoding="utf-8")
    (docs / "language-spec" / "b.md").write_text(
        "```nlang\nint main() { return 0; }\n```\n", encoding="utf-8")
    (docs / "c.md").write_text("prose only, no fenced code\n",
                               encoding="utf-8")

    def fake_audit_doc(page, ncc, nvm, wd):
        #audit_doc raises ValueError only for a page with no ```nlang
        #block at all; a broken fence comes back as problems instead.
        if page.name == "c.md":
            raise ValueError("no ```nlang blocks found")
        if page.name == "b.md":
            return [], [object()], [object()], [object()]
        return [], [object()], [], []

    (tmp_path / "mkdocs.yml").write_text("site_name: t\n", encoding="utf-8")
    opts = types.SimpleNamespace(
        config=str(tmp_path / "mkdocs.yml"), doc=None, workdir=None,
        ncc=None, nvm=None)
    monkeypatch.setattr(cli, "audit_doc", fake_audit_doc)
    monkeypatch.setenv("NLANG_NCC", "x")   #fake binaries so the audit runs
    monkeypatch.setenv("NLANG_NVM", "x")
    rc, line = cli._snippet_stage(opts)
    assert rc == 0
    assert line == ("snippets: 3 pages, 2 programs + 1 error examples OK "
                    "(1 fragments, 1 prose)")


def test_stage_fails_when_any_page_has_problems(tmp_path, monkeypatch,
                                                capsys):
    #One clean page plus one refusing page: the stage fails the build
    #(rc=1, no summary line) and prefixes each problem with its path
    #relative to docs_dir, so multi-page reports stay attributable even
    #when chapter directories repeat basenames.
    from nlang_docs import cli
    guide = tmp_path / "docs" / "getting-started"
    guide.mkdir(parents=True)
    for name in ("a.md", "b.md"):
        (guide / name).write_text(
            "```nlang\nint main() { return 0; }\n```\n", encoding="utf-8")
    (tmp_path / "mkdocs.yml").write_text("site_name: t\n", encoding="utf-8")

    def fake_audit_doc(page, ncc, nvm, wd):
        if page.name == "b.md":
            return ["exit code 1, expected 0"], [], [], []
        return [], [object()], [], []

    opts = types.SimpleNamespace(
        config=str(tmp_path / "mkdocs.yml"), doc=None, workdir=None,
        ncc=None, nvm=None)
    monkeypatch.setattr(cli, "audit_doc", fake_audit_doc)
    monkeypatch.setenv("NLANG_NCC", "x")
    monkeypatch.setenv("NLANG_NVM", "x")
    rc, line = cli._snippet_stage(opts)
    assert rc == 1
    assert line is None
    assert ("getting-started/b.md: exit code 1, expected 0"
            in capsys.readouterr().err)


def test_real_zh_config_resolves_a_nonempty_guide(monkeypatch, capsys):
    #Bilingual split: the stage derives the guide from the config's
    #docs_dir instead of the old repo-root docs/getting-started. A
    #regression there silently degrades the zh build's 4th stage to
    #"snippets: skipped (... not found)" while the build stays green.
    #So this drives _snippet_stage ITSELF -- the call site the build
    #runs, not the resolution helper (a helper-only test would stay
    #green after a call-site revert) -- with the real tree's config
    #and no binaries. The two skip lines then discriminate the
    #outcomes: a resolved guide reaches the "give --ncc/--nvm" skip,
    #an unresolved one dies earlier with the path's "not found" line.
    #(The en tree legitimately skips until its guide translation
    #lands, so only the zh path is pinned.)
    from nlang_docs import cli
    repo = Path(__file__).resolve().parents[3]
    opts = types.SimpleNamespace(
        config=str(repo / "docs" / "mkdocs.zh.yml"), doc=None, workdir=None,
        ncc=None, nvm=None)
    monkeypatch.delenv("NLANG_NCC", raising=False)
    monkeypatch.delenv("NLANG_NVM", raising=False)
    rc, line = cli._snippet_stage(opts)
    assert (rc, line) == (0, None)
    stderr = capsys.readouterr().err
    #The docs/user_manual/zh-based outcome: the guide resolved (the stage got past
    #the path check) and only the missing binaries stopped it.
    assert "give --ncc/--nvm" in stderr, stderr
    assert "not found" not in stderr, stderr
