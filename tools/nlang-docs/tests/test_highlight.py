"""NLangLexer token assertions, anchored to the flex scanner (nlang.l)."""
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from pygments.token import Comment, Error, Keyword, Number, String  # noqa: E402

from nlang_docs.highlight import (  # noqa: E402
    NLANG_CONSTANTS, NLANG_KEYWORDS, NLANG_TYPES, NLangLexer)

_SCANNER = (Path(__file__).resolve().parents[3]
            / "src" / "compiler" / "grammar" / "nlang.l")


def _tokens(code):
    #value→type pairs: dict(_tokens(code)) answers toks["import"] is Keyword
    return [(tokv, tokt) for tokt, tokv in NLangLexer().get_tokens(code)]


def test_keywords_and_types_color_distinctly():
    toks = dict(_tokens("import class int foreach List"))
    assert toks["import"] is Keyword
    assert toks["class"] is Keyword
    assert toks["int"] is Keyword.Type
    assert toks["foreach"] is Keyword
    assert toks["List"] is Keyword.Type


def test_constants_true_false_null():
    toks = dict(_tokens("true false null"))
    assert all(toks[w] is Keyword.Constant for w in ("true", "false", "null"))


def test_string_with_escape_and_interpolation_stays_whole():
    vals = [t for t, v in _tokens('string s = "a\\n${x}b";')
            if v in String]
    assert '"a\\n${x}b"' == "".join(vals)


def test_comments_both_kinds():
    toks = _tokens("int a; // line\nint b; /*--- block ---*/")
    assert any(v in Comment and "line" in t for t, v in toks)
    assert any(v in Comment and "block" in t for t, v in toks)
    #Regression face of the (?s:) multiline rule: a block comment spanning
    #lines must stay one token (a plain .*? would stop at the first \n).
    spans = [t for t, v in _tokens("/*--- a\nb\nc ---*/") if v in Comment]
    assert ["/*--- a\nb\nc ---*/"] == spans


def test_numbers_int_and_float():
    toks = dict(_tokens("1 2.5"))
    assert toks["1"] in Number
    assert toks["2.5"] in Number


def test_realistic_program_has_no_error_tokens():
    #Unmatched characters fall to Token.Error, so a whole real program
    #(imports, calls, arithmetic, strings, comments) must yield none —
    #this is the guard that catches a missing punctuation/operator rule.
    program = (
        "import io;\n"
        "\n"
        "int add(int a, int b) {\n"
        "    return a + b * 2;\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    io.print(\"sum: \" + add(1, 2));  // greet\n"
        "}\n")
    errors = [t for t, v in _tokens(program) if v is Error]
    assert [] == errors


def test_keyword_list_matches_scanner_surface():
    #Drift guard: the scanner's reserved words are extracted from the
    #flex source itself, so a keyword added or removed in nlang.l turns
    #this red instead of silently diverging from the lexer's sets.
    scanner = frozenset(re.findall(
        r'"([A-Za-z]+)"\s*\{\s*return KT_\w+;',
        _SCANNER.read_text(encoding="utf-8")))
    #List/Dict/Func are docs-side builtin types, not scanner words; the
    #exact pin turns red both when a stray word joins NLANG_TYPES and
    #when the scanner adopts one of the three.
    docs_types = NLANG_TYPES - scanner
    assert frozenset({"List", "Dict", "Func"}) == docs_types
    assert scanner == ((NLANG_KEYWORDS | NLANG_TYPES | NLANG_CONSTANTS)
                       - docs_types)


_HOOK = Path(__file__).resolve().parents[1] / "src" / "nlang_docs" / \
    "highlight_hook.py"
#Mirrors mkdocs.yml's extension list including options: the labeled-fence
#test below only exercises the real behavior when this probe config
#carries the same superfences options as the repo build.
_REPO_MKDOCS_EXT = [
    "pymdownx.highlight",
    "pymdownx.superfences:\n      relaxed_headers: true",
]


def _build_mini_site(tmp_path, page_md):
    docs = tmp_path / "docs"
    docs.mkdir()
    (docs / "index.md").write_text(page_md, encoding="utf-8")
    (tmp_path / "mkdocs.yml").write_text(
        "site_name: probe\n"
        "docs_dir: docs\n"
        "hooks:\n"
        "  - %s\n"
        "markdown_extensions:\n" % _HOOK.as_posix() +
        "".join("  - %s\n" % e for e in _REPO_MKDOCS_EXT),
        encoding="utf-8")
    #theme omitted on purpose: we only assert span emission, not colors.
    #PYTHONPATH stripped so the self-sufficiency claim is tested for real
    #even when pytest itself was launched with PYTHONPATH set.
    env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
    return subprocess.run(
        [sys.executable, "-m", "mkdocs", "build", "--strict",
         "-f", str(tmp_path / "mkdocs.yml"),
         "-d", str(tmp_path / "site")],
        capture_output=True, text=True, timeout=60, env=env)


def test_nlang_fences_get_colored_spans(tmp_path):
    r = _build_mini_site(tmp_path, "```nlang\nint x = 1;\n```\n")
    assert r.returncode == 0, r.stderr
    html = (tmp_path / "site" / "index.html").read_text(encoding="utf-8")
    assert '<span class="kt">int</span>' in html


def test_bare_fences_stay_uncolored(tmp_path):
    r = _build_mini_site(tmp_path, "```\nError: boom\n```\n")
    assert r.returncode == 0, r.stderr
    html = (tmp_path / "site" / "index.html").read_text(encoding="utf-8")
    assert '<span class="kt">' not in html


def test_labeled_fence_path_still_forms_a_block(tmp_path):
    #Permanent guard for the #19 <path> label face: "```nlang main.n" is
    #not a strict superfences header (the path holds a '/', outside the
    #lang regex), so without relaxed_headers the whole line is rejected,
    #the ``` leaks into the paragraph, and the fence mispairs with the
    #next one — swallowing following prose into the code block.
    r = _build_mini_site(
        tmp_path, "```nlang modules/main.n\nint main() { return 0; }\n```\n")
    assert r.returncode == 0, r.stderr
    html = (tmp_path / "site" / "index.html").read_text(encoding="utf-8")
    assert '<span class="kt">int</span>' in html
    assert "```nlang" not in html


def test_hook_is_self_sufficient_without_pythonpath(tmp_path):
    r = _build_mini_site(tmp_path, "```nlang\nint x = 1;\n```\n")
    assert r.returncode == 0, r.stderr
    html = (tmp_path / "site" / "index.html").read_text(encoding="utf-8")
    assert '<span class="kt">int</span>' in html


_REPO_CONFIG = Path(__file__).resolve().parents[3] / "mkdocs.yml"


def test_repo_config_mounts_hook_and_superfences():
    #The three tests above prove the hook works when mounted; this pins
    #the real site's mount (spec 4.2): deleting the hooks block or
    #reverting to fenced_code turns red instead of silently uncoloring
    #the built site that the mini-site tests never see.
    text = _REPO_CONFIG.read_text(encoding="utf-8")
    assert "highlight_hook.py" in text
    assert "pymdownx.superfences" in text
    assert "relaxed_headers: true" in text
    assert "pymdownx.highlight" in text
    assert "- fenced_code" not in text
