---
name: erpl-odata-dynamics-and-delta-sharing
description: >
  Reading Microsoft Dynamics 365 Business Central (bc_show_companies,
  bc_show_entities, bc_describe, bc_read with company and expand, ATTACH TYPE
  business_central), Dataverse / Dynamics 365 CRM (crm_show_entities,
  crm_describe, crm_read) and Delta Sharing shares from Databricks, SAP
  Business Data Cloud and other providers (delta_share_show_shares,
  delta_share_show_schemas, delta_share_show_tables, delta_share_scan,
  profile files) into DuckDB with erpl_odata. Use when the user wants ERP
  data from Business Central, CRM records from Dataverse, or a table shared
  over the Delta Sharing protocol.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — Dynamics 365 (Business Central, Dataverse) and Delta Sharing

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v1.4.5 LTS / v1.5.6 / v2.0

## Critical gotchas

1. **`bc_read` needs a company.** Pass `company := 'CRONUS DE'` (the company *name* as
   `bc_show_companies()` reports it, or its GUID). Without it the read fails with *"No
   company found with name ''"*.
2. **Business Central needs two registrations.** The Entra app needs the
   `API.ReadWrite.All` application permission on *Dynamics 365 Business Central*, **and** an
   *Microsoft Entra Applications* record inside Business Central with permission sets
   assigned (for example `D365 READ`). Without the second one every call is 401/403.
3. **The token audience is fixed.** Business Central and Datasphere refuse an environment
   that points at a host other than the official API, so a token is never sent to a host
   it was not minted for. Only for a deliberate proxy:
   `SET erpl_unsafe_allow_custom_service_urls = 'I_UNDERSTAND_THIS_SENDS_TOKENS_ELSEWHERE';`.
   Dataverse is not gated, because its token is minted for the configured org URL.
4. **Dataverse needs an application user.** Create an *Application user* for the app in the
   Power Platform admin center and give it a security role. API permission alone is not
   enough.
5. **Delta Sharing `ATTACH` is not implemented.** `ATTACH ... (TYPE delta_share)` raises
   *"not implemented yet"*. Use the `delta_share_*` functions.
6. **A column type the share uses but DuckDB cannot map fails the scan** rather than being
   stringified. Select the supported columns via a view on the provider side, or report it.
7. **The profile file is the credential.** A Delta Sharing profile contains the bearer
   token. Keep it out of the repository; it can live on S3 or Azure storage, read through
   DuckDB's filesystem secrets.

## Business Central

```sql
CREATE SECRET bc (TYPE business_central, TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...',
                  ENVIRONMENT 'production');

SELECT id, name, displayName FROM bc_show_companies();
SELECT name, url, kind FROM bc_show_entities();
SELECT property_name, property_type, nullable, is_key FROM bc_describe('customers');

-- Columns keep the API's camelCase names. WHERE and column selection push down
SELECT number, displayName, email, balanceDue
FROM bc_read('customers', company := 'CRONUS DE')
WHERE balanceDue > 1000;

-- Lines with their header
SELECT number, customerName, salesOrderLines
FROM bc_read('salesOrders', company := 'CRONUS DE', expand := 'salesOrderLines');
```

Attach a company to browse it as a catalog (read-only):

```sql
ATTACH 'bc' AS cronus (TYPE business_central, COMPANY 'CRONUS DE');   -- path = secret name
SHOW TABLES FROM cronus;
SELECT * FROM cronus.customers LIMIT 10;
```

Column names are the API's own camelCase names (`displayName`, `balanceDue`), not snake_case.
Quote them only if you change their case. Entity names follow the Business Central API v2.0 (`customers`, `vendors`, `items`,
`salesInvoices`, `generalLedgerEntries`, ...). Use `bc_show_entities()` for the list your
environment exposes, including custom API pages.

## Dataverse / Dynamics 365 CRM

```sql
CREATE SECRET crm (TYPE dataverse, TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...',
                   ENVIRONMENT_URL 'https://myorg.crm4.dynamics.com');

-- Raw EntityDefinitions metadata (PascalCase): LogicalName, EntitySetName, ...
SELECT LogicalName, EntitySetName FROM crm_show_entities();
SELECT attribute_name, attribute_type, nullable, is_primary FROM crm_describe('accounts');

-- WHERE pushes down; statecode 0 = active
SELECT name, emailaddress1, telephone1, revenue
FROM crm_read('accounts')
WHERE statecode = 0
ORDER BY revenue DESC;

SELECT name, primarycontactid FROM crm_read('accounts', expand := 'primarycontactid');
```

Use the entity **set** name (plural, `accounts`, `contacts`, `opportunities`, the
`EntitySetName` column of `crm_show_entities()`) for `crm_read`, as the Web API expects. Option-set columns come back as integer codes; join
them with the option-set metadata or map them in a `CASE`.

## Delta Sharing

Profile file (from the data provider):

```json
{
  "shareCredentialsVersion": 1,
  "endpoint": "https://sharing.example.com/delta-sharing/",
  "bearerToken": "..."
}
```

```sql
SELECT share_name, share_id   FROM delta_share_show_shares('/secure/profile.share');
SELECT schema_name, share FROM delta_share_show_schemas('/secure/profile.share', 'sales_share');
SELECT table_name, schema, share, table_id, description
FROM delta_share_show_tables('/secure/profile.share', 'sales_share', 'default');

-- The table itself: columns and types come from the share's Delta schema
SELECT * FROM delta_share_scan('/secure/profile.share', 'sales_share', 'default', 'orders') LIMIT 10;

CREATE TABLE orders AS
SELECT * FROM delta_share_scan('/secure/profile.share', 'sales_share', 'default', 'orders');
```

The profile path may be local, `https://`, `s3://` or `azure://`. Remote paths go through
DuckDB's filesystem, so `httpfs` / `azure` and their secrets apply.

Public example (Databricks open datasets):

```sql
SELECT *
FROM delta_share_scan('https://databricks-datasets-oregon.s3-us-west-2.amazonaws.com/delta-sharing/share/open-datasets.share',
                      'delta_sharing', 'default', 'owid-covid-data')
LIMIT 5;
```

## Canonical workflow: ERP customers and CRM accounts side by side

```sql
CREATE OR REPLACE TABLE bc_customers AS
SELECT number, displayName, email, balanceDue FROM bc_read('customers', company := 'CRONUS DE');

CREATE OR REPLACE TABLE crm_accounts AS
SELECT accountnumber, name, emailaddress1, revenue FROM crm_read('accounts') WHERE statecode = 0;

-- Accounts that exist in CRM but not in the ERP
SELECT a.* FROM crm_accounts a
ANTI JOIN bc_customers c ON lower(c.email) = lower(a.emailaddress1);
```

Matching master data across systems by name or address is fuzzy work; the anofox-tabular
plugin's `anofox-tabular-master-data` skill validates and standardises emails, phone
numbers, VAT ids and addresses first, and `anofox-tabular-diff` compares the two tables.

See also: `erpl-odata-setup` (secret types), `erpl-odata-microsoft-365` (Graph APIs on the
same tenant), `erpl-odata-production` (scheduling, tracing, error playbook).

Reference docs:
- `README.md` (Business Central, Dataverse, Delta Sharing)
- `docs/TLS.md` (custom service URLs)
