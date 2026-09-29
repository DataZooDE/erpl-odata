# ERPL-OData Telemetry

ERPL-OData collects **anonymous, privacy-preserving usage telemetry** so we can see
which capabilities are used, on which platforms, and where they fail — and
prioritise accordingly. It is **on by default** and **trivial to turn off**.

Telemetry is emitted through the shared
[`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library and follows the cross-product **`telemetry_schema: 2`** envelope
(`posthog-telemetry/TELEMETRY-SCHEMA.md`). Ingestion is the EU PostHog cloud.

## How to turn it off

Any one of these fully short-circuits telemetry — when disabled, **nothing
leaves the machine** (the opt-out is enforced at the transport, not just at the
call sites):

```sql
SET erpl_telemetry_enabled = false;   -- DuckDB setting (per session)
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1     # environment (1|true|yes)
```

## The guarantee: bounded, enumerated, non-PII

Every property we send is **either** a constant drawn from a small,
code-controlled enumeration **or** a pure number (durations, counts). The
library additionally clamps every outgoing string to 512 bytes as a backstop.

We **never** send: URLs, host names, service roots, entity/table/column names,
OData `$filter`/query text, SQL text, request/response bodies, row or result
data, credentials, tokens, secret names, or error messages.

The instrumentation is centralised in one small, auditable header —
`src/include/telemetry.hpp` (the vendored `posthog-telemetry` library) — and
the only per-function capture is a single `RecordFunctionCall(<constant>)` at
each function's **bind** step.

## What is collected

### Envelope (attached to every event)

`product` (`erpl_odata`), `product_version`, `product_edition` (`oss`),
`telemetry_schema` (`2`), `duckdb_version`, `os`, `arch`, `platform`, `is_ci`,
`is_container`, a per-process `$session_id`, and — once associated — the
`deployment` group. `distinct_id` is the SHA-256 of a machine id: a **stable,
pseudonymous** identifier, not tied to any personal data.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `extension_loaded` | the `erpl_odata` extension loads | — |
| `function_executed` | a DuckDB function runs — **aggregated** per function per session (not per row) | `function_name`, `call_count`, `duration_ms_p50` |

`function_name` is always one of a fixed, code-controlled set of ERPL-OData
function names, for example: `http_get`, `http_head`, `http_post`, `http_put`,
`http_patch`, `http_delete`, `odata_read`, `odata_describe`, `odata_attach`,
`odata_sap_show`, `odp_odata_show`, `odp_odata_read`, `odp_list_subscriptions`,
`odp_remove_subscription`, `datasphere_show_spaces`, `datasphere_show_assets`,
`datasphere_describe_space`, `datasphere_describe_asset`,
`datasphere_read_relational`, `datasphere_read_analytical`, `sac_show_models`,
`sac_show_stories`, `sac_describe_model`, `sac_describe_story`,
`sac_read_planning_data`, `sac_read_analytical`, `sac_read_story_data`,
`delta_share_scan`, `delta_share_show_shares`, `delta_share_show_schemas`,
`delta_share_show_tables`. The name identifies *which* function ran — never its
arguments.

This repository does **not** currently emit `feature_used` or `$exception`
events; those are reserved in the shared schema for a later per-repo
instrumentation pass.

## Function-call aggregation

DuckDB function calls are recorded via `RecordFunctionCall(function_name)`, which
aggregates in-process into a single `function_executed` event per function per
session (carrying `call_count` and `duration_ms_p50`). The call sites live at
each function's bind step, never on a per-row `GetChunk` path, so a
million-row scan produces O(1) telemetry rows, not a firehose.

## Enterprise / account analytics

OSS ERPL-OData associates only the `deployment` group (keyed on the pseudonymous
`distinct_id`). It has no license key, so no `account` group is associated.
