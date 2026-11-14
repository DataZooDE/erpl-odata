#!/usr/bin/env python3
"""scripts/check_doc_snippets.py

Docs/skills SQL snippet and name checker for the erpl_odata DuckDB extension.
Ported from anofox-forecast's checker. Unlike there, it also runs in CI (the
`docs-check` job in .github/workflows/MainDistributionPipeline.yml).

Usage:
    python3 scripts/check_doc_snippets.py [FILE ...]
    python3 scripts/check_doc_snippets.py --changed-since REF
    python3 scripts/check_doc_snippets.py --binary PATH --extension PATH [--out PATH]

Default scan: README.md, docs/**/*.md and plugins/erpl-odata/skills/**/*.md.

Binary: --binary PATH, else $ERPL_DOCS_DUCKDB, else build/release/duckdb, else
build/debug/duckdb (both have erpl_odata statically linked). With --extension PATH
every run starts with `LOAD '<PATH>'` -- this is how CI checks a stock DuckDB CLI
against the freshly built loadable artifact.

What is checked
1. Names. Every identifier with an erpl prefix (http_, odata_, odp_odata_,
   datasphere_, sac_, graph_, bc_, crm_, delta_share_, erpl_) that appears in a
   SKILL.md frontmatter description, in a backtick code span, or in call syntax
   `name(` must be a registered function/pragma (duckdb_functions()) or setting
   (duckdb_settings()), or be listed in scripts/doc_snippets/removed_names.txt.
   A few backticked words with an erpl prefix are not functions (for example
   `odata_storage`, a source file); list those in removed_names.txt too.
2. Named parameters. Every `name := value` inside a call to a registered erpl
   function must be one of that function's parameters, as reported by
   duckdb_functions().parameters (which include named parameters).
3. SQL blocks. Every ```sql fence is run in :memory: with a temp HOME and cwd.
   Blocks that would reach a remote system are never executed: anything with a
   URL, INSTALL, ATTACH, COPY ... TO, a CREATE SECRET using a token-fetching
   provider, or a call to an erpl function that does I/O. Those blocks are
   instead parsed statement by statement with json_serialize_sql (SELECT-like
   statements only), so syntax errors are still caught (status PARSED).

Status per block: PASS, FAIL, EXPECTED-ERROR (key in expected_errors.txt; the
block must still fail), SYNOPSIS (key in synopsis.txt; a signature placeholder,
not executed), PARSED (network block whose SELECTs parse), SKIP-NETWORK (network
block with nothing parseable), SKIP-VERSION (block whose first line is
`-- DuckDB 2.0+` on an older binary). A key is `<file>\\t<first non-comment line>`.

On a full scan, stale entries in expected_errors.txt / synopsis.txt /
removed_names.txt are reported as STALE.

Writes a TSV (status, file, line, first SQL line, detail) to --out (default
build/doc-snippets.tsv) and prints a summary. Exit 0 clean, 1 on FAIL/STALE,
2 if no binary or the surface query fails.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = subprocess.run(
    ["git", "rev-parse", "--show-toplevel"], cwd=SCRIPT_DIR, capture_output=True, text=True, check=True
).stdout.strip()

SUPPORT_DIR = os.path.join(SCRIPT_DIR, "doc_snippets")
FIXTURES_FILE = os.path.join(SUPPORT_DIR, "fixtures.sql")
EXPECTED_ERRORS_FILE = os.path.join(SUPPORT_DIR, "expected_errors.txt")
SYNOPSIS_FILE = os.path.join(SUPPORT_DIR, "synopsis.txt")
REMOVED_NAMES_FILE = os.path.join(SUPPORT_DIR, "removed_names.txt")
DEFAULT_OUT = os.path.join(REPO_ROOT, "build", "doc-snippets.tsv")

DEFAULT_SCAN_DIRS = ["docs", os.path.join("plugins", "erpl-odata", "skills")]
# Internal engineering notes, not user documentation.
EXCLUDED_FILES = {os.path.join("docs", "CODE_QUALITY_REFACTORING.md")}

PREFIX = r"(?:http|odata|odp_odata|datasphere|sac|graph|bc|crm|delta_share|erpl)_[a-z0-9_]+"
IDENT_RE = re.compile(r"\b(" + PREFIX + r")\b")
BACKTICK_IDENT_RE = re.compile(r"`(" + PREFIX + r")`")
CALL_IDENT_RE = re.compile(r"\b(" + PREFIX + r")\s*\(")
# `CREATE SECRET erpl_x (`, `CREATE TABLE odata_x (` and friends name objects, not calls.
DEFINITION_BEFORE_RE = re.compile(r"\b(SECRET|TABLE|VIEW|MACRO|EXISTS|INTO|AS|TYPE)\s+$", re.IGNORECASE)


def call_names(text):
    """Identifiers used in call syntax, excluding object names in DDL."""
    names = []
    for m in CALL_IDENT_RE.finditer(text):
        if DEFINITION_BEFORE_RE.search(text[max(0, m.start() - 40) : m.start()]):
            continue
        names.append((m.group(1), m.start()))
    return names

# erpl functions that never leave the process; everything else with an erpl
# prefix is assumed to do I/O and makes a block network-only.
LOCAL_FUNCTIONS = {
    "odp_odata_list_subscriptions",
    "odp_odata_remove_subscription",
    "erpl_trace_enable",
    "erpl_trace_level",
    "erpl_trace_directory",
    "erpl_trace_status",
}

FENCE_RE = re.compile(r"^```[ \t]*([A-Za-z0-9_+-]*)[ \t]*$")
SETUP_RE = re.compile(
    r"\b(CREATE\s+(OR\s+REPLACE\s+)?(TEMP\s+|TEMPORARY\s+)?(TABLE|VIEW|MACRO)|INSERT)\b",
    re.IGNORECASE,
)
CREATE_NAME_RE = re.compile(
    r"\bCREATE\s+(?:OR\s+REPLACE\s+)?(?:TEMP\s+|TEMPORARY\s+)?(?:TABLE|VIEW|MACRO)\s+"
    r"(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*)",
    re.IGNORECASE,
)
INSERT_NAME_RE = re.compile(r"\bINSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
PLACEHOLDER_ELLIPSIS_RE = re.compile(r"(?<!\d)\.\.\.(?!\d)")
NETWORK_RE = re.compile(
    r"\bINSTALL\b|https?://|s3://|\bATTACH\b|\bCOPY\b[\s\S]*?\bTO\b"
    r"|\bPROVIDER\s+(?:oauth2|client_credentials|authorization_code|access_token)\b",
    re.IGNORECASE,
)
# A named argument: `name := value` or `name = value` at the start of a top-level argument.
NAMED_PARAM_RE = re.compile(r"^\s*([a-z_][a-z0-9_]*)\s*(?::=|=(?!=))")
ATTACH_NAME_RE = re.compile(r"\bATTACH\b[^;]*?\bAS\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
V2_ONLY_RE = re.compile(r"\A\s*--\s*DuckDB 2\.0\+")
PARSEABLE_RE = re.compile(r"\A\s*(SELECT|WITH|FROM|VALUES|\()", re.IGNORECASE)


class Runner:
    def __init__(self, binary, extension):
        self.binary = binary
        self.preamble = "SET autoinstall_known_extensions = false;\n"
        if extension:
            self.preamble += "LOAD '%s';\n" % extension.replace("'", "''")

    def run(self, sql_text, timeout=120):
        tmp_dir = tempfile.mkdtemp(prefix="doc_snippets_")
        try:
            env = dict(os.environ)
            env["HOME"] = tmp_dir
            # Keep snippet runs out of the product telemetry.
            env["DATAZOO_DISABLE_TELEMETRY"] = "1"
            full_sql = self.preamble + "SET erpl_telemetry_enabled = false;\n" + sql_text
            try:
                result = subprocess.run(
                    [self.binary, "-unsigned", "-bail", "-list", ":memory:"],
                    input=full_sql,
                    cwd=tmp_dir,
                    capture_output=True,
                    text=True,
                    timeout=timeout,
                    env=env,
                )
                return result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired:
                return 124, "", f"TIMEOUT after {timeout}s"
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def query_lines(self, sql_text):
        rc, out, err = self.run(sql_text)
        if rc != 0:
            print(f"ERROR: surface query failed: {(err.strip() or out.strip())[:500]}", file=sys.stderr)
            sys.exit(2)
        return [line for line in out.splitlines() if line.strip()]


def read_text(abs_path):
    with open(abs_path, encoding="utf-8") as f:
        return f.read()


def changed_md_files(ref):
    result = subprocess.run(
        ["git", "diff", "--name-only", ref, "--", "README.md"] + DEFAULT_SCAN_DIRS,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    files = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return [
        f
        for f in files
        if f.endswith(".md") and f not in EXCLUDED_FILES and os.path.exists(os.path.join(REPO_ROOT, f))
    ]


def default_scan_files():
    files = ["README.md"] if os.path.exists(os.path.join(REPO_ROOT, "README.md")) else []
    for scan_dir in DEFAULT_SCAN_DIRS:
        abs_dir = os.path.join(REPO_ROOT, scan_dir)
        if not os.path.isdir(abs_dir):
            continue
        for root, dirs, fnames in os.walk(abs_dir):
            dirs.sort()
            for fn in sorted(fnames):
                rel = os.path.relpath(os.path.join(root, fn), REPO_ROOT)
                if fn.endswith(".md") and rel not in EXCLUDED_FILES:
                    files.append(rel)
    return files


def extract_sql_blocks(text):
    """Return list of (block_text, start_line_idx) for ```sql fenced blocks."""
    lines = text.splitlines()
    blocks = []
    i = 0
    while i < len(lines):
        m = FENCE_RE.match(lines[i].strip())
        if m and m.group(1).lower() == "sql":
            start = i
            body_lines = []
            i += 1
            while i < len(lines) and lines[i].strip() != "```":
                body_lines.append(lines[i])
                i += 1
            blocks.append(("\n".join(body_lines), start))
        i += 1
    return blocks


