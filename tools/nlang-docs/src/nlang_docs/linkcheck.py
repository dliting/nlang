"""Site-wide link/anchor audit over generated HTML.

Three rules:
 1. internal relative link targets must exist as .html files;
 2. #fragments must exist in the target page (mkdocs heading ids
    and explicit id="..." attributes both count);
 3. directory-form links are forbidden (the site must stay
    navigable over file://, where Chromium resolves no index.html).

A fourth rule joins in when the mkdocs config is available: every
built page (except 404.html) must be exactly the nav's page set --
'mkdocs build --strict' only logs nav omissions as INFO, so a page
missing from the nav would otherwise silently vanish from the side
navigation.

A fifth rule keeps the site offline-only: script[src] / link[href]
may not reference http(s). Stylesheets/scripts are deliberately not
treated as navigation, so a CDN resource would otherwise sail through
the audit -- which is exactly how an unpkg polyfill once slipped into
the built site (see offline_search.py for why the pipeline inlines
the search index itself).
"""
import re
import sys
from pathlib import Path
from urllib.parse import unquote

try:
    import yaml
except ImportError:
    #pyyaml is a hard mkdocs dependency, so the nav rule runs wherever
    #mkdocs does; exotic environments merely degrade the fourth rule
    #to a skip with a note.
    yaml = None

_A_HREF_RE = re.compile(r'<a\b[^>]*href="([^"]*)"')
_SCRIPT_SRC_RE = re.compile(r'<script\b[^>]*\bsrc="([^"]*)"')
_LINK_HREF_RE = re.compile(r'<link\b[^>]*\bhref="([^"]*)"')
_ID_RE = re.compile(r'\bid="([^"]+)"')
_EXTERNAL = ("http:", "https:", "mailto:", "data:")


if yaml is not None:
    class _ConfigLoader(yaml.SafeLoader):
        """SafeLoader that tolerates mkdocs' `!!python/name:` tags.

        The nav rule only reads nav strings; a python object reference
        (e.g. a toc slugify function) is recorded as its dotted path
        instead of failing the whole config read. The tagged node itself
        is empty — the name is spelled out in the tag's suffix — and no
        object is ever instantiated, which is exactly what SafeLoader
        refuses.
        """

    _ConfigLoader.add_multi_constructor(
        "tag:yaml.org,2002:python/name",
        lambda loader, suffix, node: suffix.lstrip(":"))


def _check_links(site_dir):
    """The three <a href> rules; prints violations, returns exit code."""
    site = Path(site_dir).resolve()
    pages = sorted(site.rglob("*.html"))
    if not pages:
        #An empty or mistargeted site dir must fail the audit, not
        #pass vacuously (verify_package reuses this check).
        print("linkcheck: no .html pages under %s" % site, file=sys.stderr)
        return 1
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
                    "%s: non-.html internal link '%s'"
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


def _check_remote_resources(site_dir):
    """Rule 5: no http(s) script[src] / link[href] in any page."""
    site = Path(site_dir).resolve()
    pages = sorted(site.rglob("*.html"))
    violations = []
    for page in pages:
        html = page.read_text(encoding="utf-8", errors="replace")
        refs = [("script", url) for url in _SCRIPT_SRC_RE.findall(html)]
        refs += [("link", url) for url in _LINK_HREF_RE.findall(html)]
        for tag, url in refs:
            if url.startswith(("http:", "https:")):
                violations.append(
                    "%s: remote %s resource '%s' (site must stay offline)"
                    % (page.relative_to(site), tag, url))
    for v in violations:
        print("linkcheck: " + v, file=sys.stderr)
    if violations:
        print("linkcheck: %d remote-resource violation(s) under %s"
              % (len(violations), site), file=sys.stderr)
        return 1
    print("linkcheck: %d pages, no remote resources" % len(pages))
    return 0


def _nav_html_pages(config_path):
    """The .md pages the nav lists, as the site paths they build to.

    mkdocs resolves nav entries against docs_dir and -- with
    use_directory_urls: false, which this site fixes -- mirrors the
    docs tree one-to-one, so docs/<path>.md becomes site <path>.html.
    """
    #An empty (or all-comments) mkdocs.yml parses to None.
    config = yaml.load(
        Path(config_path).read_text(encoding="utf-8"), Loader=_ConfigLoader)
    if not isinstance(config, dict):
        raise yaml.YAMLError("config is not a mapping")
    leaves = []

    def collect(entries):
        #Nav entries are strings or {title: subtree-string-or-list}.
        for entry in entries:
            if isinstance(entry, dict):
                for value in entry.values():
                    if isinstance(value, list):
                        collect(value)
                    else:
                        leaves.append(value)
            else:
                leaves.append(entry)

    collect(config.get("nav") or [])
    return {leaf[:-3] + ".html" for leaf in leaves
            if isinstance(leaf, str) and leaf.endswith(".md")}


def _check_nav_coverage(site_dir, config_path):
    """Set equality between built pages (except 404.html) and the nav.

    Skipped (with a note, exit code 0) when no config was supplied or
    pyyaml is unavailable -- the three link rules stay authoritative.
    """
    if yaml is None:
        print("linkcheck: nav coverage skipped (pyyaml unavailable)",
              file=sys.stderr)
        return 0
    if config_path is None:
        print("linkcheck: nav coverage skipped (no mkdocs.yml)",
              file=sys.stderr)
        return 0
    try:
        nav_pages = _nav_html_pages(config_path)
    except (OSError, yaml.YAMLError) as err:
        print("linkcheck: nav coverage failed: cannot read %s (%s)"
              % (config_path, err), file=sys.stderr)
        return 1
    site = Path(site_dir).resolve()
    built = {page.relative_to(site).as_posix()
             for page in site.rglob("*.html") if page.name != "404.html"}
    violations = ["page not in nav (unreachable): %s" % page
                  for page in sorted(built - nav_pages)]
    violations += ["nav page not built: %s" % page
                   for page in sorted(nav_pages - built)]
    for violation in violations:
        print("linkcheck: " + violation, file=sys.stderr)
    if violations:
        print("linkcheck: %d nav coverage violation(s) under %s"
              % (len(violations), site), file=sys.stderr)
        return 1
    print("linkcheck: %d pages, nav coverage OK" % len(built))
    return 0


def check_site(site_dir, config_path=None):
    """Print violations, return process exit code (0 = clean).

    config_path (a mkdocs.yml) turns on the nav-coverage rule; the CLI
    supplies it for build and check, bare calls skip that rule.
    """
    link_rc = _check_links(site_dir)
    nav_rc = _check_nav_coverage(site_dir, config_path)
    remote_rc = _check_remote_resources(site_dir)
    return 1 if (link_rc != 0 or nav_rc != 0 or remote_rc != 0) else 0
