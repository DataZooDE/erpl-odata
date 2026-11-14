# ERPL-OData troubleshooting playbook

Start every investigation the same way:

```sql
SET erpl_trace_enabled = true;
SET erpl_trace_level = 'DEBUG';      -- TRACE for full request/response detail
SET erpl_trace_output = 'console';   -- or 'file' / 'both' with erpl_trace_file_path
```

The trace shows each request URL (with the pushed-down `$filter`, `$select`, `$top`), the
response status, paging, retries and which credential source was used. Secrets are
redacted.

## Authentication

| Symptom | Cause | Fix |
|---|---|---|
| 401 from an OData / SAP / HTTP URL | No secret matched the URL, so the request went out anonymously | `SELECT * FROM which_secret('<url>', 'http_basic')`. The `SCOPE` must be a prefix of the URL, including scheme and port |
| 401 although the secret matches | Wrong password, locked user, or SAP needs a different client | Test the same URL with `curl -u`; add `?sap-client=NNN` |
| `auth :=` used, secret ignored | Function `auth` always wins over secrets | Remove `auth :=` |
| *"No Microsoft Graph secret found"* | No `microsoft_graph` secret in the session | `CREATE SECRET ... (TYPE microsoft_graph, ...)` or pass `secret :=` |
| 403 from Graph | Missing application permission or admin consent | Add the permission listed in `erpl-odata-microsoft-365` and grant consent |
| 401/403 from Business Central | No *Microsoft Entra Applications* record in BC, or no permission set | Create it in BC and assign permission sets |
| Job hangs at the first call | Interactive OAuth (`authorization_code`, Datasphere default) waiting for a browser | Use a `client_credentials` secret |
| *"... points at '<host>'. This service's OAuth token audience is fixed ..."* | Business Central / Datasphere pointed at a non-standard host | Fix the environment / tenant, or for a deliberate proxy `SET erpl_unsafe_allow_custom_service_urls = 'I_UNDERSTAND_THIS_SENDS_TOKENS_ELSEWHERE'` |
| SAC 401 after some time | SAC access tokens are short-lived | Refresh the token and recreate the secret |

## TLS

| Symptom | Fix |
|---|---|
| Certificate verify failed against an on-premise host | `SET erpl_ca_cert_file = '/path/ca.pem'` with the issuing CA |
| On-premise works, but now Graph / SAP cloud fails | `erpl_ca_cert_file` replaces the system store: concatenate the system bundle and your CA into one file |
| Every HTTPS call fails on a minimal Linux image | No CA bundle installed: install `ca-certificates`, or set `SSL_CERT_FILE` |
| *"... only accepts I_UNDERSTAND_THIS_IS_INSECURE"* | `erpl_unsafe_disable_server_cert_verification` takes only that literal; prefer the CA file |

## SAP Gateway

| Symptom | Cause | Fix |
|---|---|---|
| 403 *"Service cannot be reached"* | Service not activated in the Gateway hub | `/IWFND/MAINT_SERVICE` (v2) or `/IWFND/V4_ADMIN` (v4) |
| 404 for anything under `/sap/opu` | ICF node inactive | `SICF`, activate `/sap/opu/odata`, `/sap/opu/odata4`; see `scripts/activate_icf_services.abap` |
| 403 after activation | Missing `S_SERVICE` or business authorisations | Ask Basis to check transaction `SU53` for the user |
| 400 on an entity set without a filter | The service requires a filter or parameters | Add the required `WHERE`, or the parameter segment in the URL |
| `odata_sap_show` returns nothing | Passed a service URL instead of the host root | Pass `https://host:port` only |
| Reading a v4 service fails on draft entities | RAP draft tables | `IGNORE 'I_Draft*'` on `ATTACH` |

## SAP ODP

| Symptom | Cause | Fix |
|---|---|---|
| *"ODP delta state requires a persistent DuckDB database"* | Session is `:memory:` | Open a database file, or `ATTACH 'state.db' AS s; USE s;` |
| *"Could not open data access via extraction API RODPS_REPL_ODP_OPEN"* (RSODP_ODATA/013) | Two open extractions on one subscription: two reads in one statement, or overlapping runs | One read per statement; serialise runs with `flock` |
| Every run is a full load; warning *"Change tracking was NOT established"* | SAP did not confirm `odata.track-changes` | Check `change_tracking` in `odp_odata_show`, `RODPS_REPL_ODP_GET_DETAIL`, and the CDS annotation (`changeDataCapture.automatic`) |
| A new initial load after a config change | Subscription id includes URL, entity set and secret name | Keep the secret name stable; clean up the orphaned subscription in ODQMON |
| Rows duplicated or deleted rows still present | Delta applied with plain `INSERT` | Use the DELETE + INSERT merge with `ODQ_CHANGEMODE` |
| Error after long waiting on a 202 | The extraction did not finish within the polling budget | Run the initial load off-peak or restrict it on the SAP side |
| URL rejected as not an ODP URL | Entity set does not match `EntityOf* / FactsOf* / AttrOf*` | Use the entity set name from `odp_odata_show` |
| *"import_delta_token contains characters that are not valid"* | Token copied with quotes or whitespace | Paste the token exactly as `odp_odata_list_subscriptions()` shows it |

## Data and types

| Symptom | Cause | Fix |
|---|---|---|
| Unexpected `NULL`s plus a warning naming a column | Values that do not convert to the EDM type | `strict_typing := true` to fail and see the value; fix the source or read the column as text via `http_get` |
| Query slow, all rows transferred | A predicate did not push down (`LIKE`, functions on columns), so `LIMIT` cannot become `$top` either | Rewrite to plain comparisons; check the URL in the trace |
| Paging stops early or loops | Service paging bug or 10,000-page ceiling | Raise `max_page_size`; filter into slices (by date or key range) |
| SharePoint list read slow | No filter pushdown for lists | Land the list once, query locally |
| Umlauts garbled | Service declares a wrong charset | The reader converts ISO-8859-1/-15 and Windows-1252 to UTF-8 based on `Content-Type`; report the header if it is wrong |

## Still stuck

Collect a `TRACE`-level trace of the failing statement, the DuckDB and extension versions
(`SELECT extension_name, extension_version FROM duckdb_extensions() WHERE loaded`), and open
an issue at https://github.com/DataZooDE/erpl-odata/issues. Every error raised by the
extension ends with that link.
