---
name: erpl-odata-sap-odp-replication
description: >
  Replicating SAP data with full and delta loads through ODP (Operational Data
  Provisioning) OData services with erpl_odata: odp_odata_show to find
  ODP-enabled entity sets, odp_odata_read (secret, force_full_load,
  import_delta_token, max_page_size) as a change stream with ODQ_CHANGEMODE
  and ODQ_ENTITYCNTR, the correct DELETE + INSERT merge into a persistent
  DuckDB table, subscription state (odp_odata_list_subscriptions, PRAGMA
  odp_odata_remove_subscription), ODQMON cleanup, HTTP 202 long-running
  extractions, why a delta never arrives (CDS changeDataCapture vs byElement,
  Preference-Applied), and provisioning an ODP service headlessly with
  erpl-adt. Use when the user wants incremental or CDC loads from SAP
  extractors (0D_*, 2LIS_*), BW InfoProviders or CDS extraction views, or a
  delta load returns everything again.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — SAP ODP Full and Delta Replication

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v2.0.0 / v1.5.6

ODP exposes SAP extractors, BW providers and CDS extraction views as OData entity sets
named `FactsOf<ODP>`, `AttrOf<ODP>` or `EntityOf<ODP>`. The first `odp_odata_read` does
an initial load and opens an ODQ subscription. Every later read returns **only the
changes** since the previous one.

## Critical gotchas

1. **The result is a change stream, not a snapshot.** After the initial load each read
   returns inserts, updates **and deletes**, marked in the `ODQ_CHANGEMODE` column
   (`'D'` = delete). `INSERT INTO target SELECT * FROM odp_odata_read(...)` silently turns
   deletes into new rows and duplicates updated keys. Always merge (see below).
2. **Delta state needs a persistent DuckDB database.** The subscription and its delta token
   are stored in the `erpl_web` schema of the current database, or the first writable
   persistent one. In `:memory:` the read fails with *"ODP delta state requires a
   persistent DuckDB database"*. Open a file (`duckdb replication.db`) or
   `ATTACH 'state.db' AS state; USE state;`.
3. **The subscription id includes the secret name.** It is derived from the service URL,
   the entity set and the `secret` name. Renaming the secret, or switching between
   `secret := 'x'` and scope lookup, starts a **new** subscription with a new initial load,
   and leaves the old one orphaned in ODQ.
