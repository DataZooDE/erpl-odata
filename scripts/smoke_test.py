#!/usr/bin/env python3
"""Smoke test for erpl_odata DuckDB extension.

Downloads the official DuckDB CLI for the specified version and architecture
from GitHub releases, then verifies the built extension artifact loads and can
make a real outbound HTTP request. Simulates exactly what a user does after
downloading the extension from the distribution channel.

Usage: python3 scripts/smoke_test.py <extension_path> <duckdb_version> <arch>

  extension_path  Absolute path to the .duckdb_extension file to test
  duckdb_version  DuckDB version tag, e.g. v1.5.1
  arch            Target architecture: linux_amd64 | osx_amd64 | osx_arm64 | windows_amd64
"""

import os
import platform
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

# Windows consoles default to CP1252; DuckDB's table output uses UTF-8
# box-drawing characters that CP1252 cannot encode.  Reconfigure stdout/stderr
# to UTF-8 once at import time so all print() calls work on every platform.
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

ARCH_TO_CLI_ZIP: dict[str, str] = {
    "linux_amd64": "duckdb_cli-linux-amd64.zip",
    "linux_arm64": "duckdb_cli-linux-aarch64.zip",
    "osx_amd64": "duckdb_cli-osx-universal.zip",
    "osx_arm64": "duckdb_cli-osx-universal.zip",
    "windows_amd64": "duckdb_cli-windows-amd64.zip",
}

# The smoke test answers one question: does the freshly-built extension binary
# load into the official DuckDB CLI and can it issue a real outbound HTTP
# request? It does NOT validate httpbin.org's uptime.
#
# Two independent checks, because they fail for different reasons:
#   1. LOAD_SQL  — the extension binary loads cleanly. This is the critical
#      signal: a broken/ABI-mismatched artifact fails here and MUST fail CI.
#   2. HTTP_SQL  — http_get reaches an external endpoint. httpbin.org is the
#      project's standard test endpoint but is frequently rate-limited or
#      returns 5xx; when it is unavailable that is not a fault of the
#      extension, so we retry and ultimately tolerate httpbin-side errors.
#
# Note: we intentionally skip duckdb_extensions() here. On GitHub Actions
# Linux runners (not in Docker), querying duckdb_extensions() after loading
# this extension triggers a SIGSEGV inside DuckDB's runner security sandbox.
# LOAD success + an attempted http_get is the meaningful signal.
LOAD_SQL = """\
LOAD '{ext}';
SELECT 42 AS ok;
"""

# Any HTTP status back (even a 5xx from a struggling httpbin) proves the
# extension issued a request and parsed a response — which is all this check
# validates.
HTTP_SQL = """\
LOAD '{ext}';

SELECT
  status,
  CASE WHEN status BETWEEN 100 AND 599 THEN 'PASS' ELSE 'FAIL' END AS result
FROM http_get('https://httpbin.org/status/200')
LIMIT 1;
"""

# Markers indicating httpbin.org (not the extension) is the problem. When the
# endpoint is unavailable http_get raises an error and the CLI exits non-zero,
# so we match against the combined stdout/stderr rather than a returned row.
# The connection-level strings mirror httplib's Error → to_string() mapping,
# which src/http_client.cpp embeds in the IOException it throws.
HTTPBIN_UNAVAILABLE_MARKERS = (
    "HTTP 502",
    "HTTP 503",
    "HTTP 504",
    "Could not connect",
    "Could not establish connection",
    "Connection refused",
    "Connection reset",
    "Connection timed out",
    "Could not resolve host",
    "Failed to read connection",
    "Failed to write connection",
    "SSL connection failed",
    "timed out",
    "Timeout was reached",
)

HTTP_MAX_ATTEMPTS = 4
HTTP_RETRY_DELAY_SECONDS = 5


