"""The per-tree header language switch: every generated page carries a
link to the OTHER tree's home page at a page-depth-correct relative
URL. file:// has no server root, so an absolute path would break in
the nide embedded help browser -- the depth must come from page.url."""
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
OVERRIDE = REPO / "tools" / "nlang-docs" / "theme_override"
SRC = REPO / "tools" / "nlang-docs" / "src"


def _nlang_docs(args, env):
    #check=True alone would discard the output on failure; surface the
    #stderr (the audit's violation lines) in the raised error.
    run = subprocess.run([sys.executable, "-m", "nlang_docs", *args],
                         capture_output=True, text=True, env=env)
    if run.returncode != 0:
        raise AssertionError("%s failed rc=%d:\n%s"
                             % (" ".join(args), run.returncode, run.stderr))


def _build_tree_pair(tmp_path: Path) -> dict:
    docs = tmp_path / "docs"
    (docs / "sub").mkdir(parents=True)
    (docs / "index.md").write_text("# Home\n", encoding="utf-8")
    (docs / "sub" / "page.md").write_text("# Sub\n", encoding="utf-8")
    env = dict(os.environ,
               PYTHONPATH=str(SRC) + os.pathsep
               + os.environ.get("PYTHONPATH", ""))
    sites = {}
    configs = {}
    for tree, other, label in [("zh", "en", "English"),
                               ("en", "zh", "中文")]:
        config = tmp_path / f"mkdocs.{tree}.yml"
        config.write_text(
            "site_name: t\n"
            "docs_dir: docs\n"
            "theme:\n"
            "  name: material\n"
            "  font: false\n"  # else material emits Google Fonts links
            f"  custom_dir: {OVERRIDE.as_posix()}\n"
            "use_directory_urls: false\n"
            #The nav-coverage rule requires the nav to name every page.
            "nav:\n"
            "  - Home: index.md\n"
            "  - Sub: sub/page.md\n"
            "extra:\n"
            f"  lang_switch_target: {other}\n"
            f"  lang_switch_label: {label}\n",
            encoding="utf-8")
        configs[tree] = config
        site = tmp_path / "site" / tree
        #The switch links point at the sibling tree, so a chained
        #build's site audit would fail on the first tree (no sibling
        #yet): build deferred, then audit both like the cmake recipe.
        _nlang_docs(["build", "--config", str(config),
                     "--site-dir", str(site), "--defer-site-audit"], env)
        sites[tree] = site
    for tree, site in sites.items():
        _nlang_docs(["check", "--site-dir", str(site),
                     "--config", str(configs[tree])], env)
    return sites


@pytest.mark.parametrize("tree,other,label",
                         [("zh", "en", "English"),
                          ("en", "zh", "中文")])
def test_switch_link_on_every_page(tmp_path, tree, other, label):
    site = _build_tree_pair(tmp_path)[tree]
    for page in [site / "index.html", site / "sub" / "page.html"]:
        html = page.read_text(encoding="utf-8")
        assert 'class="nlang-lang-switch"' in html, page
        #Depth rule: pages sit 1..N levels below the tree root; the
        #other tree is a sibling of this tree.
        depth = len(page.relative_to(site).parts)
        expected = "../" * depth + f"{other}/index.html"
        assert f'href="{expected}"' in html, page
        assert label in html.split("nlang-lang-switch", 1)[1][:300]