4. **One open extraction per subscription.** Two reads of the same entity set inside one
   statement make SAP refuse the second (*"Could not open data access via extraction API
   RODPS_REPL_ODP_OPEN"*, RSODP_ODATA/013). Land each read in its own statement.
5. **A delta is only promised once SAP confirms change tracking.** If the initial load comes
   back without `Preference-Applied: odata.track-changes`, the subscription stays in
   initial-load mode and every read is a full load, with the warning *"Change tracking was
   NOT established"*. That is the safe outcome, and it usually means the source is not
   delta-capable (gotcha 7).
6. **Removing a subscription is local only.** `PRAGMA odp_odata_remove_subscription` does
   not terminate the ODQ subscription, so SAP keeps queueing changes for it. Clean up in
   transaction `ODQMON` as well.
7. **`@Analytics.dataExtraction.delta.byElement` does not make a CDS view delta-capable.**
   It activates fine, but `RODPS_REPL_ODP_GET_DETAIL` reports `supports_delta = ' '` and the
   service has no `DeltaLinksOf<EntitySet>`. Use
   `@Analytics.dataExtraction.delta.changeDataCapture.automatic: true`.
8. **`force_full_load := true` discards the token at bind time.** A `PREPARE` or `EXPLAIN`
   of such a statement already forgets it, so the next real read is a full load: extra
   work, never missed rows.

## Functions

| Function | Purpose |
|---|---|
| `odp_odata_show(host_url, secret := ...)` | ODP-enabled entity sets on the system: `service_id`, `service_description`, `entity_set_id`, `entity_set_description`, `entity_set_url`, `change_tracking` |
| `odp_odata_read(entity_set_url, secret := ..., force_full_load := ..., import_delta_token := ..., max_page_size := ...)` | Initial load, then deltas |
| `odp_odata_list_subscriptions()` | `subscription_id`, `service_url`, `entity_set_name`, `secret_name`, `delta_token`, `created_at`, `last_updated`, `subscription_status`, `preference_applied` |
| `PRAGMA odp_odata_remove_subscription(id_or_list [, keep_local_data])` | Forget a subscription locally. `keep_local_data = true` marks it `terminated` instead of deleting the record |

`odp_odata_read` parameters:

| Parameter | Meaning |
|---|---|
| `secret` | Secret to authenticate with. Part of the subscription identity (gotcha 3) |
| `force_full_load` | Drop the stored token and do a new initial load |
| `import_delta_token` | Adopt a token obtained elsewhere, for example after moving the state database |
| `max_page_size` | `odata.maxpagesize` preference; default 15000 |

The URL must end in an `EntityOf…`, `FactsOf…` or `AttrOf…` entity set, otherwise it is
rejected as not an ODP URL.

## Find ODP entity sets

```sql
CREATE SECRET sap (TYPE http_basic, USERNAME 'DEVELOPER', PASSWORD 'secret',
                   SCOPE 'http://localhost:50000/');

SELECT service_id, entity_set_id, entity_set_url, change_tracking
FROM odp_odata_show('http://localhost:50000');
```

`change_tracking` shows whether the service advertises delta support. If it is false,
expect full loads every time.

## Canonical replication loop

Run this as one script per scheduled run against the **same database file**.

```sql
-- duckdb /data/sap_replica.db
LOAD erpl_odata;
CREATE SECRET IF NOT EXISTS sap (TYPE http_basic, USERNAME 'EXTRACT_USER', PASSWORD 'secret',
                                 SCOPE 'https://s4.example.com:44300/');

-- 1. Land the change set (initial load on the first run, delta afterwards)
CREATE OR REPLACE TEMP TABLE changes AS
SELECT * FROM odp_odata_read(
    'https://s4.example.com:44300/sap/opu/odata/sap/Z_ODP_SALES_SRV/FactsOfZSALESODP',
    secret := 'sap');

-- 2. First run only: create the target with the same shape
CREATE TABLE IF NOT EXISTS sales AS SELECT * FROM changes WHERE false;

-- 3. Remove every key the change set touches (covers updates and deletes)
DELETE FROM sales
WHERE (SalesDocument, SalesDocumentItem) IN (SELECT SalesDocument, SalesDocumentItem FROM changes);

-- 4. Re-insert the surviving after-images, latest image per key
INSERT INTO sales
SELECT * FROM changes
WHERE coalesce(ODQ_CHANGEMODE, '') <> 'D'
QUALIFY row_number() OVER (PARTITION BY SalesDocument, SalesDocumentItem
                           ORDER BY ODQ_ENTITYCNTR DESC) = 1;

-- 5. Check the subscription moved on
SELECT subscription_id, subscription_status, preference_applied, last_updated
FROM odp_odata_list_subscriptions();
```

Notes on the merge:
- Use the source's business key. On BW extractors `ODQ_CHANGEMODE` and `ODQ_ENTITYCNTR`
  are part of the OData key, so do not key on them.
- `ODQ_CHANGEMODE` is `''` or `'C'` for new images, `'U'` for after-images and `'D'` for
  deletes. Test for `= 'D'` rather than listing every other value, because spelling
  differs between providers.
- If you do not need history and the source is small, `force_full_load := true` with
  `CREATE OR REPLACE TABLE` is correct and simpler, but it moves the whole provider each run.
- Wrap steps 3 and 4 in `BEGIN; ... COMMIT;` so a failure cannot leave the target half-merged.

For a versioned or time-travel target, land each change set into DuckLake or an
append-only history table instead, with a load timestamp, and derive current state with a
`QUALIFY` view. To diff two loads, use the anofox-tabular plugin's `anofox-tabular-diff`
skill.

## Managing subscriptions

```sql
SELECT subscription_id, entity_set_name, secret_name, subscription_status, delta_token
FROM odp_odata_list_subscriptions();
```

```sql
-- Forget one locally (then terminate it in ODQMON as well)
PRAGMA odp_odata_remove_subscription('<subscription_id>');

-- Several at once; keep the record but mark it terminated
PRAGMA odp_odata_remove_subscription(['<id1>', '<id2>'], true);
```

**Recovering a token.** If the state database was lost but the ODQ subscription still
exists, read the service's `DeltaLinksOf<EntitySet>` entity set with `odata_read` to get
the latest delta token, then continue with `import_delta_token := '<token>'`.
`TerminateDeltasFor<Entity>` only flips `SubscribedFlag` and issues no token.

## Long-running extractions (HTTP 202)

A large initial load can make SAP answer `202 Accepted`. The reader then re-polls,
honouring `Retry-After` (seconds or an HTTP date) and following `Location`. Polling is
bounded and ends in an error naming the URL and the time waited. If that happens, run
the initial load in an off-peak window or narrow it (an ODP selection on the SAP side),
rather than retrying in a loop.

## Why does every run do a full load?

Check in this order:

1. `SELECT preference_applied FROM odp_odata_list_subscriptions()`: `false` means SAP never
   confirmed change tracking.
2. `odp_odata_show(...)`: is `change_tracking` true for the entity set?
3. On the SAP side, call `RODPS_REPL_ODP_GET_DETAIL` for the ODP: `e_supports_delta` must
   be `'X'`. For CDS views this needs `changeDataCapture.automatic` (gotcha 7).
4. Was the secret or URL changed? That starts a new subscription (gotcha 3).
5. Is the session in-memory? Then nothing persisted between runs (gotcha 2).

Turn on tracing (`SET erpl_trace_enabled = true; SET erpl_trace_level = 'DEBUG';`) to see
the `Prefer` and `Preference-Applied` headers.

## Provisioning an ODP service (no SEGW UI)

A stock system has no ODP OData service. SAP ships no headless way to create one;
`scripts/sap/zcl_odp_provision.abap` in this repository performs the three steps the SEGW
guided activity does: generate MPC/DPC, register the model and service, and activate it
in the Gateway hub. Run it with erpl-adt:

```bash
erpl-adt object create --type CLAS/OC --name ZCL_ODP_PROVISION --package '$TMP' \
    --description 'ODP provisioning'
erpl-adt source write ZCL_ODP_PROVISION --type CLAS --file scripts/sap/zcl_odp_provision.abap
erpl-adt activate ZCL_ODP_PROVISION
erpl-adt object run ZCL_ODP_PROVISION
```

Facts that cost time the first time:
- The ODP name is the CDS view's **SQL view name** plus a suffix: `$F` for
  `#CUBE`, `$P` for `#DIMENSION`, `$T` for `#TEXT`. List them with
  `SELECT viewname, odpname FROM rsodpabapcdsextb( p_langu = @sy-langu )`. Without the
  parameter, activation fails with *"The parameter P_LANGU was not bound"*.
- Everything lives in `$TMP`: the extraction annotations and classic ODP APIs are not
  released for ABAP Cloud.
- `scripts/sap/zjr_odp_v.ddls.abap` and `scripts/sap/zjr_odp_data.tabl.abap` are a working
  delta-capable fixture whose contents you control.

See also: `erpl-odata-sap-gateway` (non-delta Gateway reads, service activation),
`erpl-odata-setup` (secrets, TLS), `erpl-odata-production` (scheduling the loop,
unattended secrets, monitoring and the error playbook).

Reference docs:
- `docs/ODP.md`
- `README.md` (SAP ODP)
- `scripts/sap/zcl_odp_provision.abap`
