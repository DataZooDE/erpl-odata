<a name="top"></a>

[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.5.6+-green.svg)](https://duckdb.org)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()

> **Formerly `erpl_web`.** This extension was renamed to `erpl_odata`; all functions, secrets and settings are unchanged. On the community repository the old `erpl_web` name is an alias that installs and loads `erpl_odata`, so existing scripts keep working; from `http://get.erpl.io`, `LOAD erpl_web` fails with a message pointing here. Update your scripts to `INSTALL erpl_odata FROM community; LOAD erpl_odata;`.

# ERPL-OData — DuckDB HTTP + OData + Delta Sharing + SAP + Microsoft 365 Extension

ERPL-OData is a production-grade DuckDB extension that lets you call HTTP/REST APIs, query OData v2/v4 services, read Delta Sharing tables, work with SAP Datasphere and SAP Analytics Cloud, and query Microsoft 365 services (Graph API, Entra ID, Business Central, Dataverse) directly from SQL. It brings secure OAuth2, DuckDB Secrets integration, predicate pushdown, robust tracing, and smart caching into a single, easy-to-use package.

- SEO topics: DuckDB HTTP client, DuckDB REST, DuckDB OData v2/v4, DuckDB Delta Sharing, DuckDB Delta Lake, Databricks Delta, DuckDB SAP Datasphere, DuckDB SAP Analytics Cloud, DuckDB Microsoft 365, DuckDB Microsoft Graph API, DuckDB Entra ID, DuckDB Business Central, OAuth2 for DuckDB, OData ATTACH, Delta Sharing ATTACH, query APIs from SQL.

## ✨ Highlights

- HTTP from SQL: GET, HEAD, POST, PUT, PATCH, DELETE with headers, auth, and body
- OData v2/v4: Universal reader with automatic version handling and pushdown
- Delta Sharing: Query Databricks, SAP, and other Delta Sharing protocol-compliant services via Parquet files
- SAP Datasphere: List spaces/assets, describe assets, and read relational/analytical data
- SAP Analytics Cloud: Query models, stories, discover dimensions/measures with automatic schema
- Microsoft 365: Query Entra ID users/groups, SharePoint lists/Excel, Teams, Outlook, Planner via Graph API; attach SharePoint lists as a read-only catalog with `ATTACH ... TYPE sharepoint_lists`
- Microsoft Dynamics 365: Read Business Central ERP and Dataverse/CRM data from SQL
- OAuth2 + Secrets: Secure flows with refresh, client credentials, and secret providers
- Tracing: Deep, configurable tracing for debugging and performance tuning
- Caching + Resilience: Response caching, metadata caching, and retry logic
- Charset handling: Automatic UTF‑8/ISO‑8859‑1/‑15/Windows‑1252 detection and conversion

---

## 🚀 Install

You can either install it from the DuckDB community repository

```sql
INSTALL erpl_odata FROM community;
LOAD erpl_odata;
```

or using our source

```sql
-- Install (DuckDB requires -unsigned when installing from a custom URL)
INSTALL 'erpl_odata' FROM 'http://get.erpl.io';
LOAD 'erpl_odata';
```

or build from source (developers):

```bash
make debug
```

---

## ⚡ Quick Start

### HTTP in one line

```sql
SELECT content
FROM http_get('https://httpbun.com/ip');

SELECT content::JSON->>'ip' AS ip_address
FROM http_get('https://httpbun.com/ip');
```

Add headers and auth:

```sql
SELECT status, content
FROM http_get(
  'https://api.example.com/data',
  headers:={'Authorization':'Bearer token123','X-Trace':'on'}
);
```

### OData v2/v4 in SQL

Attach and query any OData service:

```sql
-- OData v4
ATTACH 'https://services.odata.org/TripPinRESTierService' AS trippin (TYPE odata);
SELECT UserName, FirstName, LastName FROM trippin.People WHERE Gender='Female';

-- OData v2
ATTACH 'https://services.odata.org/V2/Northwind/Northwind.svc' AS northwind (TYPE odata);
SELECT CustomerID, CompanyName FROM northwind.Customers WHERE Country='Germany';
```

Read directly via `odata_read`:

```sql
SELECT UserName, AddressInfo[1].City."Name" AS city
FROM odata_read('https://services.odata.org/TripPinRESTierService/People')
WHERE UserName='angelhuffman';
```

### Delta Sharing (Databricks, SAP, others)

Query Delta Sharing repositories with profile files from **local paths, HTTP URLs, or S3 URIs**:

```sql
-- Local profile file
SELECT url, size FROM delta_share_scan(
  './profile.json',
  'share_name',
  'schema_name',
  'table_name'
) LIMIT 10;

-- HTTP URL (remote profile) - Example with public Delta Sharing server
SELECT url, size FROM delta_share_scan(
  'https://databricks-datasets-oregon.s3-us-west-2.amazonaws.com/delta-sharing/share/open-datasets.share',
  'delta_sharing',
  'default',
  'owid-covid-data'
) LIMIT 10;

-- S3 URI (requires S3 credentials via CREATE SECRET)
SELECT url, size FROM delta_share_scan(
  's3://my-bucket/profiles/delta.share',
  'delta_sharing',
  'default',
  'your_table_name'
) LIMIT 10;

-- Read data from shared Parquet files (using parquet_scan):
SELECT * FROM read_parquet(
  'https://presigned-url-from-delta-share.s3.amazonaws.com/...'
);
```

**Profile Format (JSON):**
```json
{
  "shareCredentialsVersion": 1,
  "endpoint": "https://sharing.databricks.com",
  "bearerToken": "YOUR_TOKEN"
}
```

**Features:**
- ✅ Local file paths: `./profile.json`, `/path/to/profile.share`
- ✅ HTTP URLs: `https://example.com/profile.share` (auto-downloaded)
- ✅ S3 URIs: `s3://bucket/profile.share` (requires S3 secrets)
- ✅ Azure URLs: `azure://container/profile.share`
- ✅ Bearer token authentication
- ✅ NDJSON response parsing
- ✅ Pre-signed URLs to cloud storage
- ✅ Works with Databricks, SAP, and any Delta Sharing protocol-compliant service
- ✅ Remote profiles transparently loaded via DuckDB's FileSystem API
- Full documentation in `DELTA_SHARE_TESTING.md`

### Delta Sharing Discovery (Catalog Functions)

Discover available shares, schemas, and tables before reading data:

```sql
-- List all shares in a Delta Sharing profile
SELECT share_name, share_id
FROM delta_share_show_shares('./profile.json');

-- List schemas in a specific share
SELECT schema_name, schema_id
FROM delta_share_show_schemas('./profile.json', 'delta_sharing');

-- List tables in a schema
SELECT name, share_name, schema_name
FROM delta_share_show_tables('./profile.json', 'delta_sharing', 'default');
```

**Features:**
- ✅ Discover share structures before querying
- ✅ Works with all profile types (local, HTTP, S3)
- ✅ Helps explore available data
- ⚠️ ATTACH support for Delta Sharing (`ATTACH ... TYPE delta_share`) is planned but not yet implemented

### Microsoft 365 in minutes

Query your organization's users, SharePoint lists, or Teams data in three steps:

```sql
-- 1. Create a secret (Azure App Registration with client credentials)
CREATE SECRET ms (
  TYPE microsoft_graph,
  tenant_id 'your-tenant-id',
  client_id 'your-client-id',
  client_secret 'your-client-secret'
);

-- 2. Query Entra ID users
SELECT display_name, mail, department FROM graph_users();

-- 3. Attach a SharePoint site as a read-only catalog of list tables
ATTACH 'Finance' AS finance_sp (TYPE sharepoint_lists, SECRET 'ms');

-- 4. Query the attached tables directly
SELECT * FROM finance_sp.main."Project Tracker" WHERE status = 'Active';
```

---

### SAP Datasphere in minutes

1) Create a Datasphere secret (OAuth2).

