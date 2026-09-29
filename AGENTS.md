# Agent Instructions: erpl-odata

**DuckDB Extension for HTTP/REST, OData, and SAP Ecosystem Integration**

This document guides AI agents in developing the erpl-odata extension using quick iteration workflows, tracing-enabled debugging, and C++ best practices.

---

## Project Overview

**ERPL-OData** is a production-grade DuckDB extension providing HTTP/REST API access, OData v2/v4 query support, and SAP ecosystem integration (Datasphere, Analytics Cloud, ODP) directly from SQL.

### Core Components

| Component | Responsibility | Key Files |
|-----------|---------------|-----------|
| **HTTP Layer** | GET/POST/PUT/DELETE with auth, headers, timeouts | `http_client.*`, `web_functions.*` |
| **OData v2/v4** | Universal reader with version detection, pushdown | `odata_client.*`, `odata_read_functions.*` |
| **SAP Datasphere** | Space/asset discovery, data reading | `datasphere_client.*`, `datasphere_catalog.*` |
| **SAP Analytics Cloud** | Model discovery, planning data | `sac_client.*`, `sac_catalog.*` |
| **ODP Integration** | Operational Data Provisioning | `odp_client_integration.*`, `odp_subscription_*` |
| **OAuth2 Auth** | Token flows, secret management | `oauth2_flow_v2.*`, `secret_functions.*` |
| **Delta Sharing** | Databricks/SAP Delta Share protocol | `delta_share_client.*`, `delta_share_catalog.*` |

### Architecture Pattern

**Service Layers with Pooling/Caching:** HTTP connections pooled, OData metadata cached, predicate pushdown to minimize data transfer.

```
SQL Query → DuckDB → erpl_odata functions → HTTP Client (pooled)
                                       → OData/SAP Adapters → Response Parsing → DuckDB Rows
```

---

## Development Philosophy

### Quick Iteration Workflow

Build feature-by-feature with rapid feedback:
1. Make code changes
2. Rebuild with Ninja: `GEN=ninja make debug`
3. Test immediately: `./build/debug/duckdb -s "YOUR_SQL"`
4. Repeat until working
5. Run full tests: `make test_debug`

### Tracing-Enabled Debugging

Always use tracing to understand problems:
```sql
SET erpl_trace_enabled = TRUE;
SET erpl_trace_level = 'DEBUG';
SET erpl_trace_output = 'console';
SELECT * FROM http_get('https://httpbin.org/ip');
```

### Test-First Approach

- Write SQLLogicTest for expected behavior
- Implement minimal code to pass
- Refactor while keeping tests green

### C++ Code Standards

**Memory:** Use `duckdb::unique_ptr`/`duckdb::shared_ptr`, never raw `new`/`delete`

**Naming:**
| Element | Style | Example |
|---------|-------|---------|
| Files | `snake_case` | `http_client.cpp` |
| Classes | `CamelCase` | `HttpClient` |
| Functions | `CamelCase` | `GetMetadata()` |
| Members/params | `snake_case` | `response_headers` |

**Principles:** SOLID, early returns, const correctness, small functions (20-30 lines max)

---

## Build & Test Commands

### Essential Commands

```bash
# Fast incremental build (always use Ninja)
GEN=ninja make debug

# Run all SQL tests
make test_debug

# Run C++ unit tests
./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests

# Single C++ test
./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests TEST_NAME

# SAP-specific tests (requires SAP environment)
make test_debug_sap
```

### Direct SQL Execution

```bash
# Single command (fastest iteration)
./build/debug/duckdb -s "SELECT * FROM http_get('https://httpbin.org/ip')"

# Interactive shell
./build/debug/duckdb

# Execute from file
./build/debug/duckdb < test_query.sql
```

### Situational Awareness

```bash
# List HTTP functions
./build/debug/duckdb -s "SELECT * FROM duckdb_functions() WHERE function_name LIKE '%http%'"

# Check loaded extensions
./build/debug/duckdb -s "SELECT * FROM duckdb_extensions()"
```

### Important: Rebuild After Changes

After modifying C++ source, you **MUST** rebuild before testing:
```bash
GEN=ninja make debug
./build/debug/duckdb -s "SELECT * FROM your_function()"
```

---

## File Organization

### Source Structure

```
src/
  erpl_odata_extension.cpp       # Entry point, function registration
  include/                     # Header files (.hpp)
    http_client.hpp
    odata_client.hpp
    datasphere_client.hpp
    sac_client.hpp
    ...
  http_client.cpp              # HTTP layer
  web_functions.cpp            # http_get/post/put/delete
  odata_*.cpp                  # OData v2/v4 support
  odp_*.cpp                    # SAP ODP integration
  datasphere_*.cpp             # SAP Datasphere
  sac_*.cpp                    # SAP Analytics Cloud
  oauth2_*.cpp                 # Authentication
  delta_share_*.cpp            # Delta Sharing protocol
```

### Test Structure

