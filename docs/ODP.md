# SAP ODP (Operational Data Provisioning)

`odp_odata_read()` reads an SAP ODP delta-enabled OData service. It is the only reader in this
extension whose result set is **not** a snapshot.

> **Read the [Change stream contract](#change-stream-contract) before you write the target table.**
> Consuming an ODP read with a plain `INSERT` is silently wrong once the first delta arrives.

---

## Functions

| Function | Purpose |
|---|---|
| `odp_odata_read(entity_set_url, ...)` | Read an ODP entity set; performs an initial load, then delta fetches |
| `odp_odata_show(service_url, ...)` | Discover the ODP-enabled entity sets a service exposes |
| `odp_odata_list_subscriptions()` | List the local subscriptions and their delta tokens |
| `PRAGMA odp_odata_remove_subscription(subscription_id, ...)` | Forget a subscription locally |

### `odp_odata_read` named parameters

| Parameter | Type | Meaning |
|---|---|---|
| `secret` | `VARCHAR` | Name of the DuckDB secret to authenticate with |
| `force_full_load` | `BOOLEAN` | Discard the stored delta token and re-run an initial load. The token is discarded when the statement is *bound*, not when it first runs, so a `PREPARE` or `EXPLAIN` of such a statement also forgets it; the next read is then a full load rather than a delta (extra work, never missed rows). |
| `import_delta_token` | `VARCHAR` | Adopt a delta token obtained elsewhere |
| `max_page_size` | `UINTEGER` | Override the `odata.maxpagesize` preference (default 15000) |

---

## Change stream contract

An ODP subscription has two phases:

1. **Initial load** — the first read returns the full current contents of the operational
   provider, and SAP's ODQ starts recording changes. The extension sends
   `Prefer: odata.track-changes` and requires the service to confirm it with
   `Preference-Applied: odata.track-changes`.
2. **Delta fetch** — every subsequent read returns **only what changed** since the previous read:
   inserted rows, updated rows, **and deleted rows**.

So the result of `odp_odata_read` is a stream of change records, not a table state. The row count
of a delta read is the size of the change set, not the size of the source.

### The change-mode marker

SAP marks each row with its change type. In the EDMX the marker is an ordinary property annotated
`sap:semantics="change-mode"` — conventionally named **`ODQ_CHANGEMODE`**:

```xml
<Property Name="ODQ_CHANGEMODE" Type="Edm.String" Nullable="false" MaxLength="1"
          sap:semantics="change-mode" .../>
```

Because it is a normal EDM property (and, in BW extractors, part of the entity key), **it is
already present in the result set of `odp_odata_read` as an ordinary column**. The extension does
not add it, rename it, hide it, or interpret it — it passes through like any other column.

Values you should expect:

| Value | Meaning |
|---|---|
| `''` (empty) or `'C'` | Insert / new image. Rows of an **initial load** carry this. |
| `'U'` | Update — an after-image of a changed record |
| `'D'` | **Delete** — the record identified by the key no longer exists |

A companion property annotated `sap:semantics="entity-counter"` (conventionally
`ODQ_ENTITYCNTR`) orders the records within a package; where one record is changed more than once
in a delta, the highest counter is the winning image.

> **Unverified against a live system.** The exact set of marker values, and their per-provider
> spelling, is SAP-side behaviour that could not be exercised here (no ODP system or credentials
> were available during this work). `'D'` for delete is well established; treat the rest as
> indicative and confirm against your own provider before relying on it. Prefer testing for
> "is it a delete" (`ODQ_CHANGEMODE = 'D'`) over enumerating every other value.

### Consuming a change stream correctly

Do **not** do this — deletes are appended as if they were new rows, and updates duplicate keys:

```sql
-- WRONG for anything after the initial load
INSERT INTO target SELECT * FROM odp_odata_read('...');
```

Land the change set and apply it as a merge, deletes first:

```sql
CREATE TEMP TABLE changes AS
SELECT * FROM odp_odata_read('https://sap.example.com/sap/opu/odata/sap/ZSRV/EntitySet');

-- 1. remove everything the change set touches (handles both updates and deletes)
DELETE FROM target
WHERE (key_a, key_b) IN (SELECT key_a, key_b FROM changes);

-- 2. re-insert only the surviving after-images
INSERT INTO target
SELECT * FROM changes
WHERE ODQ_CHANGEMODE <> 'D'
QUALIFY row_number() OVER (PARTITION BY key_a, key_b ORDER BY ODQ_ENTITYCNTR DESC) = 1;
```

If you only ever want current state and never want to think about this, use `force_full_load =>
true` on every read. That is correct but re-transfers the whole provider each time, which is the
cost ODP exists to avoid.

### Why there is no separate normalised marker column

The marker is already in the result set under its SAP name, and it is part of the entity key on
BW extractors. Adding a second, normalised column (say `_change_type`) would:

- widen the schema of every existing `SELECT *` against `odp_odata_read`, breaking positional
  consumers and any `CREATE TABLE AS` target already in production;
- duplicate a value that is already present, creating a second source of truth to keep in sync;
- still not remove the need to understand the contract above, because the merge logic is the
  hard part, not the column name.

The contract is therefore documented rather than encoded. If a future change does surface a
normalised marker, it should be opt-in via a named parameter so that existing schemas are
unaffected.

---

## Change tracking must be confirmed, not assumed

The extension advances a subscription from initial load to delta fetch **only** when the service
answered the initial load with `Preference-Applied: odata.track-changes`. If that header is
missing, the subscription deliberately stays in initial-load mode and logs a warning:

```
Initial load response did not carry 'Preference-Applied: odata.track-changes'.
Change tracking was NOT established; ...
```

This is the safe failure: you get a full re-read (correct data, more traffic) rather than a
"delta" computed over data that was never change-tracked, which would silently miss every change
made in between. Turn on tracing (`SET erpl_trace_enabled = TRUE`) if you see repeated full loads
— an absent `Preference-Applied` is the usual cause, and it normally means the entity set is not
actually delta-enabled.

## Long-running extractions (HTTP 202)

A large extraction can take SAP longer to prepare than one request will wait for. The service then
answers `202 Accepted` and the extension re-polls, honouring the `Retry-After` header in both of
its permitted forms (a number of seconds, or an HTTP date), following the `Location` header when
one is supplied. Re-polling is bounded: it stops after a maximum number of attempts or once a total
wait budget is exhausted, and then raises an error naming the URL and how long it waited, rather
than stalling indefinitely.

## Subscriptions are persistent, and live on the SAP side too

`odp_odata_list_subscriptions()` shows the subscriptions this extension is tracking, stored in a
persistent DuckDB database. The corresponding **ODQ subscription exists on the SAP system** and is
not managed by that table.

`PRAGMA odp_odata_remove_subscription(...)` forgets a subscription **locally only**. It does not
terminate the subscription in ODQ. An orphaned ODQ subscription keeps the SAP system retaining
delta queue data for a consumer that will never come back — which consumes space and can hold up
housekeeping on the provider.

If you remove a subscription locally and do not intend to resume it, terminate it on the SAP side
as well, via ODQMON (transaction `ODQMON`) or your Basis team's usual process. This extension
deliberately does **not** implement remote termination: doing it correctly requires a CSRF token
fetch and a retry, and a half-working implementation that appears to clean up but does not is worse
than an explicit hand-off.