```sql
-- Minimal OAuth2 secret (authorization_code or client_credentials)
CREATE SECRET datasphere (
  TYPE datasphere,
  PROVIDER oauth2,
  TENANT_NAME 'your-tenant',
  DATA_CENTER 'eu10',
  CLIENT_ID '...optional-if-pre-delivered...',
  CLIENT_SECRET '...optional-if-pre-delivered...',
  SCOPE 'default',                     -- or service-specific scopes
  REDIRECT_URI 'http://localhost:65000' -- default
);
```

2) Explore spaces and assets:

```sql
-- List spaces (DWAAS core)
SELECT * FROM datasphere_show_spaces();

-- List assets in a space (business name, object type, technical name)
SELECT * FROM datasphere_show_assets('MY_SPACE');

-- List assets across all accessible spaces
SELECT * FROM datasphere_show_assets();
```

3) Describe and read data:

```sql
-- Describe a space
SELECT * FROM datasphere_describe_space('MY_SPACE');

-- Describe an asset (15 columns incl. relational/analytical schema)
SELECT * FROM datasphere_describe_asset('MY_SPACE','MART_DIM_LEAD');

-- Read relational data
SELECT * FROM datasphere_read_relational('MY_SPACE','MY_TABLE') LIMIT 10;

-- Read analytical data (metrics + dimensions -> $select)
SELECT *
FROM datasphere_read_analytical(
  'MY_SPACE','MY_CUBE',
  metrics:=['Revenue','Count'],
  dimensions:=['Company','Country']
) LIMIT 10;
```

---

## 🌐 HTTP Functions (REST from SQL)

Functions return rows with consistent columns: `method`, `status`, `url`, `headers`, `content_type`, `content`.

- `http_get(url, [headers], [accept], [auth], [auth_type], [timeout])`
- `http_head(url, [headers], [accept], [auth], [auth_type], [timeout])`
- `http_post(url, body, [content_type], [headers], [accept], [auth], [auth_type], [timeout])`
- `http_put(url, body, [content_type], [headers], [accept], [auth], [auth_type], [timeout])`
- `http_patch(url, body, [content_type], [headers], [accept], [auth], [auth_type], [timeout])`
- `http_delete(url, body, [content_type], [headers], [accept], [auth], [auth_type], [timeout])`

### Authentication Precedence

HTTP functions support two authentication methods with clear precedence:

1. **Function Parameters (Highest Priority)**: Use the `auth` and `auth_type` named parameters
2. **DuckDB Secrets (Fallback)**: Use registered secrets scoped to the URL

**Important**: When the `auth` parameter is provided, it **always takes precedence** over registered secrets, regardless of whether a secret exists for the URL.

#### Using Function Parameters

```sql
-- Basic authentication (default auth_type)
SELECT * FROM http_get('https://api.example.com/data', 
                      auth := 'username:password');

-- Bearer token authentication
SELECT * FROM http_get('https://api.example.com/data', 
                      auth := 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...',
                      auth_type := 'BEARER');

-- Username only (password will be empty, defaults to BASIC auth)
SELECT * FROM http_get('https://api.example.com/data', 
                      auth := 'username_only');

-- Explicit BASIC authentication
SELECT * FROM http_get('https://api.example.com/data', 
                      auth := 'user:pass',
                      auth_type := 'BASIC');
```

