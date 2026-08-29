"""Getting-started snippet audit: block extraction, exit-code
derivation, program grouping, and the compile+run gate itself."""
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
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
    blocks = parse_blocks(doc)
    assert len(blocks) == 1
    assert blocks[0].lang == "nlang"
    assert blocks[0].label is None
    assert "return 7;" in blocks[0].code

    labeled = parse_blocks("```nlang modules/main.n\nint main() { return 0; }\n```\n")
    assert labeled[0].label == "modules/main.n"


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
    programs, skipped = group_programs(parse_blocks(doc))
    assert skipped == []
    assert len(programs) == 1
    assert set(programs[0].files) == {
        "modules/main.n", "modules/helper.n", "modules/utils/helper.n"}
    assert programs[0].entry == "modules/main.n"


def test_grouping_bare_helpers_without_program_are_skipped():
    doc = "```nlang\nint twice(int x) { return x * 2; }\n```\n"
    programs, skipped = group_programs(parse_blocks(doc))
    assert programs == []
    assert len(skipped) == 1


GOOD_DOC = "```nlang\nimport io;\nint main() { return 7; }\n```\n"
WRONG_DOC = "```nlang\nimport io;\nint main() { if (2 + 2 == 5) { return 7; }\n" \
            "return 1; }\n```\n"
COMPILE_ERROR_DOC = "```nlang\nint main() { return nosuchfn(); }\n```\n"


@needs_tools
def test_audit_green_doc_passes(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(GOOD_DOC, encoding="utf-8")
    problems = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert problems == []


@needs_tools
def test_audit_failed_expectation_names_the_block(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(WRONG_DOC, encoding="utf-8")
    problems = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert len(problems) == 1
    assert "exited 1" in problems[0] and "expected 7" in problems[0]


@needs_tools
def test_audit_compile_error_is_reported(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(COMPILE_ERROR_DOC, encoding="utf-8")
    problems = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert len(problems) == 1
    assert "compile" in problems[0]


@needs_tools
def test_audit_multifile_project_and_skips(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text(
        "`main.n`:\n```nlang modules/main.n\n"
        "import utils.helper;\nint main() { if (utils.helper.answer() == 42)"
        " { return 42; } return 1; }\n```\n"
        "`utils/helper.n`:\n```nlang modules/utils/helper.n\n"
        "int answer() { return 42; }\n```\n", encoding="utf-8")
    problems = audit_doc(doc, NCC, NVM, tmp_path / "work")
    assert problems == []


def test_audit_empty_doc_fails_not_vacuous(tmp_path):
    doc = tmp_path / "page.md"
    doc.write_text("no code here\n", encoding="utf-8")
    with pytest.raises(ValueError):
        audit_doc(doc, "ignored", "ignored", tmp_path / "work")
