"""linkcheck audit rules, exercised against synthetic sites."""
import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs.linkcheck import check_site  # noqa: E402


def make_site(tmp_path: Path, pages: dict) -> Path:
    """pages: {relative_name: html_body}."""
    for name, body in pages.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(body), encoding="utf-8")
    return tmp_path


def test_good_links_pass(tmp_path, capsys):
    site = make_site(tmp_path, {
        "index.html": '<a href="a.html">A</a> <h2 id="sec">S</h2>'
                      '<a href="#sec">sec</a>',
        "a.html": '<a href="sub/b.html">B</a> <h2 id="sec">S</h2>',
        "sub/b.html": '<a href="../a.html">up</a>'
                      '<a href="../a.html#sec">anchor</a>',
    })
    assert check_site(site) == 0


def test_missing_target_fails(tmp_path, capsys):
    site = make_site(tmp_path, {
        "index.html": '<a href="nowhere.html">X</a>',
    })
    assert check_site(site) == 1
    assert "missing target 'nowhere.html'" in capsys.readouterr().err


def test_broken_anchor_fails(tmp_path, capsys):
    site = make_site(tmp_path, {
        "index.html": '<a href="a.html#nope">X</a>',
        "a.html": '<h2 id="real">R</h2>',
    })
    assert check_site(site) == 1
    assert "anchor 'a.html#nope' not found" in capsys.readouterr().err


def test_directory_form_link_fails(tmp_path, capsys):
    site = make_site(tmp_path, {
        "index.html": '<a href="a/">X</a>',
        "a/index.html": '<p></p>',
    })
    assert check_site(site) == 1
    assert "non-.html internal link 'a/'" in capsys.readouterr().err


def test_empty_site_fails(tmp_path, capsys):
    #verify_package reuses this audit; an empty (or mistargeted) site
    #dir must fail loudly instead of passing vacuously.
    assert check_site(tmp_path) == 1
    assert "no .html pages" in capsys.readouterr().err


def test_same_page_broken_anchor_fails(tmp_path, capsys):
    site = make_site(tmp_path, {
        "index.html": '<h2 id="real">R</h2> <a href="#nope">X</a>',
    })
    assert check_site(site) == 1
    assert "broken same-page anchor #nope" in capsys.readouterr().err


def test_cross_page_anchor_hit_on_origin_page_passes(tmp_path):
    #A fragment naming a target page still counts when the linking
    #page itself defines that id; otherwise the target page's ids
    #are checked.
    site = make_site(tmp_path, {
        "index.html": '<h2 id="sec">S</h2> <a href="a.html#sec">X</a>',
        "a.html": '<p></p>',
    })
    assert check_site(site) == 0


def test_url_encoded_target_resolved(tmp_path):
    site = make_site(tmp_path, {
        "index.html": '<a href="a%20b.html">X</a>',
        "a b.html": '<p></p>',
    })
    assert check_site(site) == 0


def test_root_relative_server_form_links_skipped(tmp_path):
    #mkdocs-material's 404.html links the nav with server-root URLs;
    #that page can never render over file://, so the audit skips the
    #root form (relative links stay fully validated).
    site = make_site(tmp_path, {
        "404.html": '<a href="/index.html">home</a>',
        "index.html": '<p></p>',
    })
    assert check_site(site) == 0


def test_external_and_mailto_skipped(tmp_path):
    site = make_site(tmp_path, {
        "index.html": '<a href="https://example.com/x">e</a>'
                      '<a href="mailto:a@b.c">m</a>'
                      '<link rel="stylesheet" href="styles.css">',
    })
    assert check_site(site) == 0