#### Using DuckDB Secrets (Fallback)

When no `auth` parameter is provided, the functions automatically use registered secrets:

```sql
-- Create a secret for basic authentication
CREATE SECRET api_auth (
  TYPE http_basic,
  USERNAME 'secret_user',
  PASSWORD 'secret_pass',
  SCOPE 'https://api.example.com/'
);

-- This will use the secret (no auth parameter)
SELECT * FROM http_get('https://api.example.com/data');

-- This will use the auth parameter and ignore the secret
SELECT * FROM http_get('https://api.example.com/data', 
                      auth := 'override_user:override_pass');
```

#### Authentication Type Support

- **BASIC**: Username and password (format: `username:password`)
- **BEARER**: Token-based authentication (the entire `auth` value is used as the token)

**Note**: The `auth_type` parameter defaults to `BASIC` when not specified.

### Examples

```sql
-- JSON body
SELECT *
FROM http_post(
  'https://httpbin.org/anything',
  {'name':'Alice','age':30}::JSON
);

-- Text body
SELECT * FROM http_post('https://httpbin.org/anything','Hello','text/plain');

-- Form body
SELECT * FROM http_post('https://httpbin.org/anything','a=1&b=2','application/x-www-form-urlencoded');
```

### TLS Certificate Verification

Every HTTPS request made by ERPL-OData verifies the server certificate. If you connect
to an on-premise system with a private or self-signed certificate, trust its CA
rather than turning verification off:

```sql
SET erpl_ca_cert_file = '/etc/erpl/my-corporate-ca.pem';
```

See [docs/TLS.md](docs/TLS.md) for the full picture, including the deliberately
awkward last-resort opt-out.

### Debugging Authentication

Enable tracing to see which authentication source is being used:

```sql
SET erpl_trace_enabled = TRUE;
SET erpl_trace_level = 'DEBUG';

-- This will show "Using auth parameter" in the trace
SELECT * FROM http_get('https://api.example.com/data', auth := 'user:pass');

-- This will show "No auth parameter provided, using registered secrets" in the trace
SELECT * FROM http_get('https://api.example.com/data');
```

Character sets and binary:

- Content types are inspected and converted to UTF‑8 for textual responses (UTF‑8/ISO‑8859‑1/‑15/Windows‑1252). Binary types are returned as-is.

---

## 📊 OData v2/v4

Two styles are supported:

1) Attach as a DuckDB database: `ATTACH '...url...' AS name (TYPE odata)`
2) Read directly: `SELECT ... FROM odata_read('...entity_set_url...')`

Capabilities:

- Automatic OData version detection (v2/v4)
- Predicate pushdown: `$filter`, `$select`, `$top`, `$skip`
- Navigation property expansion via `expand=` (inline related entities as nested structs)
- Pagination handling and total-count awareness
- EDM-aware type mapping into DuckDB types

Example:

```sql
-- Filter pushdown — WHERE is translated to $filter on the server
SELECT OrderID, CustomerID
FROM odata_read('https://services.odata.org/V2/Northwind/Northwind.svc/Orders')
WHERE OrderDate >= '1996-01-01'
LIMIT 5;

-- Expand related entities inline (Orders as a nested struct array per customer)
SELECT CustomerID, CompanyName, Country, Orders
FROM odata_read(
    'https://services.odata.org/V2/Northwind/Northwind.svc/Customers',
    expand = 'Orders'
)
WHERE Country = 'Germany';

-- Fail the query on the first value that cannot be converted to its column
-- type, instead of receiving it as a NULL that looks like server-sent NULL
SELECT * FROM odata_read('https://example.com/svc/Orders', strict_typing = true);

-- Ask the service for larger pages, so a big extraction needs fewer round trips
SELECT * FROM odata_read('https://example.com/svc/Orders', max_page_size = 5000);
```

By default (`strict_typing = false`) a value that cannot be converted is
returned as `NULL`, the rest of the row is kept, and a warning naming the
column, the number of failures and the first offending value is printed once
per scan. Values are never silently replaced by `0`, `''` or `false`.

`max_page_size` sends `Prefer: odata.maxpagesize=N`, which is OData's way for a
client to ask for larger pages. It is a *preference*: a service is free to
ignore it or to cap it lower, and the reader follows whatever pages it actually
gets. Omit the parameter to send no preference at all — that is not the same as
sending a default, since a `Prefer` header can itself change how a service
pages. Raising it is the supported answer when a very large extraction hits the
10,000-page server-driven paging ceiling.

Attach usage:

```sql
ATTACH 'https://services.odata.org/TripPinRESTierService' AS trippin (TYPE odata);
SELECT TOP 5 UserName, FirstName FROM trippin.People;
```

---

## 🔁 SAP ODP (Operational Data Provisioning)

`odp_odata_read()` reads SAP ODP delta-enabled services, doing a full initial load once and then
fetching only what changed on each subsequent read.

```sql
SELECT * FROM odp_odata_read('https://sap.example.com/sap/opu/odata/sap/ZSRV/EntitySet');
SELECT * FROM odp_odata_list_subscriptions();
```

> ⚠️ **`odp_odata_read` returns a change stream, not a snapshot.** After the initial load each read
> returns inserts, updates **and deletes**, marked by the `ODQ_CHANGEMODE` column (`'D'` = delete).
> A plain `INSERT INTO ... SELECT` will silently apply deletes as inserts. See
> **[docs/ODP.md](docs/ODP.md)** for the full contract and a correct merge pattern.

