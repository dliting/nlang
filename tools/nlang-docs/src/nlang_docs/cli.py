"""CLI: build / serve / check / snippets.

build = mkdocs build --strict + offline search inlining + site audit
chained (a broken link fails the build), plus -- when --ncc/--nvm are
given -- the runnable-snippet audit of the guide. serve = plain mkdocs
serve for iteration. check = site audit only, for already-generated
sites; it accepts --config to also enforce nav coverage and otherwise
falls back to the CWD's mkdocs.yml (the cmake recipe runs from the repo
root). snippets = the runnable-snippet audit alone, for direct runs.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from . import __version__
from .linkcheck import check_site
from .offline_search import inline_search_index
from .snippets import audit_doc


def _mkdocs(args):
    #Shell out so this package never imports mkdocs itself.
    return subprocess.run(
        [sys.executable, "-m", "mkdocs", *args]).returncode


def _find_nav_config():
    """The CWD's mkdocs.yml, or None when there is none (the nav
    coverage rule is skipped rather than guessed about)."""
    candidate = Path.cwd() / "mkdocs.yml"
    return candidate if candidate.is_file() else None


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="nlang-docs",
        description="NLang help-manual pipeline "
                    "(mkdocs wrapper + link audits)")
    parser.add_argument("--version", action="version",
                        version="nlang-docs " + __version__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_build = sub.add_parser(
        "build", help="mkdocs build --strict, then audit the site")
    p_build.add_argument("--config", required=True,
                         help="mkdocs.yml (absolute path)")
    p_build.add_argument("--site-dir", required=True,
                         help="output directory")
    p_build.add_argument("--ncc", default=None,
                         help="ncc binary for the snippet audit "
                              "(env NLANG_NCC); audit runs only when "
                              "both --ncc and --nvm are given")
    p_build.add_argument("--nvm", default=None,
                         help="nvm binary for the snippet audit "
                              "(env NLANG_NVM)")

    p_serve = sub.add_parser(
        "serve", help="mkdocs serve (dev preview loop)")
    p_serve.add_argument("--config", required=True)

    p_check = sub.add_parser(
        "check", help="audit an already-generated site")
    p_check.add_argument("--site-dir", required=True)
    p_check.add_argument("--config", default=None,
                         help="mkdocs.yml for the nav-coverage rule "
                              "(default: mkdocs.yml in the CWD; the "
                              "rule is skipped when none is found)")

    p_snippets = sub.add_parser(
        "snippets", help="compile+run the ```nlang snippets of a page")
    p_snippets.add_argument("--doc", required=True,
                            help="markdown page to audit")
    p_snippets.add_argument("--ncc", default=None,
                            help="ncc binary (env NLANG_NCC)")
    p_snippets.add_argument("--nvm", default=None,
                            help="nvm binary (env NLANG_NVM)")
    p_snippets.add_argument("--workdir", default=None,
                            help="scratch root (default: a temp dir)")

    opts = parser.parse_args(argv)
    if opts.command == "build":
        rc = _mkdocs(["build", "--strict", "-f", opts.config,
                      "-d", opts.site_dir])
        if rc != 0:
            return rc
        rc = inline_search_index(opts.site_dir)
        if rc != 0:
            return rc
        rc = check_site(Path(opts.site_dir), Path(opts.config))
        if rc != 0:
            return rc
        return _snippet_stage(opts)
    if opts.command == "serve":
        return _mkdocs(["serve", "-f", opts.config])
    if opts.command == "check":
        config = (Path(opts.config) if opts.config is not None
                  else _find_nav_config())
        return check_site(Path(opts.site_dir), config)
    if opts.command == "snippets":
        doc = Path(opts.doc)
        if not doc.is_file():
            #A mistyped --doc is a clean error, not a traceback from the
            #file read inside the audit.
            print("snippets: page not found: %s" % doc, file=sys.stderr)
            return 1
        return _snippet_audit(
            doc, opts.ncc, opts.nvm,
            Path(opts.workdir) if opts.workdir else None)
    #argparse required=True makes this unreachable today; returning
    #explicitly beats silently falling into another subcommand later.
    return None


def _snippet_stage(opts):
    """The build chain's 4th stage: the guide's snippets compile+run.

    Only runs when both binaries are known (--ncc/--nvm or env); the
    cmake recipe always passes them, so a bare manual build says so
    instead of silently skipping the audit. The page is looked up under
    the config's sibling docs/ tree (this repo's docs_dir).
    """
    page = Path(opts.config).parent / "docs" / "nlang-getting-started.md"
    if not page.is_file():
        print("snippets: skipped (%s not found)" % page, file=sys.stderr)
        return 0
    return _snippet_audit(page, opts.ncc, opts.nvm, None)


def _snippet_audit(page, ncc, nvm, workdir):
    ncc = ncc or os.environ.get("NLANG_NCC")
    nvm = nvm or os.environ.get("NLANG_NVM")
    if not (ncc and nvm):
        print("snippets: skipped (give --ncc/--nvm or set NLANG_NCC/"
              "NLANG_NVM to audit the guide's snippets)", file=sys.stderr)
        return 0
    #CreateProcess rejects a relative path holding forward slashes; make
    #both binaries absolute against the invoking CWD.
    ncc, nvm = os.path.abspath(ncc), os.path.abspath(nvm)
    #Scratch under the system temp dir by default: never inside docs/,
    #so stray artifacts can never leak into the site or its globs.
    if workdir is None:
        with tempfile.TemporaryDirectory(prefix="nlang_snippets_") as tmp:
            return _snippet_audit_run(page, ncc, nvm, Path(tmp))
    return _snippet_audit_run(page, ncc, nvm, workdir)


def _snippet_audit_run(page, ncc, nvm, workdir):
    problems, programs, skipped = audit_doc(page, ncc, nvm, workdir)
    for problem in problems:
        print("snippets: " + problem, file=sys.stderr)
    if problems:
        print("snippets: %d problem(s) in %s"
              % (len(problems), page), file=sys.stderr)
        return 1
    print("snippets: %d programs OK (%d skipped)"
          % (len(programs), len(skipped)))
    return 0
