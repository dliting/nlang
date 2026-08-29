"""Offline search index inlining, exercised against synthetic sites."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs.offline_search import inline_search_index  # noqa: E402


def write_index(site: Path, payload: str) -> Path:
    search = site / "search"
    search.mkdir(parents=True, exist_ok=True)
    (search / "search_index.json").write_text(payload, encoding="utf-8")
    return search


def test_index_inlined_as_global_script(tmp_path):
    #Material's file: branch script-tags the .js to pick up __index;
    #the payload must end up byte-identical to the .json's contents.
    search = write_index(tmp_path, '{"config": {}, "docs": [{"t": "x"}]}')
    assert inline_search_index(tmp_path) == 0
    js = (search / "search_index.js").read_text(encoding="utf-8")
    assert js == 'var __index = {"config": {}, "docs": [{"t": "x"}]}'


def test_broken_json_fails_without_writing_js(tmp_path, capsys):
    search = write_index(tmp_path, '{"docs": [truncated')
    assert inline_search_index(tmp_path) == 1
    assert "cannot inline" in capsys.readouterr().err
    assert not (search / "search_index.js").exists()


def test_site_without_search_plugin_skips(tmp_path, capsys):
    #A site built with search disabled has no index to inline; that is
    #a configuration, not an error.
    assert inline_search_index(tmp_path) == 0
    assert "not inlined" in capsys.readouterr().err