---

## 🟦 SAP Datasphere (DWAAS Core + Catalog)

ERPL-OData includes first-class support for SAP Datasphere using a secured OAuth2 secret. The extension integrates both the DWAAS core APIs and the Catalog OData service to provide discovery and rich metadata.

### Functions overview

- `datasphere_show_spaces()` → `name`
- `datasphere_show_assets(space_id)` → `name`, `object_type`, `technical_name`
- `datasphere_show_assets()` → `name`, `object_type`, `technical_name`, `space_name`
- `datasphere_describe_space(space_id)` → `name`, `label`
- `datasphere_describe_asset(space_id, asset_id)` → 15 columns including:
  - Basic: `name`, `space_name`, `label`, `asset_relational_metadata_url`, `asset_relational_data_url`, `asset_analytical_metadata_url`, `asset_analytical_data_url`, `supports_analytical_queries`
  - Extended: `odata_context`, `relational_schema` (STRUCT of `columns(name, technical_name, type, length)`), `analytical_schema` (STRUCT of `measures|dimensions|variables` each as LIST of STRUCTs `name`, `type`, `edm_type`), `has_relational_access`, `has_analytical_access`, `asset_type`, `odata_metadata_etag`

- Readers with pushdown and parameters:
  - `datasphere_read_relational(space_id, asset_id [, secret])`
    - Named params: `top`, `skip`, `params` (MAP<VARCHAR,VARCHAR>), `secret`
  - `datasphere_read_analytical(space_id, asset_id [, secret])`
    - Named params: `top`, `skip`, `params` (MAP<VARCHAR,VARCHAR>), `secret`, `metrics` (LIST<VARCHAR>), `dimensions` (LIST<VARCHAR>)

Tips:

- `params` passes input parameters where required by the service definition.
- `metrics` and `dimensions` are translated into an OData `$select` automatically.
- Snake_case column names are used consistently in outputs.

### Secrets and OAuth2 flows

Create secrets using providers:

```sql
-- OAuth2 provider (interactive authorization_code or client_credentials)
CREATE SECRET datasphere (
  TYPE datasphere,
  PROVIDER oauth2,
  TENANT_NAME 'your-tenant',
  DATA_CENTER 'eu10',
  SCOPE 'default'
);

-- Config provider (INI-like file with key=value)
CREATE SECRET datasphere_cfg (
  TYPE datasphere,
  PROVIDER config,
  CONFIG_FILE '/path/to/datasphere.conf'
);

-- File provider (store path to credentials artifact)
CREATE SECRET datasphere_file (
  TYPE datasphere,
  PROVIDER file,
  FILEPATH '/secure/path/creds.json'
);
```

Token management is automatic. For `client_credentials`, provide `client_id`, `client_secret`, and either `token_url` or both `tenant_name` and `data_center`. For `authorization_code`, a short local callback server is used to collect the code, then tokens are persisted back into the DuckDB secret.

---

## 🔎 Tracing & Diagnostics

Enable rich tracing to debug network calls and pushdown logic:

```sql
SET erpl_trace_enabled = TRUE;
SET erpl_trace_level   = 'DEBUG';   -- TRACE, DEBUG, INFO, WARN, ERROR
SET erpl_trace_output  = 'both';    -- console, file, both
-- Optional file tuning
SET erpl_trace_file_path   = './erpl_trace.log';
SET erpl_trace_max_file_size = 10485760;  -- 10MB
SET erpl_trace_rotation      = TRUE;
```

What you get:

- URLs, headers, timing, pagination, and retry info
- OData metadata and column extraction
- Datasphere endpoint selection details and schema summaries

---

## 🧠 Performance & Resilience

- Response caching for HTTP; metadata caching for OData
- Exponential backoff and retry handling for transient errors
- Connection reuse/keep-alive for efficient throughput

Best practices:

- Use `SELECT ... LIMIT ...` to control row counts during exploration
- Favor predicate pushdown filters to reduce transferred data
- For analytical reads, specify `metrics`/`dimensions` to generate optimal `$select`

---

## 🧪 Testing

```bash
make test
```

If you need to run the C++ unit tests binary directly, use your build folder path. For example:

```bash
./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests
```

---

## 📖 Reference

### HTTP named parameters

- `headers`: MAP(VARCHAR, VARCHAR)
- `content_type`: VARCHAR (for request body)
- `accept`: VARCHAR
- `auth`: VARCHAR (`'user:pass'` for BASIC, token string for BEARER)
- `auth_type`: VARCHAR (`'BASIC'|'BEARER'`)
- `timeout`: BIGINT (ms)

### Datasphere readers

- `datasphere_read_relational(space_id, asset_id)`
  - Named parameters: `secret` VARCHAR, `top` UBIGINT, `skip` UBIGINT, `params` MAP<VARCHAR,VARCHAR>

- `datasphere_read_analytical(space_id, asset_id)`
  - Named parameters: `secret` VARCHAR, `top` UBIGINT, `skip` UBIGINT, `params` MAP<VARCHAR,VARCHAR>, `metrics` LIST<VARCHAR>, `dimensions` LIST<VARCHAR>

---

## ☁️ SAP Analytics Cloud (SAC)

Query planning models, analytics models, and stories from SAP Analytics Cloud directly in SQL with automatic schema discovery and predicate pushdown.

### Setup SAC Secret

