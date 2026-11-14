---
name: erpl-odata-odata-and-rest
description: >
  Querying any OData v2/v4 service and any REST/JSON API from DuckDB SQL with
  erpl_odata: odata_read (top, skip, expand, count, max_page_size,
  strict_typing), automatic v2/v4 detection, what WHERE / SELECT / LIMIT push
  down as $filter / $select / $top, odata_describe, odata_attach and
  ATTACH ... (TYPE odata), plus http_get / http_head / http_post / http_put /
  http_patch / http_delete with headers, auth, timeouts, JSON bodies and
  unpacking JSON responses. Use when the user wants to explore or extract an
  OData service that is not SAP-specific, call a REST endpoint, page through
  an API, or turn a JSON response into a table.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — OData v2/v4 and REST APIs

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v1.4.5 LTS / v1.5.6 / v2.0

For SAP Gateway specifics (service catalog, `/sap/opu/odata`, ICF activation) see
`erpl-odata-sap-gateway`. For credentials see `erpl-odata-setup`.

## Critical gotchas

1. **`odata_read` has no `secret` parameter.** It authenticates with the `http_basic` /
   `http_bearer` secret whose `SCOPE` is the longest prefix of the URL. No matching secret
   means an anonymous request, which usually shows up as a 401.
2. **Point `odata_read` at an entity set, `ATTACH` at a service root.**
   `.../Northwind.svc/Customers` is right for `odata_read`; `.../Northwind.svc` is right for
   `ATTACH` and `odata_attach`.
3. **`LIMIT` only becomes `$top` when every filter was pushed down.** If any predicate has
   to be evaluated locally, the reader fetches every page and DuckDB filters, because a
   server-side row limit over unfiltered data would return too few rows. Expect a full
   scan when you use `LIKE`, functions on columns, or casts in `WHERE`.
4. **Unconvertible values become `NULL` by default.** The scan prints one warning per
   column naming the first bad value. Use `strict_typing = true` when a silent `NULL`
   would corrupt downstream results.
5. **`ATTACH 'https://...' (TYPE odata)` needs httpfs loaded.** DuckDB core sees an https
   path and insists on httpfs before handing it to the OData catalog. With autoloading on
   (the default) this happens by itself; air-gapped or `autoload_known_extensions = false`
   setups need `INSTALL httpfs; LOAD httpfs;` first, or the attach fails with *"requires
   extension 'httpfs' to be loaded"*. `odata_read` does not need httpfs.
6. **`ATTACH ... (TYPE odata)` is read-only.** It exposes every entity set as a table. There
   is no `INSERT` / `UPDATE` / `DELETE` against OData; use `http_post` / `http_patch` for
   writes.
7. **Table-function arguments must be constants.** No `LATERAL http_get(t.url)`, no
   recursive CTE feeding URLs back in; see "Paging through an API" for the pattern that works.
8. **`http_*` functions are table functions** returning one row:
   `method, status, url, headers, content_type, content`. A 4xx/5xx is a row with that
   `status`, not an error, so check `status` yourself.

## Reading an entity set: `odata_read`

```sql
odata_read(url VARCHAR,
           top := UBIGINT, skip := UBIGINT, expand := VARCHAR, count := BOOLEAN,
           max_page_size := UBIGINT, strict_typing := BOOLEAN) → TABLE
```

| Parameter | Effect |
|---|---|
| `top` / `skip` | Fixed `$top` / `$skip`, independent of `LIMIT` |
| `expand` | `$expand`. Navigation properties become nested STRUCT / LIST columns. Accepts nested paths such as `'Orders($expand=Order_Details)'` (v4) or `'Orders/Order_Details'` (v2) |
| `count` | Requests the inline count (`$inlinecount` / `$count`) so progress can be reported |
| `max_page_size` | Sends `Prefer: odata.maxpagesize=N`. Only a preference; the service may cap it. Raise it for very large extractions that hit server-side paging limits |
| `strict_typing` | Fail on the first value that cannot be converted to its column type |