def first_non_comment_line(block_text):
    for line in block_text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("--"):
            return stripped
    for line in block_text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def load_key_set(path):
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as f:
        return {line.rstrip("\n") for line in f if line.strip() and not line.startswith("#")}


def load_removed_names(path):
    """name<TAB>reason, one per line; blank and '#' lines ignored."""
    names = {}
    if not os.path.exists(path):
        return names
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.strip().startswith("#"):
                continue
            parts = line.split("\t", 1)
            if parts[0].strip():
                names[parts[0].strip()] = parts[1].strip() if len(parts) > 1 else ""
    return names


def strip_sql_comments(text):
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


def split_statements(sql_text):
    """Split on top-level ';', respecting quotes, $$ strings and -- comments."""
    statements, current, i, n = [], [], 0, len(sql_text)
    while i < n:
        ch = sql_text[i]
        if sql_text.startswith("--", i):
            end = sql_text.find("\n", i)
            end = n if end == -1 else end
            i = end
            continue
        if sql_text.startswith("$$", i):
            end = sql_text.find("$$", i + 2)
            end = n if end == -1 else end + 2
            current.append(sql_text[i:end])
            i = end
            continue
        if ch in ("'", '"'):
            j = i + 1
            while j < n:
                if sql_text[j] == ch:
                    if j + 1 < n and sql_text[j + 1] == ch:
                        j += 2
                        continue
                    break
                j += 1
            current.append(sql_text[i : j + 1])
            i = j + 1
            continue
        if ch == ";":
            stmt = "".join(current).strip()
            if stmt:
                statements.append(stmt)
            current = []
        else:
            current.append(ch)
        i += 1
    stmt = "".join(current).strip()
    if stmt:
        statements.append(stmt)
    return statements