```sql
-- Create SAC OAuth2 secret
CREATE SECRET my_sac (
  TYPE sac,
  PROVIDER oauth2,
  TENANT_NAME 'your-tenant',
  REGION 'eu10',                    -- eu10, us10, ap10, ca10, jp10, au10, br10, ch10
  CLIENT_ID 'your-client-id',
  CLIENT_SECRET 'your-client-secret',
  SCOPE 'openid'
);
```

### SAC Discovery Functions

Discover available models and stories:

⚠️ **Implementation Note:** SAC catalog discovery functions (`sac_show_models`, `sac_show_stories`, `sac_get_model_info`, `sac_get_story_info`) are **stub implementations** and currently return empty results. For querying SAC data, use the fully functional data reading functions listed below instead.

```sql
-- For data reading (fully functional):
SELECT * FROM sac_read_planning_data('REVENUE_MODEL', secret := 'my_sac', top := 1000);
SELECT * FROM sac_read_analytical('SALES_CUBE', secret := 'my_sac', dimensions := 'Territory', measures := 'SalesAmount');
SELECT * FROM sac_read_story_data('DASHBOARD_001', secret := 'my_sac');
```

### SAC Data Reading

Query data directly from SAC models:

```sql
-- Read planning model data
SELECT *
FROM sac_read_planning_data('REVENUE_MODEL', secret := 'my_sac', top := 1000);

-- Read analytics model with dimension/measure filtering
SELECT *
FROM sac_read_analytical(
  'SALES_CUBE',
  secret := 'my_sac',
  dimensions := 'Territory,Product,Quarter',
  measures := 'SalesAmount,UnitsShipped',
  top := 5000
);

-- Extract data from stories
SELECT * FROM sac_read_story_data('DASHBOARD_001', secret := 'my_sac');
```

### SAC ATTACH (Direct SQL Access)

For direct SQL access to SAC OData services:

```sql
-- Attach SAC instance
ATTACH 'https://your-tenant.eu10.sapanalytics.cloud' AS sac (
  TYPE sac,
  SECRET my_sac
);

-- Query attached models
SELECT * FROM sac.Planning_Models WHERE ID = 'REVENUE_MODEL';
SELECT * FROM sac.Stories WHERE Owner = 'john.doe@company.com';
```

### SAC Functions Reference

**Discovery Functions (⚠️ Stub Implementations - Return Empty Results):**
- `sac_show_models()`
  - Named parameters: `secret` VARCHAR
  - Returns: id, name, description, type, owner, created_at, last_modified_at

- `sac_show_stories()`
  - Named parameters: `secret` VARCHAR
  - Returns: id, name, description, owner, created_at, last_modified_at, status

- `sac_get_model_info(model_id)`
  - Named parameters: `secret` VARCHAR
  - Returns: id, name, description, type, dimensions (comma-separated), created_at

- `sac_get_story_info(story_id)`
  - Named parameters: `secret` VARCHAR
  - Returns: id, name, description, owner, status, created_at, last_modified_at

**Data Reading Functions (✅ Fully Functional):**
- `sac_read_planning_data(model_id)`
  - Named parameters: `secret` VARCHAR, `top` UBIGINT, `skip` UBIGINT
  - Returns: All columns from the planning model (auto-detected schema)

- `sac_read_analytical(model_id)`
  - Named parameters: `secret` VARCHAR, `top` UBIGINT, `skip` UBIGINT, `dimensions` VARCHAR, `measures` VARCHAR
  - Returns: Selected dimensions and measures with aggregated data

- `sac_read_story_data(story_id)`
  - Named parameters: `secret` VARCHAR
  - Returns: Data used in story visualizations

---

## 🔷 Microsoft 365 & Dynamics 365

ERPL-OData provides native SQL access to Microsoft 365 services via the Microsoft Graph API, as well as Microsoft Dynamics 365 Business Central and Dataverse (CRM). Authentication uses standard OAuth2 client credentials.

### Authentication

Two secret types cover all Microsoft services:

**`TYPE microsoft_graph`** — for all Graph API functions (SharePoint, Teams, Excel, Planner, Outlook, Entra ID):
```sql
CREATE SECRET ms_graph (
  TYPE microsoft_graph,
  tenant_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  client_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  client_secret 'your-client-secret'
);
```

**`TYPE microsoft_entra`** — for Entra ID with explicit scope control (also used for Business Central):
```sql
CREATE SECRET ms_entra (
  TYPE microsoft_entra,
  tenant_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  client_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  client_secret 'your-client-secret',
  scope 'https://graph.microsoft.com/.default'   -- optional, defaults to Graph
);
```

**`TYPE business_central`** — for Business Central ERP:
```sql
CREATE SECRET ms_bc (
  TYPE business_central,
  tenant_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  environment 'production',                        -- or 'sandbox'
  client_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  client_secret 'your-client-secret'
);
```

**`TYPE dataverse`** — for Dynamics 365 CRM / Dataverse:
```sql
CREATE SECRET ms_crm (
  TYPE dataverse,
  tenant_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  environment_url 'https://myorg.crm.dynamics.com',
  client_id 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
  client_secret 'your-client-secret'
);
```

Required Azure App Registration permissions per service are listed in each section below.

---

### Entra ID (Users, Groups, Devices)

Required permissions: `User.Read.All`, `Group.Read.All`, `Device.Read.All` (Application, admin-consented).
`graph_signin_logs()` additionally requires `AuditLog.Read.All` and Azure AD Premium P1/P2.

