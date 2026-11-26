# gate_time_table.py -- the reference timing table model and the single
# caps()/budget derivation site (D-1/D-3/D-5/D-7;
# docs/dev/gate_timing_design.md). load_table parses one table file,
# effective_rows folds in the machine-local overlay, caps() and
# caps_values compute the watchdog cap and the budget in exactly one
# place, resolve_row/resolve_scoped_row map a (config, entry) pair to
# its row, row_line/form_token print table-shape rows and
# measurement form tokens, and emit_cmake derives the configure-time
# ctest TIMEOUT caps file. The CLI entry and the guard verdict bodies
# live in the sibling gate_time_check / gate_time_measure /
# gate_time_guards modules. The watchdog cap and the budget are computed
# in exactly one
# place, caps(), per D-5. Stdlib only; run with -I.
#
# Importable consumers (D-5): caps(config, entry, work_factor=1.0,
# scalar=1.0) -> (budget_s, watchdog_s) and load_table(path).

import re
from collections import namedtuple
from pathlib import Path

# Frozen values pinned by the design (D-1/D-3/D-7/D-16); changing one
# changes the caps() or membership contract every consumer shares.
SAFETY_MULTIPLIER = 2
WATCHDOG_MULTIPLIER = 10
MIN_CAP_S = 60
MIN_WATCHDOG_S = 120
SCALE_BAND_MIN = 0.25
SCALE_BAND_MAX = 4.0
DEFAULT_WORK_FACTOR = 1
# The one fast/slow threshold (D-16): an entry name is slow exactly when
# its baseline row's reference seconds exceed this number; the slow set
# runs only in the pre-version closeout batch. This constant is the only
# site of the number, and the reference table never repeats it.
SLOW_MIN_REF_S = 60

SENTINEL_MARK = "yes"
# The samples scheduling class (2026-11-26 user ruling): a demo-
# acceptance entry marked here is excluded from every scheduled round
# by class, not by seconds, and re-runs in the closeout batch.
SAMPLES_MARK = "samples"
# Every token the sentinel column accepts. Anything else is a typo away
# from silently scheduling nothing, so the parser owns the vocabulary.
SENTINEL_VOCAB = (SENTINEL_MARK, SAMPLES_MARK)
SENTINELS_PER_CONFIG = (5, 8)
CTEST_CONFIGS = ("ctest-Debug", "ctest-IDE", "ctest-ASan")
E2E_CONFIGS = ("e2e-Debug", "e2e-ASan", "e2e-Release")
CONFIG_KEYS = CTEST_CONFIGS + ("ctest-Release",) + E2E_CONFIGS
MANIFEST_SCOPES = ("manifest.txt", "examples_manifest.txt")
LOCAL_TABLE_NAME = "gate_time_reference.local.txt"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TABLE = REPO_ROOT / "tests" / "gate_time_reference.txt"
# Each decision tree feeds exactly one ctest config key (D-2).
DECISION_TREES = {"ctest-Debug": "build",
                  "ctest-IDE": "build-ide",
                  "ctest-ASan": "build-asan"}

# One line of the claim roster (D-11): `claim <resource-name> <ctest
# entry>...` in tools/gate_tiers.txt. The resource-name charset is the
# one tools/gate.sh enforces at acquisition, so the lock directory name
# stays filesystem-safe on every platform. Entry names share the charset:
# real ctest entry names are identifiers, but a dotted or dashed name is
# legal to CMake and the add_test wrapper sanitizes it exactly as the
# TIMEOUT lookup does, so the roster tolerates punctuation and only
# rejects whitespace, which would make the roster ambiguous.
CLAIM_LINE_RE = re.compile(r"claim ([A-Za-z0-9_.-]+)"
                           r"(?: [A-Za-z0-9_.-]+)+\Z")

Row = namedtuple("Row", "config entry ref_s ref_work form sentinel reason "
                        "line")


