#!/usr/bin/env bash
# Run live/integration SQLLogicTest files so that a real failure cannot pass as a skip.
#
# usage: scripts/run_live_sql.sh <test file>...
#   UNITTEST=path/to/unittest   runner to use (default: build/debug/test/unittest)
#
# Why this exists (GitHub #276). DuckDB's unittest has a built-in skip list,
# TestConfiguration::ErrorMessagesToBeSkipped() in duckdb/test/helpers/test_config.cpp:
# when no test config overrides `skip_error_messages`, ANY error whose text contains
# "HTTP" or "Unable to connect" turns the whole file into a SKIP, and a skip exits 0.
# Nearly every failure of a live test against Graph, Business Central, Datasphere or SAP
# carries "HTTP" in its message ("Failed to read connection error for HTTP GET to ..."),
# so a Graph 500 on sites?search was reported as
#
#     All tests were skipped (total skipped 1)
#     skip on error_message matching 'HTTP': 1
#
# in the middle of an otherwise green `make test_debug_ms`. Two guards, both needed:
#
#  1. The runner gets a --test-config whose skip_error_messages is a sentinel that never
#     matches, which REPLACES the default list. An HTTP error is then an ordinary failure
#     with the real error text.
#  2. Each file runs on its own and its output is inspected. A skip is accepted only when
#     every reason the runner prints is `require-env VAR [VALUE]` and that condition really
#     is unmet in this environment - i.e. the test's credentials are absent. Any other skip
#     (an error-message skip, an unmet `require <extension>`, a require-env that IS met) is
#     a failure. "No test cases matched" is a failure too: unittest exits 0 for it.
#
# So a developer without credentials still gets a clean run (every file skips on its
# require-env), and a developer WITH credentials can no longer get a false green.
# Ported from duckdb-gdrive's scripts/run_live_tests.sh.
set -uo pipefail

cd "$(dirname "$0")/.."

UNITTEST="${UNITTEST:-build/debug/test/unittest}"
if [[ ! -x "$UNITTEST" ]]; then
    echo "FAIL: $UNITTEST is not built. Run 'make unittest' first." >&2
    exit 1
fi
if [[ $# -eq 0 ]]; then
    echo "usage: $0 <test file>..." >&2
    exit 2
fi

config="$(mktemp -t erpl-live-test-config-XXXXXX.json)"
trap 'rm -f "$config"' EXIT
# A non-empty list that never matches: present => the defaults ("HTTP",
# "Unable to connect") are not used at all.
cat >"$config" <<'JSON'
{
  "description": "erpl-odata live run: an HTTP error is a failure, not a skip (#276)",
  "skip_error_messages": ["__erpl_odata_live_never_skip_on_error__"]
}
JSON

# Is `require-env VAR [VALUE]` unmet here? sqllogictest skips when getenv() returns
# null, or, with a VALUE, when the variable differs from it. An exported but EMPTY
# variable is "set" and does not skip.
require_env_unmet() {
    local var="$1" want="${2-}"
    if [[ -z "${!var+x}" ]]; then
        return 0
    fi
    if [[ -n "$want" && "${!var}" != "$want" ]]; then
        return 0
    fi
    return 1
}

failed=()
skipped=()
passed=()
for t in "$@"; do
    echo "--- $t"
    if [[ ! -f "$t" ]]; then
        echo "FAIL: $t does not exist." >&2
        failed+=("$t")
        continue
    fi
    out="$("$UNITTEST" --test-config "$config" "$t" 2>&1)"
    rc=$?
    printf '%s\n' "$out"

    if [[ $rc -ne 0 ]]; then
        failed+=("$t")
        continue
    fi
    if grep -qE 'No test cases matched|No tests ran' <<<"$out"; then
        echo "FAIL: $t matched NO test cases - nothing was executed (unittest exits 0 for this)." >&2
        failed+=("$t")
        continue
    fi
    if grep -qE 'All tests were skipped|Skipped tests for the following reasons' <<<"$out"; then
        # The reason lines look like "require-env ERPL_MS_TENANT_ID: 1".
        reasons="$(sed -n '/Skipped tests for the following reasons:/,$p' <<<"$out" \
            | sed '1d' | grep -E ': [0-9]+$' | sed -E 's/: [0-9]+$//')"
        bad=""
        while IFS= read -r reason; do
            [[ -z "$reason" ]] && continue
            read -r kind var want _ <<<"$reason"
            if [[ "$kind" == "require-env" && -n "${var:-}" ]] && require_env_unmet "$var" "${want:-}"; then
                continue
            fi
            bad+="      $reason"$'\n'
        done <<<"$reasons"
        if [[ -z "$reasons" ]]; then
            bad="      (the runner printed no reason)"$'\n'
        fi
        if [[ -n "$bad" ]]; then
            echo "FAIL: $t was SKIPPED for a reason other than absent credentials:" >&2
            printf '%s' "$bad" >&2
            echo "      A skip here is a false green: the file's credentials are present, or the" >&2
            echo "      skip is an error-message skip. See GitHub #276." >&2
            failed+=("$t")
        else
            echo "SKIP: $t - its credentials are not set (require-env)."
            skipped+=("$t")
        fi
        continue
    fi
    passed+=("$t")
done

echo "========================================"
echo "live SQL: ${#passed[@]} passed, ${#skipped[@]} skipped (credentials absent), ${#failed[@]} failed"
for t in "${skipped[@]}"; do echo "  skipped: $t"; done
for t in "${failed[@]}"; do echo "  FAILED:  $t"; done
[[ ${#failed[@]} -eq 0 ]]