```sql
-- All users in the directory
SELECT id, display_name, mail, job_title, department, account_enabled
FROM graph_users();

-- All security groups
SELECT id, display_name, mail_enabled, security_enabled
FROM graph_groups()
WHERE security_enabled = true;

-- Registered devices
SELECT id, display_name, operating_system, os_version, trust_type
FROM graph_devices();

-- Sign-in audit log (Azure AD Premium required)
SELECT user_display_name, app_display_name, ip_address, status, created_datetime
FROM graph_signin_logs()
WHERE status != 'Success'
ORDER BY created_datetime DESC;
```

Functions: `graph_users([secret])`, `graph_groups([secret])`, `graph_devices([secret])`, `graph_signin_logs([secret])`

---

### Planner (Tasks & Plans)

Required permissions: `Tasks.Read.All`, `Group.Read.All` (Application).

```sql
-- Plans for a Microsoft 365 group
SELECT id, title, owner_group_id, created_at
FROM graph_planner_plans('your-group-id');

-- Buckets in a plan
SELECT id, name, plan_id, order_hint
FROM graph_planner_buckets('your-plan-id');

-- Tasks in a plan
SELECT id, title, bucket_id, percent_complete, priority, due_at
FROM graph_planner_tasks('your-plan-id')
WHERE percent_complete < 100
ORDER BY due_at;
```

Functions: `graph_planner_plans(group_id, [secret])`, `graph_planner_buckets(plan_id, [secret])`, `graph_planner_tasks(plan_id, [secret])`

#### Creating tasks

Required permissions: `Tasks.ReadWrite.All` (Application).

`graph_planner_create_task` is a table function that calls the Graph API once per invocation and returns `(task_id, task_url)`. Use it with a **lateral join** to bulk-create tasks from any query result.

```sql
-- Create a single task
SELECT task_id, task_url
FROM graph_planner_create_task(
    plan_id          := 'your-plan-id',
    title            := 'Review Q2 budget',
    bucket_id        := 'Backlog',          -- name or GUID
    due_date         := '2024-07-31',
    assigned_to      := 'user-object-id',
    description      := 'Check the finance sheet before the board meeting.',
    priority         := 3,                  -- 1=urgent 3=important 5=medium 9=low
    percent_complete := 0,
    secret           := 'ms_graph'
);

-- Bulk-create tasks from a DuckDB table using a lateral join
SELECT t.title, p.task_id, p.task_url
FROM todos t,
     LATERAL graph_planner_create_task(
         plan_id   := 'your-plan-id',
         title     := t.title,
         bucket_id := t.bucket,
         due_date  := t.due_date,
         secret    := 'ms_graph'
     ) p;
```

`bucket_id` accepts either a bucket GUID or a human-readable bucket name (resolved automatically). `due_date` / `start_date` accept `'YYYY-MM-DD'` or full ISO 8601 datetimes.

Functions: `graph_planner_create_task(plan_id, title, [bucket_id, due_date, start_date, assigned_to, description, priority, percent_complete, secret])`

---

### SharePoint Lists

Required permissions: `Sites.Read.All` (Application).

Site and list arguments accept a **friendly name**, a **site URL**, or the raw GUID — the API resolves them automatically.

```sql
-- Discover accessible sites
SELECT id, display_name, web_url
FROM graph_show_sites();

-- Document library drives in a site (by name or ID)
SELECT id, name, drive_type, web_url
FROM graph_show_drives(site := 'Finance', secret := 'ms_graph');

-- Lists in a site (by name or ID)
SELECT id, name, display_name, description
FROM graph_show_lists(site := 'Finance', secret := 'ms_graph');

-- Schema of a list (site and list accept names, URLs, or GUIDs)
SELECT name, column_type, required
FROM graph_describe_list('Finance', 'Budget');

-- Items in a list — filter pushdown reduces server-side payload
SELECT * FROM graph_sharepoint_list_read('Finance', 'Budget', secret := 'ms_graph')
WHERE status = 'Active';

-- Works equally with site URLs and list display names
SELECT * FROM graph_sharepoint_list_read(
    'https://tenant.sharepoint.com/sites/Finance',
    'Project Tracker',
    secret := 'ms_graph'
);
```

Functions: `graph_show_sites([secret])`, `graph_show_drives([site_id], [secret, site])`, `graph_show_lists([site_id], [secret, site])`, `graph_describe_list(site_id_or_name, list_id_or_name, [secret])`, `graph_sharepoint_list_read(site_id_or_name, list_id_or_name, [secret])`

#### Writing list items

Required permissions: `Sites.ReadWrite.All` (Application).

`graph_sharepoint_create_item` is a **table function** returning the new `item_id`; use it in `SELECT` or with a `LATERAL` join. `graph_sharepoint_update_item` and `graph_sharepoint_delete_item` are **scalar functions** returning `BOOLEAN` (true on success) — note their `secret` is the **last positional argument**, not a named parameter.

```sql
-- Create a new item (fields as JSON object) — returns the new item_id
SELECT item_id
FROM graph_sharepoint_create_item(
    'Finance',
    'Budget',
    '{"Title": "New Entry", "Status": "Draft", "Amount": 1500}',
    secret := 'ms_graph'
);

-- Update an existing item by ID — scalar, returns true on success (secret is positional)
SELECT graph_sharepoint_update_item(
    'Finance', 'Budget', 'item-id-here',
    '{"Status": "Approved"}',
    'ms_graph'
) AS updated;

-- Delete an item by ID — scalar, returns true on success (secret is positional)
SELECT graph_sharepoint_delete_item('Finance', 'Budget', 'item-id-here', 'ms_graph') AS deleted;

-- Bulk-create items from a query result
SELECT src.title, p.item_id
FROM staging_items src,
     LATERAL graph_sharepoint_create_item(
         'Finance', 'Budget',
         json_object('Title', src.title, 'Amount', src.amount::VARCHAR),
         secret := 'ms_graph'
     ) p;
```

