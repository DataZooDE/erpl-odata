# Code Quality Refactoring Playbook

This note records the process used for the cleanup passes on
`src/odata_read_functions.cpp`. Use it as the template for similar reviews in
other parts of the extension.

## Diagnosis From This Pass

The main issue in `src/odata_read_functions.cpp` was not one isolated bad
function. The file had accumulated several responsibilities:

- DuckDB table function binding and scanning
- OData V2/V4 response shape handling
- JSON document lifetime management
- JSON-to-DuckDB value conversion
- expanded navigation row extraction
- `odata_describe` result type and row construction

That made the code harder to reason about than nearby modules such as
`src/odata_client.cpp` and `src/odata_content.cpp`, where responsibilities are
more explicit and helper types do more of the work.

Concrete hotspots found:

- `ParseJsonValueToDuckDBValue` mixed type dispatch, primitive parsing, list
  conversion, struct conversion, OData V2 date handling, boolean parsing, and
  fallback behavior in one long branch chain.
- OData V2 and V4 expanded data processing had duplicate loops. The only real
  difference was how the row array is reached.
- JSON document ownership was managed manually with repeated
  `yyjson_doc_free(...)` calls.
- Column extraction serialized first-row objects back to JSON and reparsed
  them, duplicating payload traversal and making the metadata-field filtering
  harder to see.
- `odata_describe` repeated the same nested DuckDB `STRUCT` type construction
  for bind data, empty lists, and row values.

## Fixes Applied

The first pass stayed inside the existing public API and avoided moving symbols
between files. This kept the risk bounded while reducing the worst complexity.

- Added a small RAII `JsonDocHandle` for `yyjson_doc` ownership.
- Centralized OData metadata-field filtering and JSON field-name normalization.
- Reworked column extraction to operate directly on the first row object when
  possible instead of serializing and reparsing.
- Split JSON-to-DuckDB conversion into small helpers by target type:
  varchar, integer, bigint, float/double, decimal, boolean, timestamp, list,
  struct, and fallback.
- Collapsed V2/V4 expanded row conversion into shared row processing after the
  version-specific array lookup.
- Centralized `odata_describe` result schema construction in helper functions.
- Added a focused C++ regression test for initial JSON content without
  metadata, covering the column-name path touched by the refactor.

The second pass converted the first pass into a clearer file layout without
changing the SQL surface:

- Moved `ODataDataExtractor` and its JSON conversion helpers to
  `src/odata_data_extractor.cpp`.
- Moved `odata_describe` bind/scan/schema code to
  `src/odata_describe_functions.cpp`.
- Registered both new implementation files in `CMakeLists.txt`.
- Left `src/odata_read_functions.cpp` focused on read binding, scan state,
  row buffering, progress tracking, and error handling.

After the split, `src/odata_read_functions.cpp` is down to about 2.1k lines,
with about 1.1k lines in `src/odata_data_extractor.cpp` and about 0.5k lines
in `src/odata_describe_functions.cpp`. That is still not "small", but each
file now has a narrower reason to change.

## Repeatable Workflow

1. Start with `git status --short` and note unrelated user changes. Do not
   modify or revert them.
2. Map the module's public surface: SQL functions, C++ classes, headers, tests,
   and registration points.
3. Compare against adjacent better-structured modules before inventing a new
   style. For OData read cleanup, `odata_client.cpp` and `odata_content.cpp`
   were useful references.
4. Identify hotspots with concrete signals: long functions, repeated branches,
   repeated type construction, manual resource lifetime, hidden fallback paths,
   and mixed protocol/application/DuckDB concerns.
5. Add or identify focused tests before broad refactoring. Prefer tests that
   pin externally visible behavior or a tricky internal contract.
6. Extract pure helpers first. Keep them private to the file unless multiple
   modules already need them.
7. Introduce RAII wrappers before changing logic around manually released
   resources.
8. Collapse duplicated version branches only after naming the true difference
   between the branches. For OData V2/V4, array discovery differs; row
   conversion does not.
9. Centralize schema/type construction so bind code and row construction cannot
   drift.
10. Rebuild before testing runtime behavior: `GEN=ninja make debug`. If the full
    build fails in an unrelated auxiliary target, verify the touched targets
    directly with `cmake --build build/debug --target duckdb erpl_odata_tests`.
11. Run focused tests first, then broader tests when the environment allows it.
12. Record residual debt instead of mixing a large file split into the first
    behavior-preserving cleanup.
13. When the first pass is green, split one responsibility at a time into a new
    implementation file, register it in the build, rebuild, and rerun the
    focused tests before moving the next responsibility.

## Codebase Audit Round 1

After the OData read cleanup, the same review was applied across `src/` using
two filters:

- Structural risk: file size, public SQL surface, and number of different
  responsibilities in one file.
- Refactoring smell: duplicated resource ownership, duplicated DuckDB schema
  construction, manual JSON/XML parsing, direct HTTP calls mixed with result
  row construction, and repeated bind/scan state.

The largest/highest-risk files found in this pass were:

- `src/include/odata_edm.hpp` at about 2.5k lines.
- `src/datasphere_catalog.cpp` at about 1.7k lines.
- `src/odata_content.cpp` and `src/http_client.cpp`, both above 1k lines.
- Graph reader modules around 0.9k to 1.1k lines each.

### Cleanup Applied: Graph JSON Scan State

The Graph SharePoint and Excel table functions repeated the same scan state in
multiple bind-data structs:

