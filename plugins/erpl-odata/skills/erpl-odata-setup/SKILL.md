---
name: erpl-odata-setup
description: >
  Getting started with the erpl_odata DuckDB extension: installing and loading
  it, choosing the right function family for a source system (HTTP, OData,
  SAP Gateway, SAP ODP, Datasphere, SAC, Microsoft Graph, Business Central,
  Dataverse, Delta Sharing), creating the matching DuckDB secret
  (http_basic, http_bearer, microsoft_graph, microsoft_entra,
  business_central, dataverse, datasphere, sac), how a secret is picked
  up, TLS trust for on-premise systems (erpl_ca_cert_file), and telemetry
  opt-out. Use when the user installs erpl_odata, asks "how do I connect to
  ...", needs to authenticate, hits a certificate error, or is unsure which
  function to start with.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — Setup, Connecting and Authentication

**Extension:** `erpl_odata` v2026.10.24 (formerly `erpl_web`) | **DuckDB:** v2.0.0 / v1.5.6

This skill gets a user from zero to a first authenticated query. The per-source
skills listed under "Where to go next" take over from there.

## Critical gotchas

1. **The extension is `erpl_odata`, not `erpl_web`.** On the community repository
   `erpl_web` is an alias, but from `http://get.erpl.io`, `LOAD erpl_web` fails. Write
   `INSTALL erpl_odata FROM community; LOAD erpl_odata;` in new scripts.
2. **Which secret is used depends on the function family.**
   - `http_*`, `odata_*`, `odata_sap_show` and `odp_odata_*` look up an `http_basic` or
     `http_bearer` secret **by URL `SCOPE`** (longest prefix wins). They can also be told
     the secret explicitly with `secret := 'name'` where the function accepts it.
   - Graph, Business Central, Dataverse, Datasphere and SAC functions use their **own
     secret type**, chosen with `secret := 'name'` or, if omitted, the first secret of
     that type.
3. **`auth :=` on an `http_*` call overrides every secret.** That is handy for one-off
   tests and a trap in production: a stray `auth` silently wins.
4. **Secrets are in-memory by default.** `CREATE SECRET` lasts for the session.
   `CREATE PERSISTENT SECRET` writes it to `~/.duckdb/stored_secrets/` **unencrypted**.
   Treat that directory like a password file, or recreate secrets from environment
   variables at session start (see `erpl-odata-production`).
5. **TLS verification is always on.** For an on-premise SAP host with a private CA, trust
   the CA with `erpl_ca_cert_file`. Setting it **replaces** the platform trust store, so
   bundle the public CAs too if the same session talks to cloud services.
6. **Interactive OAuth opens a browser.** `TYPE microsoft_graph, PROVIDER authorization_code`
   and `TYPE datasphere, PROVIDER oauth2` without `grant_type 'client_credentials'` start
   a local callback server and a browser login. They need a desktop session and will hang
   in CI, cron or a container. Use the client-credentials variants there.

## Install and load

```sql
INSTALL erpl_odata FROM community;
LOAD erpl_odata;
```

Nightly or pre-release builds come from DataZoo's own repository. DuckDB needs to be
started with `-unsigned` (or `allow_unsigned_extensions`) for that:

```sql
INSTALL erpl_odata FROM 'http://get.erpl.io';
LOAD erpl_odata;
```

Check what is registered. Every function carries a description and an example:

```sql
SELECT function_name, function_type, description
FROM duckdb_functions()
WHERE function_name LIKE 'odata_%' OR function_name LIKE 'odp_odata_%'
ORDER BY function_name;
```

## Choose the function family