Functions: `graph_sharepoint_create_item(site, list, fields_json, [secret])`, `graph_sharepoint_update_item(site, list, item_id, fields_json, [secret])`, `graph_sharepoint_delete_item(site, list, item_id, [secret])`

---

### SharePoint Attach

Attach SharePoint lists from a site as a read-only DuckDB catalog. Tables are lazy — list data is fetched when a table is queried.

Required permissions: `Sites.Read.All`.

```sql
ATTACH 'Finance' AS finance_sp (TYPE sharepoint_lists, SECRET 'ms_graph');
```

After attaching, SharePoint lists are available as tables in the attached catalog:

```sql
SELECT * FROM finance_sp.main."Project Tracker";
SHOW TABLES IN finance_sp;
```

Storage extension: `ATTACH '<site name, URL, or site-id>' AS <catalog> (TYPE sharepoint_lists, SECRET '<secret>')`

---

### Excel (OneDrive / SharePoint)

Required permissions: `Files.ReadWrite.All` (Application). Note: the Microsoft Graph WAC (Web Application Companion) service requires write permission even for read-only access to Excel data.

File paths and drive locations accept either a **friendly name** (`site := 'Finance'`, `drive := 'Documents'`) or raw IDs (`drive_id := 'b!...'`).

```sql
-- Files accessible in OneDrive/SharePoint (by name or raw drive_id)
SELECT id, name, web_url, size, created_at, modified_at, mime_type, is_folder
FROM graph_show_files(site := 'Finance', drive := 'Documents', secret := 'ms_graph');

-- Worksheets in a workbook
SELECT name, id, position, visibility
FROM graph_excel_worksheets('Budget.xlsx',
  site   := 'Finance',
  drive  := 'Documents',
  secret := 'ms_graph'
);

-- Named tables in a workbook
SELECT name, id, show_headers, show_totals
FROM graph_excel_tables('Budget.xlsx',
  site   := 'Finance',
  drive  := 'Documents',
  secret := 'ms_graph'
);

-- Read table data
SELECT * FROM graph_excel_read('Budget.xlsx', 'SalesData',
  site   := 'Finance',
  drive  := 'Documents',
  secret := 'ms_graph'
);

-- Read a cell range
SELECT * FROM graph_excel_range('Budget.xlsx', 'Sheet1',
  site   := 'Finance',
  drive  := 'Documents',
  secret := 'ms_graph'
);
```

Functions: `graph_show_files([folder_path], [secret, drive_id, site, drive])`, `graph_excel_worksheets(file_path, [secret, drive_id, site, drive])`, `graph_excel_tables(file_path, [secret, drive_id, site, drive])`, `graph_excel_read(file_path, table_name, [secret, drive_id, site, drive])`, `graph_excel_range(file_path, sheet_name, [secret, drive_id, site, drive])`

#### Writing Excel data

Required permissions: `Files.ReadWrite.All` (Application).

**Append rows via ATTACH:**

Attach a workbook as an `excel_workbook` catalog and use standard `INSERT INTO` to append rows to a named Excel table.

```sql
ATTACH '/sites/Finance/Shared Documents/Budget.xlsx' AS budget
    (TYPE excel_workbook, SECRET 'ms_graph');

-- Tables in the workbook become DuckDB tables
SHOW TABLES IN budget;

-- Append rows
INSERT INTO budget.main."SalesData"
SELECT region, product, revenue::VARCHAR
FROM local_sales
WHERE quarter = 'Q2';
```

The `drive` option scopes the path to a specific SharePoint drive: `(TYPE excel_workbook, SECRET 'ms_graph', drive 'b!drive-id...')`.

**Delete rows by column value:**

```sql
-- Delete all rows where a column equals a given value (column by name or 0-based index)
SELECT rows_deleted
FROM graph_excel_delete_rows(
    'Budget.xlsx', 'SalesData',
    'Region', 'North',          -- column name (or a 0-based index like 0) and match value
    site   := 'Finance',
    secret := 'ms_graph'
);
```

Functions: `graph_excel_delete_rows(file_path, table_name, column, col_value, [secret, drive, site])` — `column` is a column name (resolved against the table header) or a 0-based index; `col_value` is always compared as a string.

Storage extension: `ATTACH '<file-path>' AS <catalog> (TYPE excel_workbook, SECRET '<secret>'[, drive '<drive-id>'])`

---

### Outlook (Calendar, Contacts, Mail)

Required permissions: `Calendars.Read`, `Contacts.Read`, `Mail.Read` — both **Delegated** and **Application** are supported.

- **Delegated** (`authorization_code` secret): use `/me/...` by omitting `user`
- **Application** (`client_credentials` secret): provide `user` to route to `/users/{id}/...`

The `user` parameter accepts a GUID, a `userPrincipalName` (e.g. `jr@contoso.com`), or an email address — all three forms are resolved correctly.