- JSON response string
- raw `yyjson_doc *`
- array iterator
- `done` flag
- destructor calling `yyjson_doc_free(...)`
- `InitIterator()` implementation for the `value` array

That pattern is simple but risky because every new table function had to copy
manual lifetime handling correctly. The cleanup introduced
`GraphJsonArrayScanBindData` in `src/include/graph_json_scan.hpp` and switched
the SharePoint and Excel list-style functions to inherit from it.

This is intentionally a small shared abstraction. It owns only the common JSON
document lifecycle and array-iterator initialization; each function still keeps
its own URL arguments, output schema, HTTP request, and row conversion.

### Highest Priority: `src/datasphere_catalog.cpp`

This file is the best next target because it mixes SQL binding, OAuth token
loading, HTTP endpoint probing, XML parsing, JSON parsing, schema inference,
fallback behavior, and DuckDB result row construction.

Concrete hotspots:

- `GetOrRefreshDatasphereToken` keeps secret/token handling in the catalog
  implementation.
- `FetchDetailedAnalyticalSchema` mixes an initial metadata client request with
  a second raw HTTP fetch of the same metadata URL. This should be reviewed for
  necessity before changing behavior.
- `ParseAnalyticalMetadata` parses XML with string searches such as
  `<Property`, `find(...)`, and `substr(...)` even though the project already
  depends on `tinyxml2` and has OData EDM XML parsing code.
- `ParseDwaasAnalyticalSchema` and `ParseDwaasRelationalSchema` duplicate JSON
  document lifetime management and DuckDB list/struct construction.
- `LoadResourceDetails` combines endpoint selection, authentication, HTTP
  fallback logic, parser selection, and row assembly.

Recommended split:

- `datasphere_auth.cpp`: token/secret loading helpers used by catalog and read
  functions.
- `datasphere_metadata_parser.cpp`: XML and DWAAS JSON schema parsing.
- `datasphere_schema_values.cpp`: DuckDB `STRUCT`/`LIST` type and value
  builders for schema rows.
- `datasphere_catalog.cpp`: bind/scan functions and SQL registration only.

First safe changes:

- Extract a local RAII wrapper for `yyjson_doc` or reuse an existing one if it
  becomes shared.
- Extract common schema-field struct builders before changing parser behavior.
- Replace the string-based XML parser with `tinyxml2` behind the same returned
  `duckdb::Value` shape, with tests covering representative metadata.

Progress applied:

- Added local `yyjson_doc` RAII ownership for the DWAAS JSON schema parsers.
- Centralized analytical/relational schema `STRUCT`/`LIST` type and value
  builders so bind schemas and row values share the same construction path.
- Simplified DWAAS analytical and relational JSON parsing around small helpers
  for JSON field extraction, `definitions` traversal, and field classification.
- Added focused C++ parser coverage for analytical schema JSON, relational
  schema JSON, and invalid JSON.

Remaining Datasphere cleanup:

- Move the parser/value helpers into dedicated implementation files once the
  XML parser tests are in place.
- Replace the string-based `ParseAnalyticalMetadata` XML parser with
  `tinyxml2`.
- Review the double metadata fetch in `FetchDetailedAnalyticalSchema` before
  changing request behavior.
- Split `LoadResourceDetails` after parser behavior is pinned with tests.

### Second Priority: `src/include/odata_edm.hpp`

This header is about 2.5k lines and contains model definitions, XML parsing,
primitive constants, and type conversion logic. Because most implementations
are inline, every include pays the compile-time and dependency cost.

Concrete hotspots:

- Many `FromXml(...)` implementations live inline in the header.
- Primitive type constants are defined in the header.
- `DuckTypeConverter` contains repeated Edm-to-DuckDB mapping branches.

Recommended split:

- Keep lightweight model declarations in `odata_edm.hpp`.
- Move XML parser implementations to `src/odata_edm.cpp`.
- Move type conversion to `src/odata_type_converter.cpp` or a smaller dedicated
  helper if that better matches existing include patterns.
- Centralize primitive type mapping in one table/function so all callers share
  the same behavior.

This refactor should be staged carefully because the header is broad and can
create compile/link churn.

### Lower Priority: `src/http_client.cpp`

The HTTP client is large, but its responsibilities are more coherent than the
Datasphere catalog file. It should come after the mixed catalog/parsing files.

Good splits when touched:

- URL parsing and normalization.
- libcurl option construction.
- response-header/body collection.
- auth/header composition.
- charset/response decoding if not already owned elsewhere.

## Next Proposals

The current passes reduce the worst local complexity, but the remaining read
path still has too many reasons to change. Good next steps are:

- Split the remaining read path into smaller implementation files:
  `odata_read_bind.cpp` for bind-data construction and argument parsing, and
  possibly `odata_service_scan.cpp` for service-root row production.
- Move generic JSON-to-DuckDB value conversion behind a dedicated converter
  class if other modules need the same behavior; keep it private to
  `odata_data_extractor.cpp` until there is proven reuse.
- Extract service-root and entity-set scan row builders from the table function
  code.
- Replace placeholder-style C++ tests in OData read/expand coverage with tests
  that assert actual bind, URL, and conversion behavior.
- Add focused `odata_describe` tests that assert the schema rows for entity
  sets, properties, navigation properties, and functions.
- Continue the `src/datasphere_catalog.cpp` cleanup by replacing the XML
  string parser with a tested `tinyxml2` implementation.
- Move `src/include/odata_edm.hpp` implementation details into `.cpp` files
  after the Datasphere parser work has tests.
- Continue extracting Graph helpers only where at least two function families
  already share the same state or paging behavior.