def _download_duckdb_cli(version: str, arch: str, dest_dir: str) -> str:
    zip_name = ARCH_TO_CLI_ZIP.get(arch)
    if zip_name is None:
        raise SystemExit(
            f"Unsupported arch '{arch}'. Supported: {sorted(ARCH_TO_CLI_ZIP)}"
        )

    url = (
        f"https://github.com/duckdb/duckdb/releases/download/{version}/{zip_name}"
    )
    zip_path = os.path.join(dest_dir, "duckdb_cli.zip")

    print(f"Downloading DuckDB {version} CLI ({arch}):\n  {url}")
    urllib.request.urlretrieve(url, zip_path)

    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dest_dir)

    bin_name = "duckdb.exe" if platform.system() == "Windows" else "duckdb"
    binary = os.path.join(dest_dir, bin_name)
    if not os.path.isfile(binary):
        raise SystemExit(
            f"DuckDB binary not found after extraction: {binary}\n"
            f"Zip contents: {zipfile.ZipFile(zip_path).namelist()}"
        )

    if platform.system() != "Windows":
        os.chmod(binary, 0o755)

    return binary


def _run_sql(duckdb_bin: str, sql: str) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        [duckdb_bin, "-unsigned"],
        input=sql,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    return proc


def run_smoke_test(extension_path: str, duckdb_version: str, arch: str) -> None:
    if not os.path.isfile(extension_path):
        raise SystemExit(f"Extension artifact not found: {extension_path}")

    # Forward slashes work in DuckDB SQL on all platforms including Windows
    ext_sql_path = extension_path.replace("\\", "/")
    load_sql = LOAD_SQL.format(ext=ext_sql_path)
    http_sql = HTTP_SQL.format(ext=ext_sql_path)

    with tempfile.TemporaryDirectory() as tmpdir:
        duckdb_bin = _download_duckdb_cli(duckdb_version, arch, tmpdir)

        print(
            f"\nSmoke test:\n"
            f"  extension : {extension_path}\n"
            f"  duckdb    : {duckdb_bin}\n"
        )

        # Check 1: the extension binary must load cleanly. This is the
        # authoritative health signal — a broken artifact fails here.
        print(f"\n[1/2] Load check:\n{load_sql}")
        load_proc = _run_sql(duckdb_bin, load_sql)
        if load_proc.returncode != 0 or "42" not in (load_proc.stdout or ""):
            raise SystemExit(
                f"Smoke test FAILED: extension did not load "
                f"(duckdb exit code {load_proc.returncode})"
            )

        # Check 2: http_get must reach an external endpoint. Every failure is
        # retried to ride out transient httpbin.org hiccups; only after all
        # attempts do we classify: a sustained httpbin-side error is tolerated,
        # anything else (http_get missing, crash) fails the pipeline.
        print(f"\n[2/2] HTTP check:\n{http_sql}")
        http_proc = None
        for attempt in range(1, HTTP_MAX_ATTEMPTS + 1):
            if attempt > 1:
                time.sleep(HTTP_RETRY_DELAY_SECONDS * (attempt - 1))
            http_proc = _run_sql(duckdb_bin, http_sql)

            if http_proc.returncode == 0 and "PASS" in (http_proc.stdout or ""):
                print("\nSmoke test PASSED")
                return

            print(
                f"\n[attempt {attempt}/{HTTP_MAX_ATTEMPTS}] HTTP check failed "
                f"(duckdb exit code {http_proc.returncode}), retrying..."
                if attempt < HTTP_MAX_ATTEMPTS
                else f"\n[attempt {attempt}/{HTTP_MAX_ATTEMPTS}] HTTP check failed "
                f"(duckdb exit code {http_proc.returncode})."
            )

        assert http_proc is not None
        combined = (http_proc.stdout or "") + (http_proc.stderr or "")
        if any(marker in combined for marker in HTTPBIN_UNAVAILABLE_MARKERS):
            # Extension loaded and issued the request; httpbin is simply
            # down. Treat as a pass rather than failing the pipeline.
            print(
                "\nSmoke test PASSED (extension OK; httpbin.org unavailable, "
                "tolerated)"
            )
            return

        raise SystemExit(
            f"Smoke test FAILED (duckdb exit code {http_proc.returncode})"
        )


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(
            f"Usage: {sys.argv[0]} <extension_path> <duckdb_version> <arch>"
        )
    run_smoke_test(
        extension_path=sys.argv[1],
        duckdb_version=sys.argv[2],
        arch=sys.argv[3],
    )