def setup_created_name(block_text):
    m = CREATE_NAME_RE.search(block_text) or INSERT_NAME_RE.search(block_text)
    return m.group(1) if m else None


def select_required_setup(target_text, setup_accum, fixture_names=()):
    """Transitive closure of earlier setup blocks referenced by target_text."""
    shadowed = set(CREATE_NAME_RE.findall(strip_sql_comments(target_text))) | set(fixture_names)
    needed_text = [strip_sql_comments(target_text)]
    included, included_names = [], set()
    changed = True
    while changed:
        changed = False
        combined = "\n".join(needed_text)
        for name, block_text in setup_accum:
            if name in included_names or name in shadowed:
                continue
            if re.search(r"\b" + re.escape(name) + r"\b", combined):
                included.append(block_text)
                included_names.add(name)
                needed_text.append(strip_sql_comments(block_text))
                changed = True
    order = {id(bt): i for i, (_, bt) in enumerate(setup_accum)}
    included.sort(key=lambda bt: order.get(id(bt), 0))
    return included


def first_error_line(stderr, stdout):
    for stream in (stderr, stdout):
        for line in stream.splitlines():
            if line.strip():
                return line.strip()[:200]
    return ""


def is_network_block(block_text, registered, network_names=()):
    """network_names: tables/catalogs created earlier in the file by network-only blocks;
    a block that reads them cannot run either."""
    code = strip_sql_comments(block_text)
    if NETWORK_RE.search(code):
        return True
    own = set(CREATE_NAME_RE.findall(code))
    for name in network_names:
        if name not in own and re.search(r"\b" + re.escape(name) + r"\b", code):
            return True
    for name, _ in call_names(code):
        if name in registered and name not in LOCAL_FUNCTIONS:
            return True
    return False


