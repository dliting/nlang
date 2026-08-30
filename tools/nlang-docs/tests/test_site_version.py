"""Site-footer version injection, exercised with real mini mkdocs builds.

The pipeline exports NLANG_COPYRIGHT (composed from the VERSION file
next to mkdocs.yml) before invoking mkdocs; the config picks it up via
mkdocs' built-in !ENV tag. A bare build without a VERSION file keeps
the config's dev fallback.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs import cli  # noqa: E402


def _write_mini_project(tmp_path: Path):
    #Material theme (the pipeline's own pinned dependency), with the same
    #two offline settings the real mkdocs.yml pins (font: false kills the
    #Google Fonts round-trip; use_directory_urls: false keeps links in
    #file://-navigable .html form) -- otherwise the audit rules reject
    #the theme's own chrome. The nav is required by the nav-coverage rule.
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "index.md").write_text("# Home\n", encoding="utf-8")
    (tmp_path / "mkdocs.yml").write_text(
        "site_name: Mini\n"
        "docs_dir: docs\n"
        "theme:\n"
        "  name: material\n"
        "  font: false\n"
        "use_directory_urls: false\n"
        "nav:\n"
        "  - Home: index.md\n"
        "copyright: !ENV [NLANG_COPYRIGHT, NLang dev build]\n",
        encoding="utf-8")


def _run_build(tmp_path: Path):
    return cli.main(["build",
                     "--config", str(tmp_path / "mkdocs.yml"),
                     "--site-dir", str(tmp_path / "site")])


def test_build_embeds_version_in_footer(tmp_path):
    _write_mini_project(tmp_path)
    (tmp_path / "VERSION").write_text("0.1.0\n", encoding="utf-8")

    assert _run_build(tmp_path) == 0
    html = (tmp_path / "site" / "index.html").read_text(encoding="utf-8")
    assert "NLang v0.1.0" in html


def test_bare_build_falls_back_to_dev_label(tmp_path):
    #No VERSION file: the env stays unset and mkdocs uses the default.
    _write_mini_project(tmp_path)

    assert _run_build(tmp_path) == 0
    html = (tmp_path / "site" / "index.html").read_text(encoding="utf-8")
    assert "NLang dev build" in html
