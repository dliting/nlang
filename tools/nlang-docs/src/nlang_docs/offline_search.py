"""Offline search index generation (the file:// search enabler).

Material's bundle cannot fetch search/search_index.json over file://,
so it script-tags search/search_index.js (which assigns __index)
instead -- but the mkdocs search plugin only emits the .json. Material
ships an "offline" plugin doing the .js inlining, yet it also injects a
remote unpkg polyfill <script> into every page, which an offline-only
site must not reference; this module therefore performs just the
inlining step (identical output to the plugin's on_post_build).
"""
import json
import sys
from pathlib import Path


def inline_search_index(site_dir):
    """Write search/search_index.js next to the .json; return exit code.

    A site built without the search plugin has no index to inline: the
    step then only notes the skip, it is not an error.
    """
    index_json = Path(site_dir) / "search" / "search_index.json"
    if not index_json.is_file():
        print("offline_search: no search_index.json under %s; "
              "search index not inlined" % index_json, file=sys.stderr)
        return 0
    try:
        data = index_json.read_text(encoding="utf-8")
        json.loads(data)  # a truncated .json would ship a broken .js
    except (OSError, json.JSONDecodeError) as err:
        print("offline_search: cannot inline %s (%s)" % (index_json, err),
              file=sys.stderr)
        return 1
    index_js = index_json.with_name("search_index.js")
    index_js.write_text("var __index = " + data, encoding="utf-8")
    return 0
