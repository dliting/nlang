# gate_time_table.py -- the reference timing table model and the single
# caps()/budget derivation site (D-1/D-3/D-5/D-7;
# docs/dev/gate_timing_design.md). load_table parses one table file,
# effective_rows folds in the machine-local overlay, caps() and
# caps_values compute the watchdog cap and the budget in exactly one
# place, resolve_row/resolve_scoped_row map a (config, entry) pair to
# its row, row_line/form_token print table-shape rows and measurement
# form tokens, and emit_cmake derives the configure-time ctest TIMEOUT
# caps file. The CLI entry and the guard verdict bodies live in the
# sibling gate_time_check / gate_time_measure / gate_time_guards
# modules. The watchdog cap and the budget are computed in exactly one
# place, caps(), per D-5. Stdlib only; run with -I.
#
# Importable consumers (D-5): caps(config, entry, work_factor=1.0,
# scalar=1.0) -> (budget_s, watchdog_s) and load_table(path).

import re
from collections import namedtuple
from pathlib import Path

# Frozen values pinned by the design (D-1/D-3/D-7); changing one changes
# the caps() contract every consumer shares.
SAFETY_MULTIPLIER = 2
WATCHDOG_MULTIPLIER = 10
MIN_CAP_S = 60
MIN_WATCHDOG_S = 120
SCALE_BAND_MIN = 0.25
SCALE_BAND_MAX = 4.0
DEFAULT_WORK_FACTOR = 1
SLOW_MIN_REF_S = 300

SENTINEL_MARK = "yes"
SENTINELS_PER_CONFIG = (5, 8)
CTEST_CONFIGS = ("ctest-Debug", "ctest-IDE", "ctest-ASan")
E2E_CONFIGS = ("e2e-Debug", "e2e-ASan", "e2e-Release")
CONFIG_KEYS = CTEST_CONFIGS + ("ctest-Release",) + E2E_CONFIGS
CATEGORIES = ("常规", "慢功能", "演示验收", "性能")
MANIFEST_SCOPES = ("manifest.txt", "examples_manifest.txt")
LOCAL_TABLE_NAME = "gate_time_reference.local.txt"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TABLE = REPO_ROOT / "tests" / "gate_time_reference.txt"
# Each decision tree feeds exactly one ctest config key (D-2).
DECISION_TREES = {"ctest-Debug": "build",
                  "ctest-IDE": "build-ide",
                  "ctest-ASan": "build-asan"}

Row = namedtuple("Row", "config entry ref_s ref_work category form sentinel "
                        "reason line")


def load_table(path):
    """Parse one reference table file. One row per (config, entry),
    whitespace-separated: config entry ref_s ref_work category form
    [sentinel [reason]]; `reason` runs to end of line, so a row with seven
    tokens carries no sentinel and a row with six neither. Blank lines and
    `#` comments are skipped. Raises ValueError on a malformed row."""
    rows = []
    text = Path(path).read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split(None, 7)
        if len(fields) < 6:
            raise ValueError("%s line %d: expected at least six columns, "
                             "got %d" % (path, lineno, len(fields)))
        config, entry, ref_s, ref_work, category, form = fields[:6]
        sentinel = fields[6] if len(fields) > 6 else ""
        reason = fields[7] if len(fields) > 7 else ""
        if config not in CONFIG_KEYS:
            raise ValueError("%s line %d: unknown config key %r (want one "
                             "of %s)" % (path, lineno, config,
                                         ", ".join(CONFIG_KEYS)))
        if category not in CATEGORIES:
            raise ValueError("%s line %d: unknown category %r (want one of "
                             "%s)" % (path, lineno, category,
                                      ", ".join(CATEGORIES)))
        try:
            ref_s = float(ref_s)
            ref_work = int(ref_work)
        except ValueError:
            raise ValueError("%s line %d: ref_s/ref_work must be numeric"
                             % (path, lineno))
        if ref_s < 0.0 or ref_work < 1:
            raise ValueError("%s line %d: ref_s must be >= 0 and ref_work "
                             ">= 1" % (path, lineno))
        rows.append(Row(config, entry, ref_s, ref_work, category, form,
                        sentinel, reason, lineno))
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


def emit_cmake(configs, out_path, work_factor=DEFAULT_WORK_FACTOR,
               table_path=DEFAULT_TABLE):
    """Write the configure-time TIMEOUT cap file (D-1/D-2): one
    `set(GATE_TIMEOUT_<sanitized entry> <watchdog cap>)` line per entry
    with a row under any requested config key, the cap the largest
    watchdog cap over those keys (one TIMEOUT property cannot switch with
    ctest -C, so the widest of the tree's keys wins). Names sanitize every
    character outside [A-Za-z0-9_] to '_', the same rule the add_test
    wrapper applies when it looks the variable up. The machine scale never
    participates: it reaches test processes only through the leg
    environment (D-4), so the file stays a pure function of the table plus
    the work factor. A work factor below 1 is refused (D-8: the
    configure-time knob may widen caps above the committed table but never
    shrink them). An entry with no row under any requested key gets no
    variable at all: the leg's ctest --timeout fallback owns it. Returns
    the number of entries emitted."""
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
    for config in configs:
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
        "# table (gate timing design D-1/D-2). Edit the table, not this",
        "# file; the configure step regenerates it whenever the table",
        "# changes.",
        "# configs: %s" % " ".join(keys),
        "# work_factor: %r" % factor,
    ]
    for entry in sorted(watchdogs):
        var = "GATE_TIMEOUT_" + re.sub(r"[^A-Za-z0-9_]", "_", entry)
        lines.append("set(%s %r)" % (var, watchdogs[entry]))
    # Explicit newline="\n" (same reasoning as apply_record): the
    # generated file stays LF on every platform.
    with Path(out_path).open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return len(watchdogs)


def row_line(config, entry, ref_s, ref_work, category, form, sentinel,
             reason):
    # A row never carries a reason without a sentinel token in front of
    # it (the parser would have read the reason as the sentinel), so the
    # sentinel token gates the reason token.
    line = "%s %s %.2f %s %s %s" % (config, entry, ref_s, ref_work,
                                    category, form)
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