def parse_check(runner, block_text):
    """Return (n_parsed, error) for the SELECT-like statements of a block."""
    # CLI dot commands (.mode, .once, .read) are not SQL
    sql_only = "\n".join(l for l in block_text.splitlines() if not l.lstrip().startswith("."))
    stmts = [s for s in split_statements(sql_only) if PARSEABLE_RE.match(s)]
    if not stmts:
        return 0, ""
    probe = "\n".join(
        "SELECT json_serialize_sql(%s);" % ("$erpl$" + s + "$erpl$") for s in stmts
    )
    rc, out, err = runner.run(probe)
    if rc != 0:
        return 0, first_error_line(err, out)
    for stmt, line in zip(stmts, [l for l in out.splitlines() if l.strip().startswith("{")]):
        try:
            parsed = json.loads(line)
        except ValueError:
            continue
        if parsed.get("error"):
            return 0, "parse error: %s (in: %s)" % (parsed.get("error_message", "")[:150], stmt[:60])
    return len(stmts), ""


def matching_paren(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def check_named_params(text, params_by_function):
    """Yield (function, param, offset) for unknown named parameters."""
    code = strip_sql_comments(text)
    for m in CALL_IDENT_RE.finditer(code):
        name = m.group(1)
        if name not in params_by_function:
            continue
        open_idx = m.end() - 1
        args = code[open_idx + 1 : matching_paren(code, open_idx)]
        # Only top-level named params of this call, not of nested calls.
        depth, top_level = 0, []
        for ch in args:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            top_level.append(ch if depth == 0 else " ")
        for arg in "".join(top_level).split(","):
            pm = NAMED_PARAM_RE.match(arg)
            if pm and pm.group(1) not in params_by_function[name]:
                yield name, pm.group(1), m.start()


def collect_name_mentions(rel_path, text):
    """(name, line_no) mentions: bare tokens in SKILL.md frontmatter, backtick
    spans and call syntax everywhere else."""
    mentions = []
    lines = text.splitlines()
    frontmatter_end = 0
    is_skill = rel_path.endswith("SKILL.md")
    if is_skill and lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                frontmatter_end = i
                break
        for i in range(1, frontmatter_end):
            if lines[i].startswith("name:"):
                continue
            for m in IDENT_RE.finditer(lines[i]):
                mentions.append((m.group(1), i + 1))
    for i, line in enumerate(lines):
        if is_skill and i < frontmatter_end:
            continue
        for m in BACKTICK_IDENT_RE.finditer(line):
            mentions.append((m.group(1), i + 1))
        for name, _ in call_names(line):
            mentions.append((name, i + 1))
    return mentions


def resolve_binary(explicit):
    for c in (explicit, os.environ.get("ERPL_DOCS_DUCKDB")):
        if c:
            return c if os.path.exists(c) else None
    # Both builds link erpl_odata statically; a stale one checks the docs against old code,
    # so take whichever was built last.
    builds = [os.path.join(REPO_ROOT, "build", b, "duckdb") for b in ("release", "debug")]
    builds = [b for b in builds if os.path.exists(b)]
    return max(builds, key=os.path.getmtime) if builds else None


def probe_named_parameters(runner, function_name, max_arity):
    """Named parameters of a table function, read from DuckDB's own binder error.

    duckdb_functions() does not tell positional from named parameters, but binding a
    call with an unknown named parameter fails before the function's bind runs (so no
    network) with "Invalid named parameter ... Candidates:" followed by the full list.
    The positional arity is unknown, so each arity is tried until the binder gets that far.
    """
    found = set()
    for arity in range(0, max_arity + 1):
        args = ", ".join(["NULL"] * arity + ["__erpl_probe__ := 1"])
        rc, out, err = runner.run(f"SELECT * FROM {function_name}({args});\n", timeout=60)
        text = err + out
        if "Invalid named parameter" not in text:
            continue
        in_candidates = False
        for line in text.splitlines():
            if line.strip().startswith("Candidates:"):
                in_candidates = True
                continue
            if in_candidates:
                m = re.match(r"^\s+([A-Za-z_][A-Za-z0-9_]*)\s+\S", line)
                if not m:
                    break
                found.add(m.group(1))
        return found
    return found


def query_surface(runner):
    rows = runner.query_lines(
        "COPY (SELECT function_name, function_type, len(parameters) AS n FROM duckdb_functions() "
        "WHERE regexp_matches(function_name, '^" + PREFIX + "$')) TO '/dev/stdout' (FORMAT json);\n"
    )
    arity = {}
    for line in rows:
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if row["function_type"] == "table":
            arity[row["function_name"]] = max(arity.get(row["function_name"], 0), int(row["n"] or 0))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        futures = {name: pool.submit(probe_named_parameters, runner, name, n) for name, n in arity.items()}
        params = {name: f.result() for name, f in futures.items()}
    names = set(runner.query_lines("SELECT DISTINCT function_name FROM duckdb_functions();\n"))
    names |= set(runner.query_lines("SELECT name FROM duckdb_settings();\n"))
    # Identifiers that are not functions but legitimately appear in docs
    names |= set(runner.query_lines("SELECT type FROM duckdb_secret_types();\n"))
    names |= set(runner.query_lines("SELECT extension_name FROM duckdb_extensions();\n"))
    return names, params


def parse_args(argv):
    opts = {"files": [], "changed_since": None, "out": DEFAULT_OUT, "binary": None, "extension": None}
    flags = {"--changed-since": "changed_since", "--out": "out", "--binary": "binary", "--extension": "extension"}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in flags:
            if i + 1 >= len(argv):
                print(f"ERROR: {a} requires an argument", file=sys.stderr)
                sys.exit(1)
            opts[flags[a]] = argv[i + 1]
            i += 2
            continue
        opts["files"].append(a)
        i += 1
    return opts


def main():
    opts = parse_args(sys.argv[1:])
    binary = resolve_binary(opts["binary"])
    if not binary:
        print("ERROR: no duckdb binary found -- build it (make dev / make release) or pass --binary", file=sys.stderr)
        return 2
    runner = Runner(binary, opts["extension"])

    is_full_scan = not opts["files"] and not opts["changed_since"]
    if opts["files"]:
        files = opts["files"]
    elif opts["changed_since"]:
        files = changed_md_files(opts["changed_since"])
    else:
        files = default_scan_files()

    fixtures = read_text(FIXTURES_FILE) if os.path.exists(FIXTURES_FILE) else ""
    fixture_names = set(CREATE_NAME_RE.findall(strip_sql_comments(fixtures)))
    expected_errors = load_key_set(EXPECTED_ERRORS_FILE)
    synopsis_keys = load_key_set(SYNOPSIS_FILE)
    removed_names = load_removed_names(REMOVED_NAMES_FILE)
    registered, params_by_function = query_surface(runner)
    version_lines = runner.query_lines("SELECT version();\n")
    vm = re.search(r"v(\d+)\.", " ".join(version_lines))
    duckdb_major = int(vm.group(1)) if vm else 0

    rows = []
    counts = {k: 0 for k in ("PASS", "FAIL", "EXPECTED-ERROR", "SYNOPSIS", "PARSED", "SKIP-NETWORK", "SKIP-VERSION", "STALE")}
    all_block_keys, all_mentioned, seen_unknown = set(), set(), set()

    def add(status, rel, line, first, detail):
        rows.append((status, rel, str(line), first, detail))
        counts[status] += 1

    for rel_path in files:
        abs_path = os.path.join(REPO_ROOT, rel_path)
        if not os.path.exists(abs_path):
            continue
        text = read_text(abs_path)

        for name, line_no in collect_name_mentions(rel_path, text):
            all_mentioned.add(name)
            if name in registered or name in removed_names or (rel_path, name) in seen_unknown:
                continue
            if name.endswith("_") and any(r.startswith(name) for r in registered):
                continue  # a prefix such as `erpl_trace_*`
            seen_unknown.add((rel_path, name))
            add("FAIL", rel_path, line_no, "", f"unknown function or setting name {name}")

        setup_accum = []
        network_names = set()
        for block_text, start_line in extract_sql_blocks(text):
            first = first_non_comment_line(block_text)
            key = f"{rel_path}\t{first}"
            all_block_keys.add(key)
            line_no = start_line + 1

            for fn, param, _ in check_named_params(block_text, params_by_function):
                add("FAIL", rel_path, line_no, first[:100], f"{fn} has no named parameter '{param}'")

            if key in synopsis_keys:
                add("SYNOPSIS", rel_path, line_no, first[:100], "signature placeholder, not executed")
                continue
            if V2_ONLY_RE.search(block_text) and duckdb_major < 2:
                add("SKIP-VERSION", rel_path, line_no, first[:100], "DuckDB 2.0+ block, binary is older")
                continue
            if is_network_block(block_text, registered, network_names):
                code = strip_sql_comments(block_text)
                network_names |= set(CREATE_NAME_RE.findall(code)) | set(ATTACH_NAME_RE.findall(code))
                n_parsed, err = parse_check(runner, block_text)
                if err:
                    add("FAIL", rel_path, line_no, first[:100], err)
                elif n_parsed:
                    add("PARSED", rel_path, line_no, first[:100], f"{n_parsed} statement(s) parsed, not executed")
                else:
                    add("SKIP-NETWORK", rel_path, line_no, first[:100], "reaches a remote system, not executed")
                continue

            full_sql = fixtures
            for setup_sql in select_required_setup(block_text, setup_accum, fixture_names):
                full_sql += "\n" + setup_sql
            full_sql += "\n" + block_text
            rc, stdout, stderr = runner.run(full_sql)
            if rc == 0:
                if key in expected_errors:
                    add("FAIL", rel_path, line_no, first[:100], "expected error but passed")
                else:
                    add("PASS", rel_path, line_no, first[:100], "")
            elif key in expected_errors:
                add("EXPECTED-ERROR", rel_path, line_no, first[:100], "")
            else:
                add("FAIL", rel_path, line_no, first[:100], first_error_line(stderr, stdout))

            if SETUP_RE.search(block_text) and not PLACEHOLDER_ELLIPSIS_RE.search(block_text):
                name = setup_created_name(block_text)
                if name:
                    setup_accum.append((name, block_text))

    if is_full_scan:
        for label, keys in (("expected_errors.txt", expected_errors), ("synopsis.txt", synopsis_keys)):
            for key in sorted(keys - all_block_keys):
                add("STALE", key.split("\t", 1)[0], "", "", f"stale {label} entry: {key}")
        for name in sorted(removed_names):
            if name in registered:
                add("STALE", "scripts/doc_snippets/removed_names.txt", "", "", f"{name} is registered")
            elif name not in all_mentioned:
                add("STALE", "scripts/doc_snippets/removed_names.txt", "", "", f"{name} is no longer mentioned")

    os.makedirs(os.path.dirname(opts["out"]) or ".", exist_ok=True)
    with open(opts["out"], "w", encoding="utf-8") as f:
        for row in rows:
            f.write("\t".join(row) + "\n")
    for row in rows:
        if row[0] in ("FAIL", "STALE"):
            print("\t".join(row))
    print("check_doc_snippets: " + ", ".join(f"{v} {k}" for k, v in counts.items()) + f" (binary: {binary})")
    return 1 if (counts["FAIL"] or counts["STALE"]) else 0


if __name__ == "__main__":
    sys.exit(main())
