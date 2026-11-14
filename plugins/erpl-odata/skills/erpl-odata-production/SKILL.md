---
name: erpl-odata-production
description: >
  Running erpl_odata extractions unattended and operating them: job scripts
  for the DuckDB CLI, persistent DuckDB files or DuckLake as the landing zone,
  idempotent snapshot and delta (ODP) loads, scheduling with cron or systemd
  without overlapping runs, unattended credentials (client_credentials,
  secrets from environment variables, persistent secrets), run logs and
  monitoring, file tracing (erpl_trace_* settings, PRAGMA erpl_trace_status),
  and an error-to-fix playbook (401/403, TLS, 404 on /sap/opu, 202 polling,
  RODPS_REPL_ODP_OPEN, custom service URL guard, NULLs from type conversion).
  Also how to hand extracted data to other DataZoo tools: flapi to serve it
  as REST/MCP, anofox-tabular for data-quality gates, anofox-forecast for
  forecasting and anofox-visualization for dashboards. Use when the user
  wants to schedule or productionise an extraction, debug a failing job, or
  build an end-to-end pipeline from SAP or Microsoft data to an API,
  forecast or dashboard.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — Production Pipelines and Operations

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v2.0.0 / v1.5.6

The error-to-fix playbook is in `references/troubleshooting.md`. Read it first when a
job fails.

## Critical gotchas

1. **Nothing interactive in a job.** `authorization_code` (Graph) and Datasphere's default
   `oauth2` grant open a browser and block forever in cron, CI or a container. Use
   `client_credentials` secrets for every unattended job.
2. **ODP needs the same database file on every run, and no overlap.** The delta token
   lives in that file. Two overlapping runs of the same subscription fail with
   `RODPS_REPL_ODP_OPEN`. Serialise runs (`flock`) and never point two jobs at one
   subscription.
3. **A run must be idempotent.** Snapshot sources: `CREATE OR REPLACE TABLE` in one
   statement, so a failure leaves the previous table intact. Delta sources: the
   DELETE + INSERT merge inside `BEGIN ... COMMIT` (see `erpl-odata-sap-odp-replication`).
4. **Persistent secrets are plain text on disk.** `CREATE PERSISTENT SECRET` writes to
   `~/.duckdb/stored_secrets/`. For services, prefer creating temporary secrets at job start
   from environment variables injected by the scheduler or secret store.
5. **Failures must fail the job.** Run the CLI with `-bail` so the first error stops the
   script with a non-zero exit code. For `http_*`, check `status` yourself, because an HTTP
   error is a row, not an exception.
6. **Tracing to console is noisy in logs.** In production write traces to a rotating file,
   at `INFO`, and switch to `DEBUG` only while diagnosing.

## A job script

`jobs/sap_sales.sql`, run with `duckdb -bail /data/warehouse.db -f jobs/sap_sales.sql`:

```sql
LOAD erpl_odata;
SET erpl_telemetry_enabled = false;
SET erpl_trace_enabled = true;
SET erpl_trace_output = 'file';
SET erpl_trace_file_path = '/var/log/erpl/sap_sales.log';
SET erpl_trace_level = 'INFO';
SET erpl_trace_rotation = true;

-- Credentials from the environment (set by the scheduler / secret store)
CREATE OR REPLACE SECRET sap (
    TYPE http_basic,
    USERNAME getenv('SAP_USER'),
    PASSWORD getenv('SAP_PASSWORD'),
    SCOPE 'https://s4.example.com:44300/'
);
SET erpl_ca_cert_file = '/etc/erpl/erpl-ca-bundle.pem';

-- Snapshot load: replaced atomically
CREATE OR REPLACE TABLE customers AS
SELECT * FROM odata_read('https://s4.example.com:44300/sap/opu/odata/sap/API_BUSINESS_PARTNER/A_Customer',
                         strict_typing := true);

-- Run log for monitoring
CREATE TABLE IF NOT EXISTS run_log (job VARCHAR, finished_at TIMESTAMP, table_name VARCHAR, row_count BIGINT);
INSERT INTO run_log SELECT 'sap_sales', now(), 'customers', count(*) FROM customers;
```

`getenv` is available in the DuckDB CLI. In an embedded client (Python, Java) read the
environment in the host language and pass the values as parameters.

## Scheduling

```bash
# crontab: every 30 minutes, never two runs at once, logs kept
*/30 * * * * flock -n /tmp/sap_sales.lock \
    duckdb -bail /data/warehouse.db -f /opt/jobs/sap_sales.sql >> /var/log/erpl/cron.log 2>&1
```