| Source system | Start with | Secret type | Skill |
|---|---|---|---|
| Any REST/JSON API | `http_get`, `http_post`, … | `http_basic` / `http_bearer` / `auth :=` | `erpl-odata-odata-and-rest` |
| Any OData v2/v4 service | `odata_read`, `ATTACH … (TYPE odata)` | `http_basic` / `http_bearer` | `erpl-odata-odata-and-rest` |
| SAP S/4HANA / ECC Gateway | `odata_sap_show`, then `odata_read` | `http_basic` (scoped to the host) | `erpl-odata-sap-gateway` |
| SAP extractors / CDS via ODP (full + delta) | `odp_odata_show`, `odp_odata_read` | `http_basic` | `erpl-odata-sap-odp-replication` |
| SAP Datasphere | `datasphere_show_spaces` | `datasphere` | `erpl-odata-sap-datasphere-sac` |
| SAP Analytics Cloud | `sac_read_planning_data` | `sac` | `erpl-odata-sap-datasphere-sac` |
| Excel, SharePoint lists, Planner, Outlook, Teams, Entra ID | `graph_*` | `microsoft_graph` | `erpl-odata-microsoft-365` |
| Dynamics 365 Business Central | `bc_show_companies`, `bc_read` | `business_central` | `erpl-odata-dynamics-and-delta-sharing` |
| Dataverse / Dynamics 365 CRM | `crm_show_entities`, `crm_read` | `dataverse` | `erpl-odata-dynamics-and-delta-sharing` |
| Delta Sharing (Databricks, SAP BDC, …) | `delta_share_show_shares`, `delta_share_scan` | profile file, no secret | `erpl-odata-dynamics-and-delta-sharing` |

## Secret reference

### HTTP, OData, SAP Gateway, ODP

```sql
-- Basic auth, scoped to one host: every URL under the scope uses it
CREATE SECRET sap_dev (
    TYPE http_basic,
    USERNAME 'DEVELOPER',
    PASSWORD 'secret',
    SCOPE 'https://sap-dev.example.com:44300/'
);

-- Bearer token
CREATE SECRET api_token (
    TYPE http_bearer,
    TOKEN 'eyJhbGciOi...',
    SCOPE 'https://api.example.com/'
);
```

Scope the secret to the scheme, host and port **exactly as they appear in the URLs you
query**. `http://host:50000/` and `https://host:44300/` are different scopes.

### Microsoft Graph (Excel, SharePoint, Planner, Outlook, Teams, Entra)

| Provider | Use for | Parameters |
|---|---|---|
| `client_credentials` (default) | App-only access, unattended jobs | `tenant_id`, `client_id`, `client_secret`, optional `scope` (defaults to `https://graph.microsoft.com/.default`) |
| `authorization_code` | Delegated access as the signed-in user (`/me/...`), opens a browser | `tenant_id`, `client_id`, optional `client_secret`, `scope`, `redirect_uri` (default `http://localhost:8080/callback`) |
| `config` | A token you obtained elsewhere | `access_token`, optional `refresh_token`, `expires_at`, plus the app parameters |

```sql
CREATE SECRET ms_graph (
    TYPE microsoft_graph,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_SECRET 'app-secret'
);
```

`TYPE microsoft_entra` takes the same client-credentials parameters with an explicit
`scope`, for when a token must be minted for a resource other than Graph.

### Business Central and Dataverse

```sql
CREATE SECRET bc (
    TYPE business_central,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_SECRET 'app-secret',
    ENVIRONMENT 'production'          -- or 'sandbox', or your environment name
);

CREATE SECRET crm (
    TYPE dataverse,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_SECRET 'app-secret',
    ENVIRONMENT_URL 'https://myorg.crm4.dynamics.com'
);
```

Both also have a `config` provider that takes `access_token` / `refresh_token` / `expires_at`.

### SAP Datasphere

| Provider | Parameters |
|---|---|
| `oauth2` (default) | `tenant_name`, `data_center`, `client_id`, `client_secret`, `scope`, `redirect_uri` (default `http://localhost:65000`), `grant_type` (`authorization_code` default, or `client_credentials`), `token_url` |
| `config` | `config_file`, an INI-style `key=value` file |
| `file` | `filepath`, a credentials JSON file |

```sql
-- Unattended: client_credentials needs token_url, or tenant_name + data_center
CREATE SECRET dsp (
    TYPE datasphere,
    PROVIDER oauth2,
    TENANT_NAME 'my-tenant',
    DATA_CENTER 'eu10',
    CLIENT_ID 'sb-...',
    CLIENT_SECRET '...',
    GRANT_TYPE 'client_credentials'
);
```