```
test/
  sql/                         # SQLLogicTest files
    web_core.test              # HTTP core tests
    web_http.test              # HTTP function tests
    odata_v2.test              # OData v2 tests
    odata_v4.test              # OData v4 tests
    delta_share.test           # Delta Share tests
    datasphere/                # Datasphere tests
    sap/                       # SAP-specific tests
  cpp/                         # C++ unit tests
    test_http_client.cpp
    test_odata_client.cpp
    ...
  fixtures/                    # Test data
```

### Configuration Files

```
CMakeLists.txt                 # Build config
extension_config.cmake         # Extension registration
vcpkg.json                     # Dependencies (openssl, tinyxml2, cpptrace)
Makefile                       # Build orchestration
```

---

## Module Architecture

### Dependency Flow

```
erpl_odata_extension (Entry Point)
    │
    ├── Foundation Layer
    │   ├── HttpClient (libcurl-based, connection pooling)
    │   ├── ErplTracer (logging: TRACE/DEBUG/INFO/WARN/ERROR)
    │   ├── CharsetConverter (UTF-8, ISO-8859-1/15, Windows-1252)
    │   └── DuckDBArgumentHelper (parameter binding)
    │
    ├── OData Layer
    │   ├── ODataClient (version detection, metadata caching)
    │   ├── ODataCatalog (schema discovery)
    │   ├── ODataContent (JSON/XML parsing)
    │   ├── ODataEdm (type mapping)
    │   └── PredicatePushdownHelper ($filter, $select, $top)
    │
    ├── SAP Layers
    │   ├── DatasphereClient → DatasphereRead
    │   ├── SacClient → SacCatalog
    │   └── OdpClient → OdpSubscriptionManager
    │
    └── Authentication Layer
        ├── OAuth2Flow (authorization_code, client_credentials)
        ├── SecretFunctions (CREATE SECRET integration)
        └── OAuth2Server (local callback server)
```

### Key Design Patterns

1. **Connection Pooling:** HTTP connections reused across requests
2. **Metadata Caching:** OData metadata fetched once, cached
3. **Predicate Pushdown:** SQL WHERE → OData $filter
4. **Secret Provider:** Flexible credential management (OAuth2, config, file)
5. **Response Parsing:** Structured ODataContent objects with type mapping

---

## Session Protocol

### Starting a Session

```bash
# 1. Verify build works
GEN=ninja make debug

# 2. Run tests to confirm baseline
make test_debug

# 3. Check current state
git status
```

### Landing the Plane (Session Completion)

**Work is NOT complete until `git push` succeeds:**

```bash
# 1. Run quality gates
GEN=ninja make debug && make test_debug

# 2. Commit changes (conventional commit style)
git add <files>
git commit -m "feat: add new functionality"

# 3. Push to remote
git push

# 4. Verify push succeeded
git status  # Must show "up to date with origin"
```

### Commit Message Format

Use conventional commits **WITHOUT** AI attribution:

```bash
git commit -m "feat: add SAC model discovery"
git commit -m "fix: resolve OAuth2 token refresh"
git commit -m "perf: optimize OData metadata caching"
git commit -m "refactor: extract HTTP connection pooling"
git commit -m "test: add Datasphere secret provider coverage"
```

**DO NOT include:**
- "Generated with Claude Code"
- "Co-Authored-By: Claude"
- Any AI/automation references

---

## Getting Unstuck

### Codex Consultation Protocol

When stuck on complex architectural decisions (after 15-20 minutes of attempts):

1. **Prepare context file** in `.ai/codex_context/`
2. **Invoke Codex:** `codex exec --file context.md --prompt "Recommend approach"`
3. **Implement** based on consultation
4. **Document** decision in code and commit

See the full Codex consultation workflow in the detailed instructions below.

### Common Issues

| Problem | Solution |
|---------|----------|
| Extension won't load | Check `DUCKDB_EXTENSION_ENTRY` macro |
| Function not found | Verify registration in `erpl_odata_extension.cpp` |
| Test fails unexpectedly | Check SQLLogicTest type indicators (I/T/R/D/B) |
| Linker errors | Verify CMakeLists.txt dependencies |
| OAuth2 flow fails | Enable tracing, check callback server |
| Windows build fails | Check VCPKG triplet, path handling |

---

## Common Development Tasks

### Adding a New HTTP Function

1. Define in `web_functions.cpp` (bind + execute phases)
2. Register in `erpl_odata_extension.cpp`
3. Add tests in `test/sql/web_http.test`
4. Use `ERPL_TRACE_*` macros for debugging

### Extending OData Support

1. Modify `odata_client.cpp` for query generation
2. Update `odata_edm.hpp` for new type mappings
3. Enhance `odata_content.cpp` for parsing
4. Update `odata_predicate_pushdown_helper.hpp` if needed
5. Add tests in `test/sql/odata_*.test`

### Adding a New SAP Integration

1. Create client module following SAC/Datasphere patterns
2. Implement catalog discovery functions
3. Implement data reading with pushdown
4. Add secret/auth handling
5. Register in extension entry point
6. Create tests in `test/sql/`

---

## Testing Requirements for New Features

### Mandatory Test Coverage

Every new feature **MUST** include both SQL integration tests AND C++ unit tests. Features without proper test coverage are incomplete.

