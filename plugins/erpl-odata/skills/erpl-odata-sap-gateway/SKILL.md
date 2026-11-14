---
name: erpl-odata-sap-gateway
description: >
  Reading SAP S/4HANA and ECC data through SAP Gateway OData services with
  erpl_odata: discovering services with odata_sap_show (v2 catalog and v4
  service groups), URL anatomy of /sap/opu/odata (v2) and /sap/opu/odata4 (v4
  RAP services), basic-auth secrets scoped to the SAP host, describing and
  attaching a service, SAP-specific quirks (draft entity sets, Edm.DateTime,
  sap-client, mandatory filters), activating services (/IWFND/MAINT_SERVICE,
  /IWFND/V4_ADMIN, SICF), and the ABAP trial (A4H) sandbox. Use when the user
  wants data out of an SAP system via OData, cannot find or reach a Gateway
  service, or gets 403/404 from /sap/opu. For delta extraction use
  erpl-odata-sap-odp-replication instead.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — SAP Gateway OData Services

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v2.0.0 / v1.5.6

Gateway services are ordinary OData v2/v4 services, so everything in
`erpl-odata-odata-and-rest` applies. This skill adds the SAP-specific discovery, URLs,
setup on the ABAP side and quirks.

## Critical gotchas

1. **`odata_sap_show` takes the host root, not a service URL.** Pass
   `https://sap-host:44300`. The function itself appends the v2 catalog
   (`/sap/opu/odata/iwfnd/catalogservice;v=2/ServiceCollection`) and the v4 service-group
   catalog (`/sap/opu/odata4/iwfnd/config/default/iwfnd/catalog/0002/ServiceGroups`).
2. **Scope the secret to exactly the host and port in your URLs.** `http://host:50000/`
   and `https://host:44300/` are different scopes. A non-matching scope means an
   anonymous call, and SAP answers 401.
3. **A service in the catalog is not necessarily active.** A v2 service must be added in
   `/IWFND/MAINT_SERVICE` and a v4 service group published in `/IWFND/V4_ADMIN`, and the
   ICF nodes under `/sap/opu` must be active in SICF. An inactive one answers 403
   ("Service cannot be reached") or 404.
4. **RAP (v4) services expose draft tables.** Entity sets such as `I_DraftAdministrativeData`
   are noise for analytics and some refuse to be read. Skip them with
   `IGNORE 'I_Draft*'` on `ATTACH` or `ignore := ['I_Draft*']` on `odata_attach`.
5. **Plain Gateway reads are snapshots.** Re-reading a large entity set every night
   transfers everything again. If the source is an extractor or a CDS view with
   delta, use ODP (`erpl-odata-sap-odp-replication`).
6. **Writes are out of scope.** Gateway writes need a CSRF token plus a session cookie
   across two requests. Use the erpl RFC extension (BAPIs) or the ABAP side for
   write-backs.

## Service URL anatomy

| Kind | URL pattern | Example |
|---|---|---|
| v2 (SEGW, CDS `@OData.publish`, ODP) | `/sap/opu/odata/<namespace>/<SERVICE_NAME>/<EntitySet>` | `/sap/opu/odata/sap/API_BUSINESS_PARTNER/A_BusinessPartner` |
| v4 (RAP service binding) | `/sap/opu/odata4/<group path>/srvd/<namespace>/<service definition>/<version>/<EntitySet>` | `/sap/opu/odata4/dmo/ui_travel_d_d_o4/srvd/dmo/ui_travel_d_d/0001/Travel` |

`odata_sap_show` returns `service_id`, `description`, `version` and `service_url` for both
kinds. `service_url` is the service root; append an entity set name for `odata_read`.

## Discover

```sql
CREATE SECRET sap (
    TYPE http_basic,
    USERNAME 'DEVELOPER',
    PASSWORD 'secret',
    SCOPE 'http://localhost:50000/'
);

-- Every v2 service and v4 service group the user may see
SELECT service_id, description, version, service_url
FROM odata_sap_show('http://localhost:50000')
WHERE service_id ILIKE '%TRAVEL%';
```

`odata_sap_show` also accepts `secret := 'name'` if you do not want scope-based lookup.

## Describe and read