```sql
-- v2 and v4 are detected automatically
SELECT CustomerID, CompanyName, Country
FROM odata_read('https://services.odata.org/V2/Northwind/Northwind.svc/Customers')
WHERE Country = 'Germany';

SELECT UserName, FirstName, LastName
FROM odata_read('https://services.odata.org/TripPinRESTierService/People')
WHERE Gender = 'Female';
```

### What pushes down

| SQL | Becomes |
|---|---|
| Selected columns | `$select` |
| `=`, `<>`, `<`, `<=`, `>`, `>=` against constants | `$filter` with `eq`, `ne`, `lt`, `le`, `gt`, `ge` |
| `IS NULL` / `IS NOT NULL` | `eq null` / `ne null` |
| `AND` / `OR` of pushable predicates | `and` / `or` |
| `IN (...)` | a parenthesised `or` chain (OData v2/v4 have no `in`) |
| `LIMIT n` with no residual filter | `$top=n` |

Anything else (`LIKE`, `lower(col) = ...`, arithmetic on columns) is evaluated by DuckDB
after the rows arrive. To see the exact request URLs:

```sql
SET erpl_trace_enabled = true;
SET erpl_trace_level = 'DEBUG';
SELECT OrderID FROM odata_read('https://services.odata.org/V2/Northwind/Northwind.svc/Orders')
WHERE OrderDate >= '1998-01-01' AND ShipCountry IN ('Germany', 'France');
```

### Expanding navigation properties

```sql
SELECT CustomerID, CompanyName, len(Orders) AS order_count
FROM odata_read('https://services.odata.org/V2/Northwind/Northwind.svc/Customers',
                expand := 'Orders')
WHERE Country = 'Germany';

-- Flatten the nested orders
SELECT c.CustomerID, o.OrderID, o.OrderDate
FROM odata_read('https://services.odata.org/V2/Northwind/Northwind.svc/Customers',
                expand := 'Orders') c,
     UNNEST(c.Orders) AS t(o);
```

Nested complex types are STRUCTs too. Address their fields with dot notation, and quote
names that clash with keywords:

```sql
SELECT UserName, AddressInfo[1].City."Name" AS city
FROM odata_read('https://services.odata.org/TripPinRESTierService/People');
```

## Discovering a service

```sql
-- Properties (DuckDB and EDM types, keys, nullability), navigation properties and
-- entity sets of the service behind one entity set URL. Accepts secret := 'name'.
SELECT * FROM odata_describe('https://services.odata.org/V2/Northwind/Northwind.svc/Customers');
```

### Attach a whole service

```sql
ATTACH 'https://services.odata.org/V2/Northwind/Northwind.svc' AS northwind (TYPE odata);
SHOW TABLES FROM northwind;
SELECT CustomerID, CompanyName FROM northwind.Customers WHERE Country = 'Germany' LIMIT 5;
DETACH northwind;
```

`IGNORE 'pattern'` skips entity sets whose names match (for example SAP draft tables):

```sql
ATTACH 'https://host/sap/opu/odata4/sap/zsrv/srvd/sap/zsrv/0001/' AS srv (TYPE odata, IGNORE 'I_Draft*');
```

`odata_attach(url)` is the older, function-style alternative: it creates one **view per
entity set in the current schema**. `overwrite := true` replaces existing views and
`ignore := ['pattern', ...]` skips entity sets.

```sql
CALL odata_attach('https://services.odata.org/V2/Northwind/Northwind.svc', overwrite := true);
SELECT count(*) FROM Customers;
```

Prefer `ATTACH`: it keeps the service in its own catalog, so table names cannot collide
with local ones.

## REST APIs: `http_*`

```sql
http_get(url, headers := MAP, accept := VARCHAR, auth := VARCHAR, auth_type := VARCHAR,
         timeout := BIGINT) → TABLE(method, status, url, headers, content_type, content)
http_head(url, ...)                                  -- same named parameters
http_post(url, body JSON, ...)                      -- or http_post(url, content VARCHAR, content_type VARCHAR, ...)
http_put / http_patch / http_delete                 -- same two forms as http_post
```