With `authorization_code`, the first use opens the browser and the refresh token is written
back into the secret, so a `PERSISTENT` secret survives restarts without a new login.

### SAP Analytics Cloud

```sql
CREATE SECRET my_sac (
    TYPE sac,
    PROVIDER access_token,
    TENANT_NAME 'my-tenant',
    REGION 'eu10',        -- eu10, us10, ap10, ca10, jp10, au10, br10, ch10
    ACCESS_TOKEN '...'
);
```

The SAC `config` provider cannot fetch a token itself yet. Obtain the token outside DuckDB
and use `access_token`.

### Inspect secrets

```sql
SELECT name, type, provider, scope, persistent
FROM duckdb_secrets();
```

`duckdb_secrets()` redacts the sensitive values. Use `which_secret(url, type)` to see which
secret a given URL resolves to:

```sql
SELECT * FROM which_secret('https://sap-dev.example.com:44300/sap/opu/odata/sap/', 'http_basic');
```

## TLS for on-premise systems

```sql
-- Preferred: trust the CA that signed the server certificate
SET erpl_ca_cert_file = '/etc/erpl/corporate-ca.pem';
```

If the session also calls public services, build a combined bundle first, because the
setting replaces the platform store:

```bash
cat /etc/ssl/certs/ca-certificates.crt corporate-ca.pem > /etc/erpl/erpl-ca-bundle.pem
```

Last resort, accepted only with this exact literal. Every request then logs a WARN line
naming the insecure host:

```sql
SET erpl_unsafe_disable_server_cert_verification = 'I_UNDERSTAND_THIS_IS_INSECURE';
SET erpl_unsafe_disable_server_cert_verification = '';   -- back to verifying
```

On Linux the extension looks for the system bundle at `$SSL_CERT_FILE` and the usual
distribution paths. If yours lives elsewhere, set `SSL_CERT_FILE` before starting DuckDB.

## Telemetry

The extension sends anonymous usage events (extension and DuckDB version, OS, function
names; never data, URLs or credentials). Opt out per session, or for every process with an
environment variable:

```sql
SET erpl_telemetry_enabled = false;
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1
```

## Canonical first session

```sql
LOAD erpl_odata;

-- 1. A public OData service needs no secret: prove the extension works
SELECT CustomerID, CompanyName
FROM odata_read('https://services.odata.org/V2/Northwind/Northwind.svc/Customers')
LIMIT 3;

-- 2. Add credentials for your own system
CREATE SECRET sap_dev (
    TYPE http_basic,
    USERNAME 'DEVELOPER',
    PASSWORD 'secret',
    SCOPE 'http://localhost:50000/'
);

-- 3. Discover what the system offers, then hand over to the source skill
SELECT service_id, description, version, service_url
FROM odata_sap_show('http://localhost:50000')
LIMIT 20;
```

If a step fails, turn tracing on (`SET erpl_trace_enabled = true; SET erpl_trace_level = 'DEBUG';`)
and see the troubleshooting playbook in `erpl-odata-production`.

## Where to go next

- `erpl-odata-odata-and-rest`: generic OData and REST APIs
- `erpl-odata-sap-gateway`: SAP Gateway services
- `erpl-odata-sap-odp-replication`: SAP ODP full and delta loads
- `erpl-odata-sap-datasphere-sac`: SAP Datasphere and SAP Analytics Cloud
- `erpl-odata-microsoft-365`: Excel, SharePoint lists, Planner, Outlook, Teams, Entra ID
- `erpl-odata-dynamics-and-delta-sharing`: Business Central, Dataverse, Delta Sharing
- `erpl-odata-production`: unattended jobs, scheduling, tracing and troubleshooting

Reference docs:
- `README.md` (Install, HTTP authentication precedence, Microsoft 365 authentication)
- `docs/TLS.md`
- `TELEMETRY.md`