```sql
-- List calendars for a user (app-only) — GUID, UPN, or email all work
SELECT id, name, is_default_calendar
FROM graph_calendars(user := 'jr@contoso.com', secret := 'ms_graph');

-- All calendar events for the current user (delegated)
SELECT subject, start_time, end_time, organizer_name, location
FROM graph_calendar_events(secret := 'ms_graph')
ORDER BY start_time DESC;

-- Date-bounded events via calendarView (app-only, returns events in range)
SELECT subject, start_time, end_time, is_all_day
FROM graph_calendar_events(
    user       := 'user-guid-or-upn',
    start_date := '2024-01-01',
    end_date   := '2024-12-31',
    secret     := 'ms_graph'
);

-- Specific calendar by id
SELECT subject, start_time FROM graph_calendar_events(
    user        := 'user-guid-or-upn',
    calendar_id := 'calendar-guid',
    secret      := 'ms_graph'
);

-- Contacts
SELECT display_name, email, company_name, job_title
FROM graph_contacts(user := 'user-guid-or-upn', secret := 'ms_graph');

-- Discover mail folders
SELECT display_name, total_item_count, unread_item_count
FROM graph_outlook_mail_folders(user := 'user-guid-or-upn', secret := 'ms_graph');

-- Email messages — metadata only, no body content
SELECT subject, from_name, from_email, received_at, importance, is_read
FROM graph_outlook_emails(user := 'user-guid-or-upn', folder := 'inbox', secret := 'ms_graph')
ORDER BY received_at DESC;

-- folder accepts well-known names or any display name from graph_outlook_mail_folders()
-- well-known: inbox, sentitems, drafts, deleteditems, junkemail, outbox, archive
```

Functions: `graph_calendars([user, secret])`, `graph_calendar_events([user, calendar_id, start_date, end_date, secret])`, `graph_contacts([user, secret])`, `graph_outlook_mail_folders([user, secret])`, `graph_outlook_emails([user, folder, secret])`

`start_date` and `end_date` must be provided together; bare ISO dates (`'2024-01-01'`) are accepted alongside full datetimes.

---

### Teams (Channels & Messages)

Required permissions: `Team.ReadBasic.All`, `Channel.ReadBasic.All`, `ChannelMessage.Read.All`, `TeamMember.Read.All` (Application).

```sql
-- Teams the user belongs to
SELECT id, display_name, description
FROM graph_my_teams();

-- Channels in a team
SELECT id, display_name, membership_type
FROM graph_teams_channels('your-team-id');

-- Members of a team
SELECT id, display_name, role
FROM graph_teams_members('your-team-id');

-- Messages in a channel
SELECT id, created_datetime, from_name, body_content, importance
FROM graph_channel_messages('your-team-id', 'your-channel-id')
ORDER BY created_datetime DESC;
```

Functions: `graph_my_teams([user, secret])`, `graph_teams_channels(team_id, [secret])`, `graph_teams_members(team_id, [secret])`, `graph_channel_messages(team_id, channel_id, [secret])`

`user` accepts a GUID, UPN, or email. Omit it for delegated (`/me/joinedTeams`); provide it for app-only auth (`/users/{id}/joinedTeams`).

---

### Business Central (ERP)

Required permissions: Azure App Registration with `API.ReadWrite.All` and a corresponding Entra application record in Business Central with assigned permission sets.

```sql
-- Companies in the environment
SELECT id, display_name, business_profile_id
FROM bc_show_companies();

-- Available OData entity sets
SELECT entity_name, entity_url, description
FROM bc_show_entities();

-- Schema of an entity
SELECT property_name, property_type, is_key, nullable
FROM bc_describe('customers');

-- Query data with pushdown
SELECT number, display_name, email, balance_due
FROM bc_read('customers')
WHERE balance_due > 1000;
```

Functions: `bc_show_companies([secret])`, `bc_show_entities([secret])`, `bc_describe(entity, [secret])`, `bc_read(entity, [secret, company, expand])`

---

### Dataverse / Dynamics 365 CRM

Required permissions: `Dynamics CRM — user_impersonation` (Delegated) or service principal with Dataverse system user.

```sql
-- Available entities
SELECT logical_name, display_name, object_type_code
FROM crm_show_entities();

-- Schema of an entity
SELECT attribute_name, attribute_type, is_primary_key
FROM crm_describe('accounts');

-- Query CRM records
SELECT name, emailaddress1, telephone1, revenue
FROM crm_read('accounts')
WHERE statecode = 0  -- Active records only
ORDER BY revenue DESC;
```

Functions: `crm_show_entities([secret])`, `crm_describe(entity, [secret])`, `crm_read(entity, [secret])`

---

## 🔐 Telemetry

Telemetry is optional and can be disabled at any time:

```sql
SET erpl_telemetry_enabled = FALSE;
-- Optionally set a custom API key
SET erpl_telemetry_key = 'your-posthog-key';
```

Data collected: extension/DuckDB version, OS/arch, anonymized function usage. No sensitive content is collected.

---

## 🤝 Contributing

Issues and PRs are welcome! Please refer to CONTRIBUTING.md for guidance.

---

## 📄 License

This project is licensed under the Business Source License (BSL) 1.1. See [LICENSE](./LICENSE).

---

Build API-powered analytics with DuckDB + ERPL-OData. Query the web like it’s a table. 🚀

## Feedback

If `erpl_odata` misbehaves — an OData service it will not read, a Datasphere or Business
Central call that fails oddly — please
[open an issue](https://github.com/DataZooDE/erpl-odata/issues). Services differ by tenant,
version and auth setup in ways we cannot reproduce here, so a report with your setup is
the fastest path to a fix. Every error the extension raises ends with that link.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a small
banner says the same thing. It never prints when output is piped, in notebooks, or in
CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