| Parameter | Meaning |
|---|---|
| `headers` | `MAP(VARCHAR, VARCHAR)`, for example `{'X-Api-Key': 'k', 'Accept-Language': 'de'}` |
| `accept` | `Accept` header |
| `content_type` | Request body type |
| `auth` | `'user:pass'` for BASIC (default) or the raw token for BEARER. Overrides any secret |
| `auth_type` | `'BASIC'` or `'BEARER'` |
| `timeout` | Milliseconds |

```sql
-- GET and pick fields out of the JSON body
SELECT status, content::JSON->>'origin' AS caller_ip
FROM http_get('https://httpbin.org/get');

-- POST a JSON body built in SQL
SELECT status, content::JSON->'json' AS echoed
FROM http_post('https://httpbin.org/post', {'name': 'Alice', 'age': 30}::JSON);

-- Form-encoded body
SELECT status FROM http_post('https://httpbin.org/post', 'a=1&b=2', 'application/x-www-form-urlencoded');

-- Bearer token for a single call
SELECT status, content
FROM http_get('https://api.example.com/v1/items', auth := 'eyJhbGciOi...', auth_type := 'BEARER',
              timeout := 10000);
```

### JSON response to table

```sql
CREATE TABLE posts AS
SELECT unnest(from_json(content::JSON,
       '[{"userId":"INTEGER","id":"INTEGER","title":"VARCHAR"}]'), recursive := true)
FROM http_get('https://jsonplaceholder.typicode.com/posts');
```

`from_json` with an explicit structure is faster and more predictable than `json_transform`
guessing types. For large or irregular payloads, write `content` to a file and use
`read_json` instead.

### Paging through an API, or one call per row

The arguments of every erpl table function must be **constants**. A column reference, as in
`LATERAL http_get(t.url)` or a recursive CTE that feeds the next URL back in, fails with
*"does not support lateral join column parameters"*. To make one call per page or per row,
generate the statements and run them, in the DuckDB CLI:

```sql
CREATE TABLE responses (page BIGINT, status INTEGER, content VARCHAR);
.mode list
.headers off
.once calls.sql
SELECT format('INSERT INTO responses SELECT {0}, status, content FROM http_get(''https://api.example.com/v1/items?page={0}'');', page)
FROM range(1, 6) t(page);
.read calls.sql
```

For JSON over plain GET with a known list of URLs, DuckDB's own `read_json` (with the
httpfs extension) also takes a list: `read_json(['https://.../p1', 'https://.../p2'])`.

Cursor or next-link APIs need the previous response to build the next URL, so drive them
from a host language (Python, a shell loop) or run the generate-and-read step repeatedly
until no `next` link is left. Always bound the loop, so a misbehaving API cannot run
forever.

## Canonical exploration workflow

```sql
-- 1. Credentials scoped to the service host (skip for public services)
CREATE SECRET svc (TYPE http_basic, USERNAME 'u', PASSWORD 'p', SCOPE 'https://svc.example.com/');

-- 2. What entity sets are there?
ATTACH 'https://svc.example.com/odata/v4/catalog' AS svc (TYPE odata);
SHOW TABLES FROM svc;

-- 3. What does one look like?
SELECT * FROM odata_describe('https://svc.example.com/odata/v4/catalog/Orders');

-- 4. Sample with pushdown
SELECT * FROM svc.Orders WHERE Status = 'OPEN' LIMIT 20;

-- 5. Materialise what you need
CREATE TABLE open_orders AS
SELECT OrderID, CustomerID, Amount, CreatedAt
FROM odata_read('https://svc.example.com/odata/v4/catalog/Orders', strict_typing := true)
WHERE Status = 'OPEN';
```

See also: `erpl-odata-setup` (secrets, TLS), `erpl-odata-sap-gateway` (SAP-specific
discovery and quirks), `erpl-odata-production` (incremental loads, scheduling, tracing,
error playbook). To profile or validate what you extracted, the anofox-tabular plugin's
`anofox-tabular-profiling` and `anofox-tabular-data-quality` skills apply directly.

Reference docs:
- `README.md` (HTTP Functions, OData v2/v4)
- `docs/TLS.md`