A systemd timer with `Persistent=true` catches up after downtime. In GitHub Actions or
Kubernetes CronJobs, use `concurrencyPolicy: Forbid` (or a `concurrency:` group) for the
same effect, and keep the database on a persistent volume or in DuckLake.

## Landing zones

| Need | Use |
|---|---|
| One writer, analytical queries on one machine | A DuckDB file |
| Several readers or writers, history, time travel | DuckLake (`ATTACH 'ducklake:...' AS lake`), one table per source entity |
| Hand-off to other engines | `COPY table TO 'out/' (FORMAT parquet, PARTITION_BY (...))` |

For ODP, the subscription state lives in the database the reader uses (the `erpl_web`
schema of the default writable database). Keep that one stable even when the data goes to
DuckLake.

## Monitoring

```sql
-- Freshness and volume per job
SELECT job, table_name, max(finished_at) AS last_run, arg_max(row_count, finished_at) AS last_rows
FROM run_log GROUP BY ALL;

-- ODP subscriptions: still active? change tracking confirmed? last token update?
SELECT entity_set_name, subscription_status, preference_applied, last_updated
FROM odp_odata_list_subscriptions();

-- Tracing configuration of the session
PRAGMA erpl_trace_status;
```

Alert when `last_run` is older than two schedule intervals, when `row_count` of a
snapshot drops sharply, or when `preference_applied` is false for an ODP subscription.

## Handing data to other DataZoo tools

These extensions ship their own Claude Code plugins. Use their skills for the work itself.
This skill only covers the hand-off.

| Goal | Tool and skill |
|---|---|
| Gate a load on data quality (null rates, volume, freshness, custom checks) | anofox-tabular, `anofox-tabular-data-quality` (`run_checks`), with thresholds from `anofox-tabular-profiling` |
| Compare today's extract with yesterday's, or source with target after a migration | anofox-tabular, `anofox-tabular-diff` |
| Find and mask personal data in HR, CRM or directory extracts | anofox-tabular, `anofox-tabular-pii` |
| Standardise customer and vendor master data from ERP and CRM | anofox-tabular, `anofox-tabular-master-data` |
| Forecast demand, revenue or cash from SAP or BC history | anofox-forecast, `anofox-forecast-data-prep`, then `anofox-forecast-models` and `anofox-forecast-backtest` |
| Serve extracted data as a REST API or as MCP tools for agents | flapi, `flapi-setup`, `flapi-rest`, `flapi-mcp`, and `flapi-cache-design` for refresh schedules |
| Dashboards on the landed tables | anofox-visualization, `build-dashboard` (a project skill inside the anofox-visualization repository, not an installable plugin) |

### Serving through flapi

Never let an API request hit SAP directly: every call would be a live extraction with
SAP's latency and authorisation checks. Load into a flapi **DuckLake cache** on a schedule
and serve from the cache. In `flapi.yaml`, the connection's `init` loads the extension and
creates the secret (read env vars as `flapi-setup` describes):

```yaml
connections:
  sap:
    init: |
      INSTALL erpl_odata FROM community;
      LOAD erpl_odata;
```

The cache template then holds the extraction SQL (`odata_read(...)` or the ODP merge), and
`flapi-cache-design` explains full-refresh vs incremental modes.

## Canonical end-to-end pipeline: SAP to quality gate, forecast, API

```sql
-- 1. Extract (snapshot) — erpl_odata; service, entity set and fields are illustrative
CREATE OR REPLACE TABLE sales_history AS
SELECT Material AS sku, CAST(BillingDocumentDate AS DATE) AS ds, sum(BillingQuantity) AS y
FROM odata_read('https://s4.example.com:44300/sap/opu/odata/sap/API_BILLING_DOCUMENT_SRV/A_BillingDocumentItem')
WHERE BillingDocumentDate >= DATE '2023-01-01'
GROUP BY ALL;

-- 2. Gate — anofox-tabular-data-quality: stop if volume or null rates are off
-- 3. Prepare and forecast — anofox-forecast-data-prep, anofox-forecast-models
--    (gap filling per sku, then a forecast per sku into a forecast table)
-- 4. Serve — flapi endpoint over the forecast table, cached (flapi-rest, flapi-cache-design)
-- 5. Visualise — anofox-visualization build-dashboard over sales_history + forecast
```

Keep each step a materialised table, so a failure is restartable from the last good step
and every step can be inspected.

See also: `erpl-odata-setup` (secrets, TLS), `erpl-odata-sap-odp-replication` (delta
merge), `erpl-odata-odata-and-rest` (pushdown, which decides how much each run moves).

Reference docs:
- `references/troubleshooting.md`
- `README.md` (Tracing & Diagnostics)
- `docs/ODP.md`, `docs/TLS.md`
