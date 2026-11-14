---
name: erpl-odata-sap-datasphere-sac
description: >
  Pulling SAP Datasphere and SAP Analytics Cloud (SAC) data into DuckDB with
  erpl_odata: the datasphere secret (oauth2 with authorization_code or
  client_credentials, config, file), discovery with datasphere_show_spaces,
  datasphere_show_assets, datasphere_describe_space and
  datasphere_describe_asset, reading relational assets with
  datasphere_read_relational and analytic models with
  datasphere_read_analytical (metrics, dimensions, params, top, skip), and
  SAC planning and analytic models with sac_read_planning_data,
  sac_read_analytical and sac_read_story_data using an access_token secret.
  Also covers what is not implemented yet (SAC discovery functions, ATTACH
  TYPE sac). Use when the user wants data from a Datasphere space or an SAC
  model, or asks how to log in to Datasphere from DuckDB.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — SAP Datasphere and SAP Analytics Cloud

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v2.0.0 / v1.5.6

## Critical gotchas

1. **Interactive Datasphere login needs a browser.** `TYPE datasphere, PROVIDER oauth2`
   defaults to `grant_type 'authorization_code'`: the first call opens a browser and runs a
   callback server on `http://localhost:65000`. For CI, cron and containers create an OAuth
   client of type *Technical User* in Datasphere and use `GRANT_TYPE 'client_credentials'`.
2. **The redirect URI must match the Datasphere OAuth client exactly.** Register
   `http://localhost:65000` (or whatever you pass as `REDIRECT_URI`) on the OAuth client in
   *System → Administration → App Integration*.
3. **Only assets exposed for consumption are readable.** In the Data Builder the view must
   have *Expose for Consumption* switched on. Analytic models are read with
   `datasphere_read_analytical`; tables and relational views with
   `datasphere_read_relational`. `datasphere_describe_asset` tells you which access an asset
   supports (`has_relational_access`, `has_analytical_access`).
4. **Analytic reads aggregate.** `datasphere_read_analytical` returns the measures
   aggregated by the dimensions you select. Choosing fewer `dimensions` returns fewer,
   more aggregated rows. That is not data loss.
5. **Input parameters are mandatory where the model declares them.** Pass them with
   `params := MAP {'P_YEAR': '2024'}`, or the service answers 400.