def load_table(path):
    """Parse one reference table file. One row per (config, entry),
    whitespace-separated: config entry ref_s ref_work form [sentinel
    [reason]]; `reason` runs to end of line, so a row of seven tokens is
    complete, six carries a sentinel but no reason and five carries
    neither. Blank lines and `#` comments are skipped. Raises ValueError
    on a malformed row."""
    rows = []
    text = Path(path).read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split(None, 6)
        if len(fields) < 5:
            raise ValueError("%s line %d: expected at least five columns, "
                             "got %d" % (path, lineno, len(fields)))
        config, entry, ref_s, ref_work, form = fields[:5]
        sentinel = fields[5] if len(fields) > 5 else ""
        reason = fields[6] if len(fields) > 6 else ""
        if config not in CONFIG_KEYS:
            raise ValueError("%s line %d: unknown config key %r (want one "
                             "of %s)" % (path, lineno, config,
                                         ", ".join(CONFIG_KEYS)))
        if not (form.startswith("ctest-") or form.startswith("e2e-")
                or form == "unknown"):
            # D-17 deleted the category column between ref_work and the
            # form; a stale row would otherwise reparse silently with the
            # category token posing as the form, so the row shape error
            # names the deleted column instead of trusting the token.
            raise ValueError("%s line %d: column 5 must be the measurement "
                             "form (ctest-*/e2e-*/unknown); %r reads as the "
                             "category column deleted in D-17 -- drop it "
                             "and shift the rest one column left"
                             % (path, lineno, form))
        try:
            ref_s = float(ref_s)
            ref_work = int(ref_work)
        except ValueError:
            raise ValueError("%s line %d: ref_s/ref_work must be numeric"
                             % (path, lineno))
        if ref_s < 0.0 or ref_work < 1:
            raise ValueError("%s line %d: ref_s must be >= 0 and ref_work "
                             ">= 1" % (path, lineno))
        # The sentinel vocabulary check runs after the column-shape
        # checks above: a stale pre-D-17 row must keep reporting its
        # deleted category column, not a misleading token complaint.
        if sentinel and sentinel not in SENTINEL_VOCAB and sentinel != "-":
            raise ValueError("%s line %d: unknown sentinel token %r (want "
                             "one of yes/samples, or - for a plain row)"
                             % (path, lineno, sentinel))
        rows.append(Row(config, entry, ref_s, ref_work, form, sentinel,
                        reason, lineno))
    return rows


def effective_rows(table_path):
    """Rows of the checked-in table with the machine-local overlay applied
    (D-3): gate_time_reference.local.txt beside it overrides same
    (config, entry) rows and may add rows; it is never checked in."""
    rows = load_table(table_path)
    local = Path(table_path).resolve().parent / LOCAL_TABLE_NAME
    if not local.is_file():
        return rows
    overlay = {(r.config, r.entry): r for r in load_table(local)}
    merged = [overlay.pop((r.config, r.entry), r) for r in rows]
    merged.extend(overlay.values())
    return merged


def caps_values(ref_s, work_factor, scalar):
    """D-1 verbatim: work ratio = max(reference work scaled, 1); watchdog
    cap = max(ref_s * ratio * WATCHDOG_MULTIPLIER, MIN_WATCHDOG_S); budget
    = clamp(ref_s * ratio * scalar * SAFETY_MULTIPLIER, MIN_CAP_S, cap).
    The scalar scales the budget only -- the watchdog stays machine-free
    so a slow machine never loses its hang detection."""
    ratio = max(float(work_factor), 1.0)
    watchdog = max(ref_s * ratio * WATCHDOG_MULTIPLIER, float(MIN_WATCHDOG_S))
    budget = min(max(ref_s * ratio * scalar * SAFETY_MULTIPLIER,
                     float(MIN_CAP_S)), watchdog)
    return budget, watchdog


def resolve_row(rows, config, entry):
    """Entry scope first, then the `default` row of an e2e config (D-3);
    ctest configs list every entry per row and have no fallback."""
    for row in rows:
        if row.config == config and row.entry == entry:
            return row
    if config in E2E_CONFIGS:
        for row in rows:
            if row.config == config and row.entry == "default":
                return row
    raise KeyError("%s %s" % (config, entry))