#### 1. SQL Integration Tests (Most Important)
SQL tests verify the **public API** that users will actually call.

**Required coverage:**
- Happy path: Basic functionality works
- Error handling: Invalid inputs produce clear error messages
- Edge cases: Empty inputs, boundary conditions
- Cleanup: Resources are properly released

**Example pattern:**
```sql
# test/sql/feature_name.test

# Test 1: Happy path
statement ok
CREATE SECRET my_feature_secret (TYPE feature_type, param 'value');

# Verify it worked
query III
SELECT name, type, provider FROM duckdb_secrets() WHERE name = 'my_feature_secret';
----
my_feature_secret	feature_type	default_provider

# Test 2: Error handling - missing required parameter
statement error
CREATE SECRET bad_secret (TYPE feature_type);
----
'param' is required

# Cleanup
statement ok
DROP SECRET IF EXISTS my_feature_secret;
```

#### 2. C++ Unit Tests (For Internal Logic)
C++ tests verify internal functions that can't be tested via SQL.

**Required coverage:**
- URL/path construction
- Data parsing (JSON, XML, etc.)
- Token/expiration logic
- Helper functions

**Example pattern:**
```cpp
TEST_CASE("Feature URL Generation", "[feature][basic]") {
    auto url = FeatureClient::GetUrl("param");
    REQUIRE(url == "https://expected.url/path");
}

TEST_CASE("Feature Data Parsing", "[feature][basic]") {
    // Test with valid JSON
    auto result = ParseFeatureResponse(valid_json);
    REQUIRE(result.success);

    // Test with invalid JSON
    REQUIRE_THROWS(ParseFeatureResponse(invalid_json));
}
```

### Test Review Checklist

Before marking a feature complete, verify:

- [ ] All **public SQL functions** have integration tests
- [ ] All **internal helper functions** have C++ unit tests
- [ ] **Error paths** are tested (what happens when things go wrong?)
- [ ] **Edge cases** are covered (empty inputs, large inputs, special characters)
- [ ] Tests **actually run** (`make test_debug` passes)
- [ ] Tests are **deterministic** (no flaky tests)

### Authentication/Secret Features - Extra Requirements

For features that involve secrets or authentication:

- [ ] Secret creation with all providers tested
- [ ] Required parameter validation tested
- [ ] Token caching/refresh logic tested
- [ ] Credential redaction verified
- [ ] Multi-tenant scenarios tested (if applicable)

### Test File Locations

| Test Type | Location | Runner |
|-----------|----------|--------|
| SQL Integration | `test/sql/*.test` | `make test_debug` |
| C++ Unit | `test/cpp/test_*.cpp` | `./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests` |
| Fixtures | `test/fixtures/` | N/A |

---

## Build System Details

### Debug vs Release Linking

- **Debug:** Extension statically linked into `./build/debug/duckdb`
- **Release:** Loadable `.duckdb_extension` module

### VCPKG Dependencies

```json
{
  "dependencies": ["openssl", "tinyxml2", "cpptrace"]
}
```

Requires `VCPKG_ROOT` environment variable.

### CI/CD (GitHub Actions)

- Multi-platform: Linux, macOS, Windows
- Uses `extension-ci-tools` reusable workflows
- Deploys to S3 for distribution

Diagnose failures with: `gh run view <RUN_ID> --log`

---

## When Claude Code Gets Stuck: Consulting Codex

### When to Consult

- Architectural ambiguity (multiple valid approaches)
- Deep technical unknowns (undocumented behavior)
- Security/correctness critical decisions
- Build system configuration mysteries
- Complex debugging scenarios

### Consultation Workflow

```bash
# 1. Create context file
mkdir -p .ai/codex_context
cat > .ai/codex_context/$(date +%Y%m%d_%H%M%S)_topic.md << 'EOF'
# Codex Consultation: [TOPIC]

## Context
[What you're working on]

## Problem
[What you're stuck on]

## Options Considered
1. Option A: pros/cons
2. Option B: pros/cons

## Question
[Specific question]

## Constraints
- Must follow DuckDB patterns
- Must be thread-safe
- ...

## Code Snippets
[Relevant code]
EOF

# 2. Get Codex response
codex exec --file .ai/codex_context/TIMESTAMP_topic.md \
           --prompt "Recommend the best approach" \
           > .ai/codex_context/TIMESTAMP_topic_response.md

# 3. Implement and document
```

### When NOT to Consult

- Syntax errors
- Simple refactoring
- Straightforward bugs with clear cause
- Documentation updates

---

## References

- **README.md** - Feature documentation and SQL examples
- **docs/UPDATING.md** - DuckDB version update process
- **.ai/README.md** - Extension analysis
- **DuckDB Extension Template** - https://github.com/duckdb/extension-template
- **Extension CI Tools** - https://github.com/duckdb/extension-ci-tools
- **DuckDB CONTRIBUTING.md** - https://github.com/duckdb/duckdb/blob/main/CONTRIBUTING.md
- **Community Extensions** - https://duckdb.org/community_extensions/list_of_extensions
- **OpenAI Codex** - https://github.com/openai/codex