6. **SAC discovery is not implemented yet.** `sac_show_models`, `sac_show_stories`,
   `sac_describe_model` and `sac_describe_story` raise *"not implemented yet"* (GitHub #243),
   and so does `ATTACH ... (TYPE sac)` (#252). Take model and story IDs from SAC itself
   (the URL of the model or story).
7. **SAC needs a token you obtained yourself.** Only `PROVIDER access_token` works today. SAC
   tokens are short-lived, so refresh the secret before long jobs.

## SAP Datasphere

### Secret

```sql
-- Interactive (desktop): opens a browser on first use, stores the refresh token
CREATE PERSISTENT SECRET dsp (
    TYPE datasphere,
    PROVIDER oauth2,
    TENANT_NAME 'my-tenant',          -- from https://my-tenant.eu10.hcs.cloud.sap
    DATA_CENTER 'eu10',
    CLIENT_ID 'sb-...',
    CLIENT_SECRET '...',
    REDIRECT_URI 'http://localhost:65000'
);

-- Unattended
CREATE SECRET dsp_tech (
    TYPE datasphere,
    PROVIDER oauth2,
    TENANT_NAME 'my-tenant',
    DATA_CENTER 'eu10',
    CLIENT_ID 'sb-...',
    CLIENT_SECRET '...',
    GRANT_TYPE 'client_credentials'
);
```

`TOKEN_URL` overrides the token endpoint when your tenant does not follow the
`tenant_name` + `data_center` pattern. `PROVIDER config` (`CONFIG_FILE`) and
`PROVIDER file` (`FILEPATH`) keep credentials outside SQL.

### Discover

```sql
SELECT * FROM datasphere_show_spaces();                -- name
SELECT * FROM datasphere_show_assets('SALES');         -- name, object_type, technical_name
SELECT * FROM datasphere_show_assets();                -- + space_name, across all spaces
SELECT * FROM datasphere_describe_space('SALES');

-- 15 columns: data URLs, relational_schema, analytical_schema (measures, dimensions,
-- variables), has_relational_access, has_analytical_access, ...
SELECT name, has_relational_access, has_analytical_access,
       analytical_schema.measures, analytical_schema.dimensions
FROM datasphere_describe_asset('SALES', 'AM_REVENUE');
```

All of these accept `secret := 'name'` when there is more than one datasphere secret.

### Read

```sql
datasphere_read_relational(space_id, asset_id [, secret],
                           top := UBIGINT, skip := UBIGINT, params := MAP(VARCHAR, VARCHAR),
                           secret := VARCHAR) → TABLE
datasphere_read_analytical(space_id, asset_id [, secret],
                           metrics := VARCHAR[], dimensions := VARCHAR[],
                           top := ..., skip := ..., params := ..., secret := ...) → TABLE
```

```sql
-- Relational: WHERE and column selection push down as $filter / $select
SELECT CustomerID, Country, Revenue
FROM datasphere_read_relational('SALES', 'V_SALES_ORDERS')
WHERE Country = 'DE';

-- Analytical: metrics and dimensions become the $select of the analytic query
SELECT *
FROM datasphere_read_analytical('SALES', 'AM_REVENUE',
                                metrics := ['Revenue', 'Quantity'],
                                dimensions := ['Country', 'FiscalYear'],
                                params := MAP {'P_CURRENCY': 'EUR'});
```

## SAP Analytics Cloud

```sql
CREATE SECRET my_sac (
    TYPE sac,
    PROVIDER access_token,
    TENANT_NAME 'my-tenant',
    REGION 'eu10',                -- eu10, us10, ap10, ca10, jp10, au10, br10, ch10
    ACCESS_TOKEN '...'
);
```

```sql
sac_read_planning_data(model_id, secret := , top := , skip := , params := ) → TABLE
sac_read_analytical(model_id, secret := , dimensions := 'A,B', measures := 'M1,M2',
                    top := , skip := , params := ) → TABLE
sac_read_story_data(story_id, secret := ) → TABLE
```

```sql
-- Planning model data
SELECT * FROM sac_read_planning_data('REVENUE_MODEL', secret := 'my_sac', top := 1000);

-- Analytic model: dimensions and measures are comma-separated strings here
SELECT *
FROM sac_read_analytical('SALES_CUBE', secret := 'my_sac',
                         dimensions := 'Territory,Product,Quarter',
                         measures := 'SalesAmount,UnitsShipped');

-- Data behind a story's widgets
SELECT * FROM sac_read_story_data('STORY_ID', secret := 'my_sac');
```

Note the inconsistency: Datasphere takes `metrics` / `dimensions` as `VARCHAR[]` lists;
SAC takes `measures` / `dimensions` as one comma-separated `VARCHAR`.

## Canonical workflow: Datasphere model to local mart

```sql
-- 1. Unattended secret (see gotcha 1)
CREATE SECRET dsp (TYPE datasphere, PROVIDER oauth2, TENANT_NAME 'my-tenant', DATA_CENTER 'eu10',
                   CLIENT_ID 'sb-...', CLIENT_SECRET '...', GRANT_TYPE 'client_credentials');

-- 2. Find the asset and check how it can be read
SELECT name, object_type, technical_name FROM datasphere_show_assets('SALES');
SELECT has_relational_access, has_analytical_access, analytical_schema
FROM datasphere_describe_asset('SALES', 'AM_REVENUE');

-- 3. Land an aggregated slice
CREATE OR REPLACE TABLE revenue_by_country AS
SELECT *
FROM datasphere_read_analytical('SALES', 'AM_REVENUE',
                                metrics := ['Revenue'],
                                dimensions := ['Country', 'FiscalYear', 'FiscalPeriod']);
```

From there, the anofox-forecast plugin's `anofox-forecast-models` skill forecasts the
series, and anofox-visualization's `build-dashboard` skill (available inside that
repository) builds a dashboard on top.

See also: `erpl-odata-setup` (secret basics, TLS), `erpl-odata-production` (unattended
tokens, scheduling, tracing), `erpl-odata-sap-odp-replication` (delta from S/4 sources that
feed Datasphere).

Reference docs:
- `README.md` (SAP Datasphere, SAP Analytics Cloud)