```sql
-- Entity sets, properties, keys and navigation properties of the service
SELECT * FROM odata_describe(
    'http://localhost:50000/sap/opu/odata4/dmo/ui_travel_d_d_o4/srvd/dmo/ui_travel_d_d/0001/Travel');

-- Read one entity set with pushdown
SELECT TravelID, AgencyID, CustomerID, BeginDate, TotalPrice, CurrencyCode
FROM odata_read('http://localhost:50000/sap/opu/odata4/dmo/ui_travel_d_d_o4/srvd/dmo/ui_travel_d_d/0001/Travel')
WHERE BeginDate >= DATE '2024-01-01'
LIMIT 100;

-- Or attach the whole service as a catalog
ATTACH 'http://localhost:50000/sap/opu/odata4/dmo/ui_travel_d_d_o4/srvd/dmo/ui_travel_d_d/0001/'
    AS travel (TYPE odata, IGNORE 'I_Draft*');
SHOW TABLES FROM travel;
SELECT count(*) FROM travel.Airline;
```

## SAP-specific quirks

| Situation | What to do |
|---|---|
| Client other than the logon default | Append `?sap-client=100` to the service URL. Check in the trace (`erpl_trace_level = 'DEBUG'`) that it reaches the request |
| `Edm.DateTime` values like `/Date(1704067200000)/` | Converted to `TIMESTAMP` automatically, including the `+HHMM` offset form |
| `Edm.Decimal` amounts | Mapped to `DECIMAL` with the declared precision and scale. Keep the currency or unit column next to the amount; SAP stores them separately |
| Entity set requires a filter (HTTP 400 "filter required" or an empty result) | Add a `WHERE` on the required key; it pushes down as `$filter` |
| Parameterised CDS views (v2) | Address the parameter set directly in the URL, for example `.../ZC_SALES(P_KeyDate=datetime'2024-12-31T00:00:00')/Set` |
| Language-dependent texts | Pass `?sap-language=EN` in the URL, or use the text entity set the service exposes |
| Very large entity set | Push filters down to cut the volume. For repeated loads switch to ODP delta |
| Values that do not convert | Read with `strict_typing := true` to find them, instead of getting silent `NULL`s |

## Activating a service (ABAP side)

1. **v2:** transaction `/IWFND/MAINT_SERVICE` → *Add Service* → system alias `LOCAL` →
   find the technical service name → *Add Selected Services*.
2. **v4:** `/IWFND/V4_ADMIN` → *Publish Service Groups*. For a RAP service binding,
   *Publish* in ADT does the same.
3. **ICF:** transaction `SICF`; the nodes `/sap/opu/odata`, `/sap/opu/odata4` and
   `/sap/public/opu` must be active. `scripts/activate_icf_services.abap` in this repository
   activates them recursively.
4. **Authorisation:** the user needs `S_SERVICE` for the service, and the business
   authorisations of the data underneath. A 403 after activation is usually this.

To create CDS views, service definitions and bindings without the SAP GUI, use the
erpl-adt CLI and its MCP server (`erpl-adt object create`, `source write`, `activate`).

## ABAP trial sandbox (A4H)

The SAP ABAP Platform trial container listens on `http://localhost:50000`, logon user
`DEVELOPER`, client 001. It ships the `/DMO/` flight demo services, which are good test
targets:

```sql
SELECT service_id, service_url
FROM odata_sap_show('http://localhost:50000')
WHERE service_url LIKE '%/dmo/%';
```

## Canonical workflow: from host to table

```sql
-- 1. Credentials and, for a private CA, trust (see erpl-odata-setup)
CREATE SECRET sap (TYPE http_basic, USERNAME 'u', PASSWORD 'p', SCOPE 'https://s4.example.com:44300/');
SET erpl_ca_cert_file = '/etc/erpl/erpl-ca-bundle.pem';

-- 2. Find the service
SELECT service_id, service_url
FROM odata_sap_show('https://s4.example.com:44300')
WHERE service_id ILIKE '%BUSINESS_PARTNER%';

-- 3. Inspect its entity sets
ATTACH 'https://s4.example.com:44300/sap/opu/odata/sap/API_BUSINESS_PARTNER/' AS bp (TYPE odata);
SHOW TABLES FROM bp;

-- 4. Land what you need, with pushdown and strict typing
CREATE TABLE business_partners AS
SELECT BusinessPartner, BusinessPartnerCategory, BusinessPartnerFullName, CreationDate
FROM odata_read('https://s4.example.com:44300/sap/opu/odata/sap/API_BUSINESS_PARTNER/A_BusinessPartner',
                strict_typing := true)
WHERE BusinessPartnerCategory = '2';
```

See also: `erpl-odata-odata-and-rest` (`odata_read` parameters, pushdown, expand),
`erpl-odata-sap-odp-replication` (delta loads from extractors and CDS views),
`erpl-odata-setup` (secrets, TLS), `erpl-odata-production` (scheduling and the error
playbook).

Reference docs:
- `README.md` (OData v2/v4)
- `docs/TLS.md`
- `scripts/activate_icf_services.abap`
