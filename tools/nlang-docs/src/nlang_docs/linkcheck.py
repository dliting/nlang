"""Site-wide link/anchor audit over generated HTML.

Three rules (see docs/superpowers/specs/
2026-08-29-nlang-docs-subsystem-design.md):
 1. internal relative link targets must exist as .html files;
 2. #fragments must exist in the target page (mkdocs heading ids
    and explicit id="..." attributes both count);
 3. directory-form links are forbidden (the site must stay
    navigable over file://, where Chromium resolves no index.html).

Only <a href=...> is audited: stylesheets/scripts are <link>/<script>
and must not be mistaken for navigation.
"""
import re
import sys
from pathlib import Path
from urllib.parse import unquote

_A_HREF_RE = re.compile(r'<a\b[^>]*href="([^"]*)"')
_ID_RE = re.compile(r'\bid="([^"]+)"')
_EXTERNAL = ("http:", "https:", "mailto:", "data:")


def check_site(site_dir):
    """Print violations, return process exit code (0 = clean)."""
    site = Path(site_dir).resolve()
    pages = sorted(site.rglob("*.html"))
    anchor_ids = {}
    for page in pages:
        html = page.read_text(encoding="utf-8", errors="replace")
        anchor_ids[page] = set(_ID_RE.findall(html))

    violations = []
    for page in pages:
        html = page.read_text(encoding="utf-8", errors="replace")
        base = page.parent
        for href in _A_HREF_RE.findall(html):
            target, _, frag = href.partition("#")
            if target.startswith(_EXTERNAL):
                continue
            if target == "":
                #Fragment-only link: validate against this page.
                if frag and unquote(frag) not in anchor_ids[page]:
                    violations.append(
                        "%s: broken same-page anchor #%s"
                        % (page.relative_to(site), frag))
                continue
            if target.startswith("/"):
                #Server-root form (mkdocs-material's 404.html): not a
                #relative link; it cannot resolve over file://, but the
                #only page emitting it can never render there either.
                continue
            if not target.endswith(".html"):
                violations.append(
                    "%s: directory-form link '%s'"
                    % (page.relative_to(site), target))
                continue
            resolved = (base / unquote(target)).resolve()
            if not resolved.is_file():
                violations.append(
                    "%s: missing target '%s'"
                    % (page.relative_to(site), target))
                continue
            if not frag:
                continue
            #A fragment counts when this page defines the id itself,
            #otherwise the target page must define it.
            if unquote(frag) in anchor_ids[page]:
                continue
            if unquote(frag) not in anchor_ids.get(resolved, set()):
                violations.append(
                    "%s: anchor '%s#%s' not found"
                    % (page.relative_to(site), target, frag))
    for v in violations:
        print("linkcheck: " + v, file=sys.stderr)
    if violations:
        print("linkcheck: %d violation(s) under %s"
              % (len(violations), site), file=sys.stderr)
        return 1
    print("linkcheck: %d pages, all internal links OK" % len(pages))
    return 0