# Scope-resolution mode for callers that cannot say which coarse scope
# owns an entry (the post-hoc checker: a junit row carries no manifest
# info): consult the scope rows in the table's fixed order.
ANY_MANIFEST = object()


def resolve_scoped_row(rows, config, entry, manifest=ANY_MANIFEST):
    """Row resolution widened to the coarse scopes (D-3/R-17), nearest
    scope first: entry row > owning manifest row > `default` row.
    `manifest` names the scope the entry comes from when the caller
    knows it (the e2e runner prices every entry against the manifest it
    is walking; a subset or renamed override manifest outside
    MANIFEST_SCOPES passes None, which means no scope row and
    entry > `default` only). ANY_MANIFEST keeps the legacy fixed order
    for callers that cannot tell the owner apart. ctest configs list
    every entry and carry no coarse row today, so an unknown name
    resolves to None there and the checker reports it missing rather
    than inventing a budget. The grading of what a reading can do (red
    versus advisory) lives in check_budget, not here."""
    for row in rows:
        if row.config == config and row.entry == entry:
            return row
    if manifest is not None:
        scopes = (MANIFEST_SCOPES if manifest is ANY_MANIFEST
                  else (manifest,))
        for scope in scopes:
            for row in rows:
                if row.config == config and row.entry == scope:
                    return row
    for row in rows:
        if row.config == config and row.entry == "default":
            return row
    return None


def caps(config, entry, work_factor=1.0, scalar=1.0, table_path=None):
    """(budget_s, watchdog_s) for one entry: the single place caps and
    budgets are computed (D-5)."""
    path = DEFAULT_TABLE if table_path is None else table_path
    row = resolve_row(effective_rows(path), config, entry)
    return caps_values(row.ref_s, work_factor, scalar)


def parse_claim_line(line):
    """Parse one tools/gate_tiers.txt claim line (D-11): `claim
    <resource-name> <ctest entry>...` naming the named resource every
    listed entry holds. Returns (resource-name, [entries]). Raises
    ValueError naming the line: the roster's shape is validated at every
    consumer (the tier_coverage_check guard and emit-cmake), never
    silently skipped."""
    if not CLAIM_LINE_RE.match(line):
        raise ValueError("malformed claim line: %r" % line)
    fields = line.split()
    return fields[1], fields[2:]


def read_claims(path):
    """The claim roster file as [(resource, [entries])] in file order:
    blank lines, comments and the `tier` lines (the coverage guard's own
    axis) are skipped; every claim line goes through parse_claim_line."""
    claims = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.split()[0] != "claim":
            continue
        claims.append(parse_claim_line(stripped))
    return claims


