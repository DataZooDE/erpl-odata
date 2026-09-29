#!/usr/bin/env python3
"""Peak-RSS of an OData scan as a function of server page size (GitHub #89).

WHY THIS EXISTS
---------------
Issue #89 proposed streaming rows out of `ODataEntitySetJsonContent::ToRows` and
sending `Prefer: odata.maxpagesize`, on the theory that peak memory is dictated by
the server's page size. The review asked for a page-size histogram before any
optimisation. This is the instrument that produced it; re-run it before revisiting
the issue rather than re-arguing from first principles.

WHAT IT MEASURES
----------------
`total_rows` is held constant and only `rows_per_page` varies, so any difference in
peak RSS is attributable to how much of a single page is live at once. Peak RSS is
the kernel's own high-water mark (`/proc/<pid>/status` VmHWM), sampled by a poller.

Pass `--query http_get` to measure the same body through `http_get()` instead. That
is the control that separates what the HTTP layer costs from what OData parsing and
`Value` boxing add on top.

MEASURED 2026-09-10, release build, 500_000 rows total, ~271 wire bytes/row
---------------------------------------------------------------------------
    rows/page   page bytes   peak RSS   delta over the 200-row page
          200       54_246     258 MB   -
        2_000      547_180     279 MB   +21 MB
       20_000    5_530_514     456 MB   +198 MB
      100_000   27_803_848    1074 MB   +816 MB
      500_000  140_203_780    3858 MB   +3600 MB

Peak RSS is linear in the bytes of ONE page, at roughly 26x the page's wire size.
The largest page any reachable real service returns is 308 KB (see
odata_page_sizes.py), which buys about 8 MB - i.e. the effect is real but only
above page sizes no measured service produces.

The `--query http_get` control shows where that 26x is spent. On the same 208 MB
body, debug build: baseline 1895 MB, `http_get` 7346 MB, `odata_read` 8298 MB. So
~85% of peak on a large page is already committed by the HTTP layer before
`ToRows` is ever called; OData parsing plus full-page `Value` boxing is the
remaining ~15%. Streaming `ToRows` cannot change the order of magnitude.
Projection makes no difference (`max(Id)` costs the same as `count(*)`), because
`ToRows` always boxes the full schema.

USAGE
-----
    # A debug binary has the extension linked in; a release binary needs LOAD.
    test/perf/odata_page_memory.py ./build/debug/duckdb \\
        --total-rows 500000 --payload 200 --pages 200 2000 20000 100000 500000

    ERPL_LOAD=build/release/extension/erpl_odata/erpl_odata.duckdb_extension \\
    test/perf/odata_page_memory.py ./build/release/duckdb --pages 200 20000

NOTE ON THE DEBUG BINARY
------------------------
The debug build is AddressSanitizer-instrumented, so its absolute numbers carry a
~1.9 GB fixed baseline and an inflated per-allocation cost. Compare deltas, or use a
release build for absolute figures.
"""
import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "odata_page_server.py")

QUERIES = {
    "odata_read": ("SELECT count(*) AS n, sum(Amount) AS s "
                   "FROM odata_read('http://127.0.0.1:{port}/svc/Items')"),
    # Control: same body over the wire, none of the OData parsing or Value boxing.
    "http_get": "SELECT status FROM http_get('http://127.0.0.1:{port}/svc/Items')",
    "noop": "SELECT 1",
}


def wait_ready(port, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/svc/", timeout=1).read()
            return True
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    return False


def poll_peak(pid, stop, out):
    """Track VmHWM, the kernel's peak-RSS counter, until the process exits."""
    while not stop.is_set():
        try:
            with open(f"/proc/{pid}/status") as status:
                for line in status:
                    if line.startswith("VmHWM:"):
                        out[0] = max(out[0], int(line.split()[1]))
                        break
        except OSError:
            return
        time.sleep(0.05)


def run_one(binary, port, rows_per_page, total_rows, payload, query):
    server = subprocess.Popen(
        [sys.executable, SERVER, str(port), str(rows_per_page), str(total_rows), str(payload)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        if not wait_ready(port):
            raise RuntimeError(f"synthetic OData server did not come up on port {port}")

        argv = [binary]
        load = os.environ.get("ERPL_LOAD")
        if load:
            argv += ["-unsigned", "-s", f"LOAD '{load}'"]
        argv += ["-s", query.format(port=port)]

        env = dict(os.environ, ASAN_OPTIONS="detect_odr_violation=0")
        started = time.time()
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env)
        peak = [0]
        stop = threading.Event()
        poller = threading.Thread(target=poll_peak, args=(proc.pid, stop, peak), daemon=True)
        poller.start()
        output, _ = proc.communicate(timeout=3600)
        stop.set()
        poller.join(timeout=1)
        wall = time.time() - started
    finally:
        server.send_signal(signal.SIGTERM)
        try:
            _, server_log = server.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server_log = ""

    served = re.findall(r"^PAGE .*bytes=(\d+)", server_log or "", re.M)
    return {
        "rows_per_page": rows_per_page,
        "page_bytes": int(served[0]) if served else 0,
        "pages_served": len(served),
        "peak_rss_kb": peak[0],
        "wall_s": round(wall, 1),
        "output": " ".join(line.strip() for line in output.splitlines() if "│" in line),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="path to a duckdb binary carrying erpl_odata")
    parser.add_argument("--total-rows", type=int, default=500000)
    parser.add_argument("--payload", type=int, default=200,
                        help="filler characters per row; 200 gives ~270 wire bytes/row, "
                             "close to Northwind v4 Orders")
    parser.add_argument("--pages", type=int, nargs="+",
                        default=[200, 2000, 20000, 100000, 500000],
                        help="rows per page to sweep")
    parser.add_argument("--query", choices=sorted(QUERIES), default="odata_read")
    parser.add_argument("--port-base", type=int, default=19300)
    args = parser.parse_args()

    query = QUERIES[args.query]
    print(f"# query={args.query} total_rows={args.total_rows} payload={args.payload}")
    print("rows_per_page\tpage_bytes\tpages\tpeak_rss_mb\twall_s")
    for index, rows_per_page in enumerate(args.pages):
        result = run_one(args.binary, args.port_base + index, rows_per_page,
                         args.total_rows, args.payload, query)
        print(f"{result['rows_per_page']}\t{result['page_bytes']}\t{result['pages_served']}\t"
              f"{result['peak_rss_kb'] / 1024:.0f}\t{result['wall_s']}")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
