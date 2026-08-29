"""linkcheck audit rules, exercised against synthetic sites."""
import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs.cli import main as cli_main  # noqa: E402
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


# --- nav coverage (rule 4; needs the mkdocs config) -------------------

def write_nav_config(tmp_path: Path, nav_body: str) -> Path:
    """A minimal mkdocs.yml carrying just the nav to audit."""
    config = tmp_path / "mkdocs.yml"
    config.write_text("nav:\n" + textwrap.dedent(nav_body), encoding="utf-8")
    return config


def test_nav_omitted_page_fails(tmp_path, capsys):
    #'mkdocs build --strict' logs a nav omission as INFO and keeps
    #going; this rule must turn it into a build failure.
    site = make_site(tmp_path, {"index.html": "<p></p>",
                                "orphan.html": "<p></p>"})
    config = write_nav_config(tmp_path, "  - 主页: index.md\n")
    assert check_site(site, config) == 1
    assert "page not in nav (unreachable): orphan.html" \
        in capsys.readouterr().err


def test_nav_page_missing_from_site_fails(tmp_path, capsys):
    site = make_site(tmp_path, {"index.html": "<p></p>"})
    config = write_nav_config(
        tmp_path, "  - 主页: index.md\n  - 幽灵: ghost.md\n")
    assert check_site(site, config) == 1
    assert "nav page not built: ghost.html" in capsys.readouterr().err


def test_full_nav_coverage_passes(tmp_path):
    #Nested sections and the 404.html exemption both in play.
    site = make_site(tmp_path, {
        "index.html": "<p></p>",
        "language-spec/overview.html": "<p></p>",
        "404.html": "<a href=\"/index.html\">home</a>",
    })
    config = write_nav_config(
        tmp_path,
        "  - 主页: index.md\n"
        "  - 语言规格:\n"
        "      - 概览: language-spec/overview.md\n")
    assert check_site(site, config) == 0


def test_check_without_config_skips_nav_rule(tmp_path, capsys):
    #A bare audit stays usable; the nav rule notes its own absence.
    site = make_site(tmp_path, {"index.html": "<p></p>"})
    assert check_site(site, None) == 0
    assert "nav coverage skipped" in capsys.readouterr().err


def test_check_without_config_still_runs_link_rules(tmp_path, capsys):
    site = make_site(tmp_path, {"index.html": '<a href="x.html">X</a>'})
    assert check_site(site, None) == 1
    assert "missing target 'x.html'" in capsys.readouterr().err


def test_check_finds_nav_config_in_cwd(tmp_path, capsys, monkeypatch):
    #cli check falls back to the CWD's mkdocs.yml (the cmake recipe
    #runs from the repo root, where it lives).
    site = make_site(tmp_path / "site", {"index.html": "<p></p>",
                                         "orphan.html": "<p></p>"})
    write_nav_config(tmp_path, "  - 主页: index.md\n")
    monkeypatch.chdir(tmp_path)
    assert cli_main(["check", "--site-dir", str(site)]) == 1
    assert "page not in nav (unreachable): orphan.html" \
        in capsys.readouterr().err


def test_check_with_explicit_config(tmp_path, capsys):
    site = make_site(tmp_path, {"index.html": "<p></p>",
                                "orphan.html": "<p></p>"})
    config = write_nav_config(tmp_path, "  - 主页: index.md\n")
    assert cli_main(["check", "--site-dir", str(site),
                     "--config", str(config)]) == 1
    assert "page not in nav (unreachable): orphan.html" \
        in capsys.readouterr().err


def test_unreadable_config_fails_loudly(tmp_path, capsys):
    #An explicitly supplied config that cannot be parsed must fail the
    #audit instead of silently degrading to the three link rules.
    site = make_site(tmp_path, {"index.html": "<p></p>"})
    config = tmp_path / "mkdocs.yml"
    config.write_text("nav: [unclosed\n", encoding="utf-8")
    assert check_site(site, config) == 1
    assert "cannot read" in capsys.readouterr().err


def test_empty_config_fails_loudly(tmp_path, capsys):
    #yaml parses an all-comments config to None, not an empty dict.
    site = make_site(tmp_path, {"index.html": "<p></p>"})
    config = tmp_path / "mkdocs.yml"
    config.write_text("# comments only\n", encoding="utf-8")
    assert check_site(site, config) == 1
    assert "cannot read" in capsys.readouterr().err