def emit_cmake(configs, out_path, work_factor=DEFAULT_WORK_FACTOR,
               table_path=DEFAULT_TABLE, claims=()):
    """Write the configure-time TIMEOUT cap file (D-1/D-2): one
    `set(GATE_TIMEOUT_<sanitized entry> <watchdog cap>)` line per entry
    with a row under any requested config key, the cap the largest
    watchdog cap over those keys (one TIMEOUT property cannot switch with
    ctest -C, so the widest of the tree's keys wins). Names sanitize every
    character outside [A-Za-z0-9_] to '_', the same rule the add_test
    wrapper applies when it looks the variable up. claims is the parsed
    claim roster (read_claims, D-11): each claimed entry gets one
    `set(GATE_RESOURCE_<sanitized entry> <resource names...>)` line in
    roster order -- the add_test wrapper turns that variable into the
    ctest RESOURCE_LOCK property -- whether or not the table holds a
    timing row for the entry, because a claimed window launcher must not
    run beside another one even when it is unmanaged for timing. The
    machine scale never participates: it reaches test processes only
    through the leg environment (D-4), so the file stays a pure function
    of the table, the work factor and the roster. A work factor below 1 is
    refused (D-8: the configure-time knob may widen caps above the
    committed table but never shrink them). An entry with no row under any
    requested key gets no TIMEOUT variable: the leg's ctest --timeout
    fallback owns it. Returns the number of TIMEOUT entries emitted; the
    resource lines derive from the roster, not from the caps, so they do
    not count."""
    try:
        factor = float(work_factor)
    except (TypeError, ValueError):
        raise ValueError("work factor %r is not a number" % (work_factor,))
    if factor < 1.0:
        raise ValueError(
            "work factor %r is below 1.0: the configure-time knob may "
            "widen the caps above the committed table but never shrink "
            "them (D-8)" % (work_factor,))
    keys = []
    # --config is repeatable and defaults to None: fold the absent case
    # into the named refusal below instead of a TypeError on None.
    for config in configs or ():
        if config not in CONFIG_KEYS:
            raise ValueError("unknown config key %r (legal keys: %s)"
                             % (config, ", ".join(CONFIG_KEYS)))
        if config not in keys:
            keys.append(config)
    if not keys:
        raise ValueError("emit-cmake needs at least one --config key")
    watchdogs = {}
    for row in effective_rows(Path(table_path)):
        if row.config not in keys or row.entry == "default":
            continue
        # caps() per row (never an inlined product): one formula site,
        # D-5. A few hundred rows re-resolving the table is milliseconds
        # at configure time.
        _budget, watchdog = caps(row.config, row.entry, work_factor=factor,
                                 table_path=table_path)
        prev = watchdogs.get(row.entry)
        if prev is None or watchdog > prev:
            watchdogs[row.entry] = watchdog
    lines = [
        "# Generated by tools/gate_time_check.py emit-cmake: the ctest",
        "# TIMEOUT watchdog caps derived from the committed reference",
        "# table (gate timing design D-1/D-2) and, from the claim",
        "# roster, the RESOURCE_LOCK resource variables (D-11). Edit the",
        "# table or the roster, not this file; the configure step",
        "# regenerates it whenever either changes.",
        "# configs: %s" % " ".join(keys),
        "# work_factor: %r" % factor,
    ]
    for entry in sorted(watchdogs):
        var = "GATE_TIMEOUT_" + re.sub(r"[^A-Za-z0-9_]", "_", entry)
        lines.append("set(%s %r)" % (var, watchdogs[entry]))
    # The claim axis (D-11): the entry -> resource names map in roster
    # order; CMake's set() turns the space-joined names into a list the
    # wrapper hands to RESOURCE_LOCK unchanged, so an entry under two
    # claims waits for both before it starts.
    resources = {}
    for claim, entries in claims or ():
        for entry in entries:
            holders = resources.setdefault(entry, [])
            if claim not in holders:
                holders.append(claim)
    for entry in sorted(resources):
        var = "GATE_RESOURCE_" + re.sub(r"[^A-Za-z0-9_]", "_", entry)
        lines.append("set(%s %s)" % (var, " ".join(resources[entry])))
    # Explicit newline="\n" (same reasoning as apply_record): the
    # generated file stays LF on every platform.
    with Path(out_path).open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return len(watchdogs)


def row_line(config, entry, ref_s, ref_work, form, sentinel, reason):
    # A row never carries a reason without a sentinel token in front of
    # it (the parser would have read the reason as the sentinel), so the
    # sentinel token gates the reason token.
    line = "%s %s %.2f %s %s" % (config, entry, ref_s, ref_work, form)
    if sentinel or reason:
        line += " %s" % (sentinel or "-")
    if reason:
        line += " %s" % reason
    return line


def form_token(config, parallel, repeat):
    """Guard ten's measurement-form token for a reading taken now: the
    family half says which product timed it (ctest junit versus the e2e
    runner's MEASURE rows), the mode half how the leg was driven. A
    reading may only be judged against a row recorded in the same form:
    a serial reference compared against parallel contention prices the
    contention into the machine scalar and misjudges both (D-2's last
    paragraph)."""
    family = "ctest-junit" if config.startswith("ctest-") else "e2e"
    mode = "-serial" if parallel <= 1 else "-parallel-%d" % parallel
    if repeat and repeat != "none":
        mode += "-repeat-%s" % repeat
    return family + mode
