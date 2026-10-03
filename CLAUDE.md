# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Self-Optimisation Principle

When you discover a non-obvious requirement, workaround, or environment quirk during a session:

1. **Add a Makefile target** that encapsulates the correct invocation (with flags, env vars, etc.).
2. **Update this file (CLAUDE.md)** to document the target, explain *why* the raw command doesn't work, and point future sessions to the target.
3. **Never document "run the binary directly" when a Makefile target is available** — targets are the canonical, always-correct interface.

This keeps institutional knowledge in the repo and prevents rediscovering the same workarounds.

---

## Project Overview

**ERPL-OData** is a production-grade DuckDB extension that provides HTTP/REST API access, OData v2/v4 query support, and SAP ecosystem integration (Datasphere, Analytics Cloud, ODP) directly from SQL. The codebase is primarily C++ with SQLLogicTest-based tests.

**Key capabilities:**
- HTTP functions (GET, HEAD, POST, PUT, PATCH, DELETE) with OAuth2 and DuckDB Secrets integration
- Universal OData v2/v4 reader with automatic version detection and predicate pushdown
- SAP Datasphere access with discovery and metadata functions
- SAP Analytics Cloud querying with model discovery and story extraction
- Comprehensive tracing and caching system
- Character set detection and conversion (UTF-8, ISO-8859-1/15, Windows-1252)

## Build & Development Commands

### Fast Incremental Build for Local Development (Recommended)

**Use `make dev` for day-to-day iteration — it skips cmake reconfiguration:**
```bash
# Fast incremental build (skips cmake configure when already configured)
make dev

# Run tests after building
make test_debug
```

**Why `make dev` instead of `make debug`?**

`make debug` (from `extension-ci-tools/makefiles/duckdb_extension.Makefile`) always runs
`cmake -S duckdb/ -B build/debug` unconditionally, which triggers vcpkg's compiler-hash
detection (`Detecting compiler hash for triplet x64-linux...`) even when nothing has changed.
This adds 30–60 seconds to every build regardless of what was edited.

`make dev` checks whether `build/debug/build.ninja` already exists and only runs cmake
configure on the first build or after `make clean`. Ninja's own dependency scanner detects
`CMakeLists.txt` changes and re-runs cmake configure automatically when needed, so you never
miss a reconfiguration that matters.

`make dev` builds only `shell`, `unittest` and `erpl_odata_tests`, not `all`. `all` also builds
DuckDB-internal tools (e.g. `plan_serializer`) whose link can fail on a newer toolchain; that
aborts the build before `./build/debug/duckdb` is relinked, so the shell silently runs stale
code. Do not "fix" a stale shell by running `cmake --build` by hand: use `make dev`.

If `build/debug` was configured under a different checkout path (the repo was renamed from
`erpl-web`), cmake fails with "CMakeCache.txt directory is different". Delete
`build/debug/CMakeCache.txt`, `build/debug/CMakeFiles` and `build/debug/build.ninja`, then
`make dev` (a full rebuild).

**When to use each target:**

| Target | When to use |
|--------|-------------|
| `make dev` | Normal iterative development — fastest, skips unnecessary reconfigure |
| `make debug` | First-time setup or after adding a new vcpkg dependency |
| `GEN=ninja make debug` | Force full reconfigure with Ninja (e.g. CI-like clean build) |

### Standard Build Commands

Without Ninja (slower, but works everywhere):
```bash
make debug      # Full reconfigure + debug build
make release    # Full reconfigure + release build
```

**Note:** Use `make clean` seldomly, it leads to long build times and is often not necessary.

### Testing

**Full test suites:**
- `make test_debug` - Run all SQL tests (SQLLogicTests)
- `make test` - Run all tests (release mode)
- `make test_cpp` - Run C++ unit tests (**always use this**, not the binary directly — see note below)
- `make test_debug_sap` - Run SAP-specific tests against the local A4H sandbox. The ODP
  cases need a **provisioned ODP service**, which a stock system does not have, so they
  are gated on `ERPL_SAP_ODP_SERVICE` / `ERPL_SAP_ODP_ENTITY_SET` and skip with a stated
  reason when those are unset:
  `make test_debug_sap ERPL_SAP_ODP_SERVICE=Z_ODP_BW_1_SRV ERPL_SAP_ODP_ENTITY_SET=FactsOf0D_NW_C01`.
  Pass them through the target rather than exporting them by hand: sqllogictest's
  `require-env` skips only when `getenv` returns null, so an **empty but exported**
  variable runs the ODP cases and fails them. The target omits the variables entirely
  when they are empty, which is the only shape that skips correctly. Function
  registration is covered unconditionally in `test/sql/odp_functions_registered.test`, so
  it does not skip along with the live cases.
  The **delta** lifecycle is gated separately again, on `ERPL_SAP_ODP_DELTA_SERVICE` /
  `ERPL_SAP_ODP_DELTA_ENTITY_SET`, because being delta-capable is a property of the
  extractor and not of ODP — see the ODP provisioning section below.
- `make test_oauth2` - Build and run **datazoo-oauth2's own Catch2 suite**, which no build of
  this repo used to run. Its cmake target is guarded on the submodule being the *top-level*
  project (`NOT DATAZOO_OAUTH2_HAS_PARENT`), and erpl-odata `add_subdirectory()`s it - so the
  guard was always false, the suite was never compiled, and the only coverage of
  `OAuth2CallbackHandler::ValidateState` (the CSRF defence of the authorization_code flow),
  the PKCE/state token generator and the callback error page's HTML escaping ran nowhere.
  This target configures the submodule standalone in `build/datazoo-oauth2-tests`, which is
  the configuration that guard already supports, so nothing in the submodule changes. It
  needs **Catch2 3** on the system and fails loudly if the binary was not produced - a silent
  skip is what hid this for so long. Wired into CI as the `oauth2-unit-tests` job, which is
  deliberately NOT gated on the extension build so a broken CSRF check is reported in a
  minute rather than behind a ~25-minute Windows leg. See GitHub #245.
- `make test_build_guard` - Regression test for GitHub #45 (pure CMake, no build needed). Verifies the dev-only C++ test target stays gated on the in-tree `duckdb` submodule so consumers who statically link erpl_odata (via `duckdb_extension_load`/FetchContent, where the submodule is absent) don't try to compile `test/cpp` and hit `catch.hpp file not found`. Run after touching the `add_subdirectory(test)` guard in `CMakeLists.txt`.
- `make unittest` - Relink **only** `./build/debug/test/unittest`, the SQLLogicTest runner.
  `duckdb`, `unittest` and `erpl_odata_tests` are **separate ninja targets**: building one
  does not build the others. A session that builds a single target after a source change
  (common when a full `make dev` is failing for an unrelated reason) leaves the other
  binaries stale — and a stale `unittest` runs your SQL tests against code from *before*
  the fix, which is indistinguishable from a real behavioural bug. This cost a full
  investigation in GitHub #172, where the same ODP delta case passed from
  `./build/debug/duckdb` and failed under `unittest` with no source difference at all.
  `test_debug_sap`, `test_debug_ms` and `test_debug_bc` depend on this target, so prefer
  them over invoking the runner by hand.
- `./build/debug/test/unittest "[test_category]"` - Run SQL tests by category (e.g., `"[sap]"`).
  Run `make unittest` first — see above.

**Running individual C++ tests:**
- `ASAN_OPTIONS=detect_odr_violation=0 ./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests -l` - List all tests
- `ASAN_OPTIONS=detect_odr_violation=0 ./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests TEST_NAME` - Run single test

**Why `ASAN_OPTIONS=detect_odr_violation=0` is required for C++ tests:**
The debug build statically links DuckDB into the test binary AND loads `libduckdb.so`, causing
AddressSanitizer to report a false-positive ODR violation on `LOOKUP_TABLE` in
`nested_to_varchar_cast.cpp`. Both copies are identical — this is a structural artefact of
the debug linking strategy, not a real bug. The `make test_cpp` target sets this flag automatically.

**Why C++ tests must set `allocator_background_threads = true` in `DBConfig`:**
Any test that creates a bare `DuckDB db(nullptr)` and runs queries will crash in DEBUG+jemalloc
builds with `terminate called after throwing 'InternalException': je_mallctl failed for setting
"arena.N.purge"`. The crash happens ~0.5s after a query in DuckDB's background worker threads
(`TaskScheduler` → `Allocator::ThreadFlush` → `JemallocExtension::ThreadFlush` → throws under
`#ifdef DEBUG`). It is unrelated to `Connection` lifecycle — do not try to fix it there.

Always use this pattern in every C++ test that touches a `DuckDB` instance:
```cpp
DBConfig config;
config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));  // prevents jemalloc crash in debug builds
DuckDB db(nullptr, &config);
```
This delegates arena management to jemalloc's own background threads; DuckDB workers then skip
the manual `ThreadFlush` call entirely.

**Note (DuckDB v1.5.1+):** `config.options.allocator_background_threads` was removed; use
`config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true))` instead.
The setting is now a generic DBConfig setting stored via `user_settings`.

### C++ tests are compiled on Windows CI — write them portably

`test/cpp/*.cpp` is globbed into the extension build, so **every** test file is compiled on
the Windows leg. POSIX-only headers therefore break the build for everyone, and the failure
appears ~20 minutes later in CI rather than locally:

- `<unistd.h>` / `getpid()` — **not shipped by MSVC**. For a unique temp name use a
  `static std::atomic` counter plus the object address, not the pid.
- Prefer `<filesystem>`, `<chrono>`, `<thread>` over POSIX equivalents.

This has now cost two round trips (`test_delta_share_scan_reexecution.cpp` documented it in
a comment, and `test_credential_trace_redaction.cpp` then repeated it). A comment inside one
test file is not where the next author looks — hence this entry.

**SQL tests location:** `test/sql/` (various `.test` files organized by module)
**C++ unit tests location:** `build/debug/extension/erpl_odata/test/cpp/`

## Quick Iteration Development Workflow

For rapid feature development and debugging, use the debug binary directly without running full test suites:

### Direct SQL Command Execution

After building with `make debug`, execute SQL commands instantly:

```bash
# Single SQL command (fastest iteration)
./build/debug/duckdb -s "SELECT * FROM http_get('https://httpbin.org/ip')"

# Multiple commands (just example, in debug build, extension is statically linked)
./build/debug/duckdb -s "LOAD 'build/debug/extension/erpl_odata/erpl_odata.duckdb_extension'; SELECT 1"

# Interactive shell (for exploring)
./build/debug/duckdb

# Execute SQL from file
./build/debug/duckdb < test_query.sql
```

### Gaining Situational Awareness with DuckDB Switches

```bash
# Enable detailed query information
./build/debug/duckdb -s ".tables" # List all tables

# Get extension info
./build/debug/duckdb -s "SELECT * FROM duckdb_functions() WHERE function_name LIKE '%http%'"

# Check loaded extensions
./build/debug/duckdb -s "SELECT * FROM duckdb_extensions()"

# Profile query execution
./build/debug/duckdb -s ".mode line" -s "PRAGMA explain; SELECT * FROM http_get('...')"

# Enable verbose output
./build/debug/duckdb -verbose -s "SELECT * FROM http_get('...')"
```

### Iterative Development Cycle

1. **Make code changes** to source files
2. **Rebuild with Ninja** (fast): `GEN=ninja make debug`
3. **Test immediately**: `./build/debug/duckdb -s "YOUR_TEST_SQL"`
4. **Repeat** until working
5. **Run full test suite** when ready: `make test_debug`

**Key advantages:**
- No waiting for full test harness to load
- Direct output and error messages
- Faster iteration = faster development
- Instant feedback loop

### Important: Rebuild After Changes

**Critical:** After modifying C++ source code, you MUST rebuild:
```bash
# Code changed? Rebuild!
GEN=ninja make debug

# Only then run your test
./build/debug/duckdb -s "SELECT * FROM your_function()"
```

The debug binary (`./build/debug/duckdb`) contains your extension code. Changes are NOT automatically reflected until you rebuild. This is the trade-off for the speed benefit of debug builds.

### Example Workflow: Adding a New HTTP Header

```bash
# 1. Edit source code to add new header validation
# File: src/web_functions.cpp
vim src/web_functions.cpp

# 2. Quick rebuild with Ninja
GEN=ninja make debug

# 3. Test immediately without full test suite
./build/debug/duckdb -s "SELECT * FROM http_get('https://api.example.com', headers={'X-Custom':'value'})"

# 4. If working, run full tests
make test_debug

# 5. If all pass, commit
git add .
git commit -m "feat: add custom header validation"
```

### Debugging with Tracing

Combine direct SQL execution with tracing for maximum visibility:

```bash
./build/debug/duckdb << EOF
SET erpl_trace_enabled = TRUE;
SET erpl_trace_level = 'DEBUG';
SET erpl_trace_output = 'console';

SELECT * FROM http_get('https://httpbin.org/ip');
EOF
```

This produces immediate, detailed trace output showing all network calls and internal logic.
Always ensure to have enough traces in the code such that you can understand problems from the output.

## Build System & Dependencies

This extension follows the **DuckDB Extension Template** and uses **extension-ci-tools** for build management. Understanding the build architecture is essential for development.

### DuckDB Build System Architecture

The project structure consists of:
- **DuckDB Core** (submodule at `./duckdb/`) - Compiled as part of the extension build
- **Extension CI Tools** (submodule at `./extension-ci-tools/`) - Shared makefile and CMake logic for all DuckDB extensions
- **This extension** (`src/`, `test/`, `CMakeLists.txt`, `extension_config.cmake`) - Extension-specific source and configuration

**Build Flow:**
1. `make debug/release` → runs `extension-ci-tools/makefiles/duckdb_extension.Makefile`
2. CI Makefile generates CMake commands with DuckDB source directory as the root (`-S duckdb/`)
3. CMakeLists.txt in DuckDB root loads `extension_config.cmake` to configure this extension
4. `extension_config.cmake` calls `duckdb_extension_load()` to integrate this extension into the DuckDB build

### Debug vs Release Build Linking

**Critical difference in linking strategy:**

- **Debug builds** (`make debug`): Extension is **statically linked** into the DuckDB executable
  - Binary: `./build/debug/duckdb` (standalone executable with extension baked in)
  - Pros: Single binary, no loader needed, easier to debug
  - Cons: Larger binary size

- **Release builds** (`make release`): Extension is **dynamically loaded** (NOT statically linked)
  - Binary: `./build/release/duckdb` (DuckDB shell)
  - Extension: `./build/release/extension/erpl_odata/erpl_odata.duckdb_extension` (loadable module)
  - Configured via `DONT_LINK` flag in `extension_config.cmake`
  - Pros: Smaller binary, distributable extension artifact, standard for production
  - Cons: Requires extension loading at runtime: `LOAD 'path/to/erpl_odata.duckdb_extension'`

**See extension_config.cmake:**
```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    duckdb_extension_load(erpl_odata
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS
        DONT_LINK)              # <-- Release: loadable extension
else()
    duckdb_extension_load(erpl_odata
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS)             # <-- Debug: statically linked
endif()
```

### Dependency Management with VCPKG

This extension uses **VCPKG** for external C++ dependency management. VCPKG is managed externally; we do **NOT** store it in the repository.

**Required setup:**
```bash
# Set the VCPKG_ROOT environment variable pointing to your VCPKG installation
export VCPKG_ROOT=/path/to/vcpkg

# The Makefile uses this variable:
# VCPKG_TOOLCHAIN_PATH?=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
```

**Current dependencies** (defined in `vcpkg.json`):
- `openssl` - HTTPS/SSL support for HTTP requests
- `tinyxml2` - XML parsing for OData v2 and Datasphere responses
- `cpptrace` - Exception tracing and debugging

**Build flow:**
1. `make debug` reads `vcpkg.json` from project root
2. Passes VCPKG toolchain to CMake via `-DCMAKE_TOOLCHAIN_FILE='${VCPKG_TOOLCHAIN_PATH}'`
3. CMake uses VCPKG to fetch and build dependencies locally
4. Libraries are statically linked into the extension per `EXTENSION_STATIC_BUILD=1` flag

**Adding new dependencies:**
1. Add to `vcpkg.json` dependencies array
2. Add `find_package()` in `CMakeLists.txt`
3. Link via `target_link_libraries()` in CMakeLists.txt
4. Ensure dependency is open-source and compatible with BSL 1.1 license

### Build Configuration Details

The Makefile uses these key flags (from `extension-ci-tools/makefiles/duckdb_extension.Makefile`):

```makefile
# Static linking enabled by default
BUILD_FLAGS=-DEXTENSION_STATIC_BUILD=1

# Debug build command
debug:
    cmake -DCMAKE_BUILD_TYPE=Debug -S duckdb/ -B build/debug
    cmake --build build/debug --config Debug

# Release build command
release:
    cmake -DCMAKE_BUILD_TYPE=Release -S duckdb/ -B build/release
    cmake --build build/release --config Release
```

**Available build variants:**
- `debug` - Full debug info, no optimizations
- `release` - Optimized, stripped symbols
- `relassert` - Release optimization + assertions enabled (good for testing)
- `reldebug` - Release optimization + debug info (best for profiling)

### Build Performance Optimization

The build system supports **mold linker** for faster linking (macOS/Linux):
- Configured in `CMakeLists.txt` to auto-detect and use if available
- Can reduce link time by 50-70% compared to default linker
- Falls back to default linker if mold not found

## Git: never verify a fix against the working tree

`git checkout <ref> -- <path>` updates the **index as well as the working tree**. Restoring
that file afterwards with `cp` puts it back in the working tree only — the index keeps the
reverted version, and the next commit silently takes it.

This has silently reverted a committed fix three times in this repo (GitHub #146, #191/#200,
and the branch that fixed #196). Each time every local test run passed, because the test
binary is built from the **working tree**: the code that was tested was not the code that was
committed. `git status` shows nothing once the working tree is restored.

**Red-checking a fix** — temporarily reverting it to prove a test fails — is the operation
that triggers this, and it is a good practice worth keeping. Do it safely:

```bash
# Safe: worktree copy, never touches the index of your branch
cp src/thing.cpp /tmp/thing.keep
python3 - <<'EOF'   # or sed/patch — anything that writes ONLY the file
...revert the change in place...
EOF
make dev && ./build/debug/... "[tag]"     # expect red
cp /tmp/thing.keep src/thing.cpp          # restore
```

Never `git checkout <ref> -- <path>` for this. If you need a pristine copy of another
revision, redirect it instead — `git show <ref>:<path> > <path>` writes the file without
touching the index.

**Before pushing any branch, audit what it actually changed:**

```bash
git diff origin/main HEAD --name-only        # every file the branch touches
git diff origin/main HEAD -- <file>          # and what it did to each one
```

A file you only reverted-and-restored will appear here and nowhere else. Checking
`git status`, or checking only the files you believe you edited, is what failed all three
times.

---

## C++ Code Standards & Best Practices

**You must act as a Senior C++ Software Engineer and produce well-readable, maintainable, and high-quality code.**

### Memory Management
- **Never** use `malloc`/`free` or `new`/`delete`—these are code smells
- Use smart pointers: `duckdb::unique_ptr` (preferred) or `duckdb::shared_ptr` (only when necessary)
- Smart pointers prevent memory leaks and make ownership explicit
- Always use `duckdb::make_unique<T>()` instead of manual `new`

### Const Correctness & References
- Apply `const` liberally throughout code (const pointers, const member functions, const parameters)
- Prefer **const references** over pointers for function arguments
- Use const references for non-trivial objects: `const std::vector<T>&`, `const std::string&`
- Use `override` or `final` keyword explicitly when overriding virtual methods—don't repeat `virtual`

### Type Usage (DuckDB Style)
- Use fixed-width types: `uint8_t`, `int16_t`, `uint32_t`, `int64_t` instead of `int`, `long`, etc.
- Use `idx_t` instead of `size_t` for offsets, indices, and counts
- Never use `using namespace std;`—always fully qualify types or use specific imports

### Naming Conventions (Match DuckDB)

| Element | Style | Example |
|---------|-------|---------|
| Files (cpp/hpp) | lowercase_with_underscores | `http_client.cpp`, `odata_client.hpp` |
| Classes/Types | CamelCase (uppercase first letter) | `HttpClient`, `ODataVersion` |
| Enums | CamelCase | `enum class ODataVersion { V2, V4 }` |
| Functions/Methods | CamelCase (uppercase first letter) | `GetMetadata()`, `ParseResponse()` |
| Member variables | snake_case (lowercase) | `response_headers`, `is_initialized` |
| Function parameters | snake_case | `const std::string& url`, `size_t timeout_ms` |
| Constants/Macros | UPPER_CASE | `const int DEFAULT_TIMEOUT = 5000;`, `#define BUFFER_SIZE 1024` |
| Local variables | snake_case | `int retry_count = 0;` |
| Namespace | lowercase | `namespace erpl_web { }` |

### Code Structure & Formatting

Follow the `.clang-format` and `.clang-tidy` configuration:
- **Indentation:** Tabs (width 4)
- **Line length limit:** 120 characters
- **Pointer alignment:** Right (e.g., `int* ptr` not `int *ptr`)
- **Opening braces:** Always required (even for single-line if/for/while)
- **No single-line functions:** Use `AllowShortFunctionsOnASingleLine: false`
- **Spacing in templates:** No spaces (e.g., `std::vector<int>` not `std::vector< int >`)

### SOLID Principles & Clean Code (Uncle Bob)

#### Single Responsibility Principle (SRP)
- Each class/function should have one reason to change
- Example: `HttpClient` handles network I/O only; don't mix authentication logic
- Split responsibilities: `HttpClient` + `OAuth2Handler` + `SecretProvider`

#### Open/Closed Principle (OCP)
- Classes should be open for extension, closed for modification
- Use abstract base classes and inheritance (e.g., `SecretProvider` interface)
- Avoid modifying existing classes when adding features; extend instead

#### Liskov Substitution Principle (LSP)
- Derived classes must be substitutable for base classes
- If inheriting from a base, don't break expected behavior
- Example: `OAuth2Provider` should work anywhere `SecretProvider` is expected

#### Interface Segregation Principle (ISP)
- Clients should not depend on interfaces they don't use
- Create focused, minimal interfaces rather than bloated ones
- Example: separate `IDataReader` from `IMetadataProvider`

#### Dependency Inversion Principle (DIP)
- Depend on abstractions, not concrete implementations
- Inject dependencies rather than creating them internally
- Example: pass `HttpClient` to constructor, don't create new one inside

#### Clean Code Practices
- **Meaningful names:** Use full words, not abbreviations (e.g., `authentication_token` not `auth_tok`)
- **Small functions:** Functions should do one thing; aim for 20-30 lines maximum
- **Error handling:** Use exceptions or Result types, not error codes
- **Comments:** Only explain "why," never "what" (code should be self-explanatory)
- **No magic numbers:** Use named `constexpr` for all constants
- **DRY (Don't Repeat Yourself):** Extract common logic into shared functions
- **Early returns:** Use guard clauses to avoid deep nesting

**Example of clean code structure:**
```cpp
// Avoid
if (user_authenticated) {
    if (has_permission) {
        if (data_valid) {
            ProcessData();
        }
    }
}

// Prefer (early return)
if (!user_authenticated) { return; }
if (!has_permission) { return; }
if (!data_valid) { return; }
ProcessData();
```

### Code Reuse Strategy

Before writing new code:
1. **Search existing ERPL-OData code** for similar functionality
2. **Check DuckDB core utilities** (in `duckdb/src/`) for reusable patterns
3. **Browse other DuckDB extensions** (PostgreSQL Scanner, Parquet, JSON) for proven approaches
4. **Minimize code footprint** by extracting and sharing utilities

Common reusable components in this extension:
- `HttpClient` - All HTTP requests should use this base client
- `ODataContent` - Response parsing; reuse for new OData-like services
- `SecretProvider` - Authentication; extend for new secret types
- `ODataEntitySetResponse` - Response handling; adapt for similar APIs
- `ErplTracer` - Logging/tracing; use consistently across modules
- `charset_converter` - Character encoding; reuse for all text handling

### Static Analysis & Linting

The project uses `clang-tidy` for static analysis. Key checks enabled:
- **Performance:** Flag inefficient patterns (`performance-*`)
- **Modernization:** Prefer C++11+ idioms (`modernize-*`)
- **Core guidelines:** Follow C++ core guidelines (`cppcoreguidelines-*`)
- **Readability:** Enforce naming, braces, identifier conventions (`readability-*`)
- **Bug prevention:** Flag likely bugs (`bugprone-*`)

**How to run locally:**
```bash
cd /Users/jr/Projects/datazoo/erpl-odata
clang-tidy -p build/debug src/**/*.cpp
```

### Example: Well-Structured C++ Class

```cpp
#pragma once

#include <memory>
#include <vector>
#include "http_client.hpp"

namespace erpl_web {

// Forward declaration
class ODataMetadata;

// Abstract base for extensibility
class ODataResponseParser {
public:
    virtual ~ODataResponseParser() = default;
    virtual std::vector<std::vector<duckdb::Value>> Parse(
        const std::string& content,
        const std::vector<std::string>& column_names
    ) = 0;
};

// Concrete implementation
class JsonODataParser : public ODataResponseParser {
public:
    explicit JsonODataParser(std::shared_ptr<ODataMetadata> metadata);

    // Override keyword required; no need to repeat virtual
    std::vector<std::vector<duckdb::Value>> Parse(
        const std::string& content,
        const std::vector<std::string>& column_names
    ) override;

private:
    // Private methods for internal logic
    std::vector<duckdb::Value> ParseRow(const nlohmann::json& row);

    // Member variables: snake_case, private section at bottom
    std::shared_ptr<ODataMetadata> metadata_;
    const int max_rows_ = 10000;
};

} // namespace erpl_web
```

## GitHub Actions CI/CD & Multi-Platform Builds

### Overview

This extension uses **extension-ci-tools** reusable workflows for automated multi-platform builds and distribution. The CI system builds extension binaries for Linux, macOS, and Windows through GitHub Actions.

**Primary workflow:** `.github/workflows/MainDistributionPipeline.yml`
- Calls `duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml@main`
- Builds for DuckDB v1.4.5 and v1.5.6 (configured via `duckdb_version` parameter)
- Generates platform-specific binaries as GitHub Actions artifacts
- Deploys to S3 for distribution

### Platform Matrix & Architecture Support

The build matrix is defined in `extension-ci-tools/config/distribution_matrix.json`:

| Platform | Architecture | Runner | VCPKG Triplet | CI Mode | Notes |
|----------|-------------|--------|---------------|---------|-------|
| **Linux** | linux_amd64 | ubuntu-24.04 | x64-linux-release | Reduced | Primary development platform |
| Linux | linux_arm64 | ubuntu-24.04-arm | arm64-linux-release | Full | Cross-compile or native ARM |
| Linux | linux_amd64_musl | ubuntu-24.04 | x64-linux-release | Full | Alpine/musl-libc compatibility |
| **macOS** | osx_amd64 | macos-latest | x64-osx-release | Full | Intel Mac support |
| **macOS** | osx_arm64 | macos-latest | arm64-osx-release | Reduced | Apple Silicon (primary dev) |
| **Windows** | windows_amd64 | windows-latest | x64-windows-static-md-release-vs2019comp | Reduced | MSVC build (critical for distribution) |
| Windows | windows_amd64_mingw | windows-latest | x64-mingw-static | Reduced | MinGW GCC alternative |

**Current configuration excludes** (see `exclude_archs` in MainDistributionPipeline.yml):
- `windows_amd64_rtools`, `windows_amd64_mingw` - Excluded for faster builds
- `windows_amd64` - Excluded **on the v1.4.5 legs only**. That leg's pinned vcpkg commit
  acquires `msys2-runtime 3.5.4-2` to build openssl, and MSYS2 has dropped that build from
  every mirror (all six 404), so the leg fails before compiling anything and re-running
  cannot help. v1.5.6 still builds and ships `windows_amd64`. Restore it by bumping the
  v1.4.5 `vcpkg_commit`. See GitHub #174.
- `wasm_mvp`, `wasm_eh`, `wasm_threads` - WebAssembly variants excluded
- `linux_arm64`, `linux_amd64_musl` - **NOT excluded.** They are built on every leg. The
  `exclude_archs` lists in `MainDistributionPipeline.yml` name only the rtools/mingw/wasm
  variants, plus `windows_amd64` on v1.4.5. This bullet used to claim they were excluded
  "to reduce build time", which sent at least one session diagnosing an arm64 CI failure
  from a false premise. Read `exclude_archs` rather than this table when it matters.
  Known flake: the v1.4.5 `linux_arm64` leg intermittently fails in its container's first
  `yum install` when the AlmaLinux mirrors 404 on stale repodata, and fail-fast then
  cancels its two sibling Linux jobs - so one infrastructure failure shows as three red
  checks that have nothing to do with the change. Confirm from the log before touching
  code; re-running the failed jobs has cleared it. See GitHub #219.

### Platform-Specific Build Challenges

#### **Linux** (Primary Development Platform)
- **Build environment:** Docker containers with Ubuntu 24.04
- **Advantages:**
  - Fast builds on GitHub Actions
  - Consistent environment across runs
  - Native builds (no cross-compilation overhead)
  - ccache support for incremental builds
- **Challenges:**
  - Disk space management (uses `disk-cleanup` action)
  - Docker storage configuration for large builds
  - ARM64 builds require native ARM runners (expensive)
- **Dependencies:** VCPKG builds OpenSSL, tinyxml2, cpptrace from source

#### **macOS** (Primary Development Platform)
- **Build environment:** Native macOS runners (macos-latest)
- **Advantages:**
  - Native compilation for both x86_64 and ARM64
  - Homebrew integration for build tools (ninja, ccache)
  - Python and toolchain management via actions/setup-*
- **Challenges:**
  - **Cross-architecture builds:** osx_amd64 may build on ARM runners (slower)
  - **Code signing:** Extensions may require developer signatures
  - **Runner costs:** macOS runners are 10x more expensive than Linux
  - **VCPKG compatibility:** Some packages require macOS-specific patches
  - **Linker differences:** Mach-O linking vs ELF (Linux) or PE (Windows)
- **Performance:** Builds typically take 10-15 minutes per architecture

#### **Windows** (Critical for Distribution)
- **Build environment:** Native Windows runners with MSVC 2019/2022
- **Advantages:**
  - MSVC produces optimized Windows binaries
  - Native Windows API support
  - VS2019 compatibility triplet ensures broader Windows support
- **Challenges:**
  - **Toolchain complexity:** MSVC vs MinGW vs rtools configurations
  - **Path handling:** Windows paths, backslashes, and Unix-style Makefile incompatibilities
  - **Dependency builds:** VCPKG on Windows is slower (no Docker caching)
  - **Static linking:** Windows requires careful static linking to avoid DLL dependencies
  - **CMake generators:** Must use correct generator (Ninja, Visual Studio, or NMake)
  - **Line endings:** CRLF vs LF can cause issues with scripts
  - **Case sensitivity:** Windows filesystem is case-insensitive (can hide bugs)
- **Performance:** Windows builds are typically slowest (15-25 minutes)
- **Testing:** Tests may be skipped on Windows (see `SKIP_TESTS=1` in Makefile)

**Why Windows is critical for distribution:**
- Large user base requires native Windows binaries
- Users expect seamless `INSTALL erpl_odata FROM community` experience
- No cross-compilation from Linux/macOS (Windows requires native builds)
- MSVC compatibility ensures wide Windows version support (Windows 10+)

### Build Pipeline Stages

The CI workflow executes in these stages:

#### **1. Matrix Generation** (ubuntu-latest)
```
Parse distribution_matrix.json → Apply exclude_archs → Generate platform matrices
```
Produces separate matrices for Linux, macOS, Windows, and optionally WASM.

#### **2. Linux Build** (Docker-based)
```
Checkout repo → Setup Docker → Build in container → Test → Upload artifacts
```
- Uses ccache for compilation speed
- VCPKG dependencies cached in S3
- Produces `erpl_odata-v1.5.6-extension-linux_amd64.duckdb_extension`

#### **3. macOS Build** (Conditional on Linux success)
```
Setup Homebrew → Install ninja/ccache → Configure VCPKG → Build → Test → Upload
```
- Builds both x86_64 and ARM64 variants
- Uses native runners (no emulation)
- Produces `erpl_odata-v1.5.6-extension-osx_{amd64,arm64}.duckdb_extension`

#### **4. Windows Build** (Conditional on Linux success)
```
Setup MSVC → Configure VCPKG → Build with Ninja/MSBuild → Test → Upload
```
- Static linking to avoid DLL dependencies
- VS2019 compatibility mode for broader Windows support
- Produces `erpl_odata-v1.5.6-extension-windows_amd64.duckdb_extension`

#### **5. Deployment** (S3 Upload)
```
Download artifacts → Configure AWS credentials → Upload to S3 bucket
```
- Deploys to custom S3 bucket: `DEPLOY_S3_BUCKET` variable
- Two upload modes: `latest` (overwrite) and `versioned` (permanent)
- Only triggers on `main` branch or tagged releases

### Diagnosing Build Failures with GitHub CLI

**Always use GitHub CLI (`gh`) to diagnose CI failures:**

```bash
# List recent workflow runs
gh run list --limit 10

# View specific run details
gh run view <RUN_ID>

# View run with logs
gh run view <RUN_ID> --log

# Filter by workflow
gh run list --workflow="MainDistributionPipeline.yml"

# Watch a running workflow
gh run watch <RUN_ID>

# Download artifacts from a run
gh run download <RUN_ID>

# Re-run failed jobs
gh run rerun <RUN_ID> --failed

# View job-specific logs
gh run view <RUN_ID> --job=<JOB_ID> --log
```

**Common failure patterns:**

1. **Linux build fails:**
   ```bash
   gh run view --log | grep -A 20 "linux_amd64"
   ```
   - Check VCPKG dependency compilation errors
   - Verify disk space (Docker storage issues)
   - Inspect CMake configuration output

2. **macOS build fails:**
   ```bash
   gh run view --log | grep -A 20 "osx_"
   ```
   - Check Homebrew installation logs
   - Verify cross-architecture configuration
   - Inspect linker errors (Mach-O format issues)

3. **Windows build fails:**
   ```bash
   gh run view --log | grep -A 20 "windows_amd64"
   ```
   - Check MSVC toolchain setup
   - Verify VCPKG triplet configuration
   - Inspect path-related errors (Windows vs Unix paths)

4. **Deployment fails:**
   ```bash
   gh run view --log | grep -A 20 "Deploy"
   ```
   - Check AWS credentials configuration
   - Verify S3 bucket permissions
   - Inspect artifact download step

### Git Commit Management by Agents

**Important:** When making commits, follow these practices:

#### **Commit Message Format**
Use conventional commit style WITHOUT AI attribution:

```bash
# Feature additions
git commit -m "feat: add SAP Analytics Cloud model discovery"

# Bug fixes
git commit -m "fix: resolve OAuth2 token refresh on expired credentials"

# Performance improvements
git commit -m "perf: optimize OData metadata caching"

# Refactoring
git commit -m "refactor: extract HTTP client connection pooling logic"

# Documentation
git commit -m "docs: update build instructions for Windows"

# Tests
git commit -m "test: add coverage for Datasphere secret providers"

# CI/CD changes
git commit -m "ci: exclude ARM64 builds to reduce CI time"
```

**DO NOT include:**
- ❌ "Generated with Claude Code"
- ❌ "AI-assisted changes"
- ❌ "Co-Authored-By: Claude"
- ❌ Any references to AI or automated tools in commit messages

**Why?** Commit messages should describe what changed and why, not how the change was made. The use of AI tools is an implementation detail, not relevant to the git history.

#### **Commit Management Commands**

```bash
# Stage and commit changes
git add src/new_feature.cpp src/include/new_feature.hpp
git commit -m "feat: implement new feature for XYZ"

# Amend last commit (only if not pushed)
git commit --amend -m "feat: implement feature with corrected logic"

# Create feature branch
git checkout -b feature/sac-integration
git commit -m "feat: add SAC catalog functions"
git push -u origin feature/sac-integration

# Check commit status before pushing
gh pr checks  # If on PR branch
git log --oneline -5  # Verify commit messages

# Push to remote
git push origin main
# Or for feature branch
git push origin feature/sac-integration
```

#### **Pull Request Creation**

Use GitHub CLI for PR creation:

```bash
# Create PR from current branch
gh pr create --title "feat: add SAP Analytics Cloud integration" \
             --body "Implements SAC model discovery and data reading functions.\n\n- Add sac_list_models() function\n- Add sac_read_planning_data() function\n- Add OAuth2 secret support for SAC\n- Add comprehensive tests"

# Create PR as draft
gh pr create --draft --title "wip: SAC integration" --body "Work in progress"

# View PR status
gh pr status

# View PR checks
gh pr checks

# Merge PR (after approval)
gh pr merge --squash  # Squash commits
gh pr merge --merge   # Regular merge
```

### Monitoring CI/CD Health

**Proactive monitoring:**

```bash
# Check latest main branch build
gh run list --branch main --limit 1

# Monitor all active runs
gh run list --status in_progress

# Check for recent failures
gh run list --status failure --limit 5

# View build duration trends
gh run list --limit 20 | awk '{print $6, $7, $8}'
```

**Performance benchmarks:**
- **Linux builds:** 8-12 minutes (target)
- **macOS builds:** 10-15 minutes per arch
- **Windows builds:** 15-25 minutes
- **Total pipeline:** ~30-45 minutes (parallel execution)

### Troubleshooting Common CI Issues

#### **Build Timeout**
```bash
# Check which job timed out
gh run view <RUN_ID> --log | grep -i "timeout"

# Solutions:
# 1. Exclude slow architectures (ARM64, musl)
# 2. Use ccache effectively
# 3. Reduce VCPKG dependency count
# 4. Split into multiple workflows
```

#### **Dependency Compilation Failures**
```bash
# Check VCPKG logs
gh run view <RUN_ID> --log | grep -A 50 "vcpkg install"

# Solutions:
# 1. Pin vcpkg_commit to known-good version
# 2. Check dependency compatibility with platform
# 3. Add vcpkg overlay ports if needed
# 4. Verify triplet configuration matches platform
```

#### **Test Failures**
```bash
# View test output
gh run view <RUN_ID> --log | grep -A 100 "unittest"

# Solutions:
# 1. Run tests locally: make test_debug
# 2. Check for platform-specific assumptions
# 3. Verify test fixtures are cross-platform
# 4. Use SKIP_TESTS if platform unsupported
```

### CI/CD Best Practices

1. **Test locally before pushing:**
   ```bash
   GEN=ninja make debug && make test_debug
   ```

2. **Use feature branches for complex changes:**
   ```bash
   git checkout -b feature/new-integration
   # Make changes, test locally
   git push -u origin feature/new-integration
   gh pr create
   ```

3. **Monitor build status:**
   ```bash
   gh run watch  # Watch current run
   ```

4. **Download and inspect artifacts:**
   ```bash
   gh run download <RUN_ID>
   ls -lh *.duckdb_extension
   ```

5. **Review platform-specific failures immediately:**
   - Windows failures often indicate path or linking issues
   - macOS failures may indicate code signing or cross-arch problems
   - Linux failures are usually dependency or disk space issues

## Extension Distribution & Community Integration

This extension follows **DuckDB community extension best practices**:

### Distribution Models
1. **Community Extensions Repository** (recommended)
   - Submit to https://github.com/duckdb/community-extensions
   - Enables installation via `INSTALL erpl_odata FROM community`
   - Automatic CI/CD for all platforms
   - Recommended for long-term maintenance

2. **GitHub Releases**
   - Attach `.duckdb_extension` binaries to releases
   - Manual `INSTALL erpl_odata FROM 'file://path'`
   - Suitable for early-stage/experimental extensions

3. **Custom Distribution**
   - Host on custom server or CDN
   - Requires `allow_unsigned_extensions` setting
   - Full control but higher maintenance burden

### Building for Distribution

Release builds create distributable artifacts:
```bash
make release
# Produces: ./build/release/extension/erpl_odata/erpl_odata.duckdb_extension
```

**Distribution packages include:**
- Multiple platform binaries (linux_amd64, windows_amd64, osx_amd64, osx_arm64)
- Built via GitHub Actions or locally
- Tested across platforms before release

### CI/CD Best Practices from DuckDB Ecosystem

The extension-ci-tools repository provides centralized CI/CD:
- **Latest 2 DuckDB versions** actively supported
- **Multiple architecture builds:** linux_amd64, windows_amd64, osx_amd64, osx_arm64
- **Automatic updates** when DuckDB changes build system
- **Phased deprecation:** Old versions removed systematically

This project currently targets **DuckDB v1.4.5 and v1.5.6** (check `.github/workflows/` and `extension-ci-tools` branch).

## Codebase Architecture

### Extension Entry Point
- **src/erpl_odata_extension.cpp** - Main extension initialization via `DUCKDB_EXTENSION_ENTRY`
- Registers all SQL functions, secret types, and pragma settings
- Initializes telemetry (PostHog) and tracing systems
- Configures DuckDB-specific settings callbacks

### Core Module Organization

#### 1. **HTTP Layer** (`http_client.*`, `timeout_http_client.*`)
- Low-level HTTP request/response handling (libcurl-based)
- Character set detection and conversion via charset_converter
- Timeout management and keep-alive pooling
- Returns consistent HttpResponse objects with headers, status, content

#### 2. **OData Support** (multiple modules)
- **odata_client.cpp/hpp** - Base OData client with version detection (V2 vs V4)
- **odata_catalog.cpp/hpp** - Metadata retrieval and schema extraction
- **odata_content.cpp/hpp** - Parses JSON/XML responses into DuckDB rows
- **odata_edm.hpp** - EDM (Entity Data Model) type mapping to DuckDB types
- **odata_predicate_pushdown_helper.hpp** - Converts DuckDB filter expressions to OData `$filter`
- **odata_expand_parser.hpp** - Handles `$expand` navigation properties
- **odata_attach_functions.cpp** - Implements `ATTACH...TYPE odata` statements
- **odata_read_functions.cpp** - Implements `odata_read()` table function
- **odata_storage.cpp/hpp** - Persistent metadata storage for OData services
- **odata_url_helpers.hpp** - URL manipulation and OData query building

#### 3. **ODP/SAP Integration** (`odp_*` modules)
- **odp_client_integration.cpp** - Integration layer with SAP ODP (Operational Data Provisioning)
- **odp_subscription_repository/state_manager.cpp** - Manages ODP subscriptions and state
- **odp_odata_read_functions.cpp** - OData reading specifically for ODP sources
- **odp_http_request_factory.cpp** - Custom HTTP request handling for ODP endpoints
- **odp_pragma_functions.cpp** - Pragma commands for ODP management
- **odp_odata_read_bind_data.cpp** - Bind parameters and data handling
- **odp_request_orchestrator.hpp** - Coordinates complex ODP requests

#### 4. **Datasphere** (`datasphere_*` modules)
- **datasphere_client.cpp/hpp** - HTTP client for Datasphere APIs
- **datasphere_catalog.cpp/hpp** - Space/asset discovery and metadata
- **datasphere_read.cpp/hpp** - Data reading for relational and analytical assets
- **datasphere_secret.cpp/hpp** - Datasphere-specific secret handling
- **datasphere_types.hpp** - Type definitions for Datasphere responses
- **datasphere_local_server.hpp** - Local callback server for OAuth2 authorization_code flow

#### 5. **SAP Analytics Cloud** (`sac_*` modules)
- **sac_client.cpp/hpp** - HTTP client for SAC APIs
- **sac_catalog.cpp/hpp** - Model/story discovery and metadata
- **sac_read_functions.cpp** - Implements `sac_read_*` functions
- **sac_attach_functions.cpp** - Implements `ATTACH...TYPE sac` statements
- **sac_url_builder.hpp** - Constructs SAC endpoint URLs

#### 6. **Authentication & Secrets**
- **secret_functions.cpp/hpp** - DuckDB secret integration (CREATE SECRET)
- **oauth2_flow_v2.cpp/hpp** - OAuth2 authorization_code and client_credentials flows
- **oauth2_server.cpp/hpp** - Local HTTP server for OAuth2 redirect callback
- **oauth2_browser.cpp/hpp** - Browser launching for user login
- **oauth2_callback_handler.cpp/hpp** - Handles OAuth2 authorization code callback
- **oauth2_types.hpp** - OAuth2 token structures

#### 7. **HTTP Functions** (`web_functions.cpp/hpp`)
- Implements `http_get()`, `http_head()`, `http_post()`, `http_put()`, `http_patch()`, `http_delete()`
- Named parameters: `headers`, `content_type`, `accept`, `auth`, `auth_type`, `timeout`
- Authentication precedence: function `auth` parameter overrides registered secrets
- Returns table with columns: `method`, `status`, `url`, `headers`, `content_type`, `content`

#### 8. **Cross-Cutting Concerns**
- **tracing.hpp** - Comprehensive logging system with levels (TRACE, DEBUG, INFO, WARN, ERROR) and outputs (console/file/both)
- **telemetry.hpp** - PostHog telemetry collection (optional, respects `erpl_telemetry_enabled` setting)
- **duckdb_argument_helper.hpp** - Utility for binding function arguments from DuckDB expressions
- **odata_transaction_manager.cpp/hpp** - Transaction handling for OData operations

### Configuration & Extension Loading
- **extension_config.cmake** - Specifies extension loading, test loading, and linking behavior (DONT_LINK for release)
- **CMakeLists.txt** - C++ build configuration with OpenSSL, tinyxml2, cpptrace dependencies
- External dependencies managed via VCPKG

## Key Design Patterns

### 1. **Object Pooling for HTTP**
Timeout HTTP client maintains connection pools for efficient reuse across multiple requests.

### 2. **Metadata Caching**
OData metadata is fetched once and cached to avoid redundant requests. Version detection (V2 vs V4) is performed early and reused.

### 3. **Predicate Pushdown**
Filter expressions from SQL `WHERE` clauses are converted to OData query syntax (`$filter`, `$select`, `$top`, `$skip`) to minimize data transfer.

### 4. **Secret Provider Pattern**
Multiple secret providers (OAuth2, config file, file-based) allow flexible credential management. OAuth2 includes token refresh handling.

### 5. **Response Parsing Strategy**
HTTP responses are parsed into `ODataContent` objects that provide both raw content access and structured row conversion with type mapping.

## Common Development Tasks

### Adding a New HTTP Function
1. Define function signature in a new file (e.g., `new_function.cpp`)
2. Implement bind and execute phases using DuckDB's `BindInfo` and `ExecutionContext`
3. Register in `erpl_odata_extension.cpp` via `CreateScalarFunction()` or similar
4. Add SQL tests in `test/sql/web_core.test` or appropriate subdirectory
5. Ensure proper tracing via `ERPL_TRACE_*` macros
6. Follow C++ standards above (const correctness, smart pointers, naming conventions)

### Extending OData Support
1. Modify `odata_client.cpp` for new query generation (e.g., new `$expand` options)
2. Update `odata_edm.hpp` if new EDM types need mapping
3. Enhance `odata_content.cpp` for response parsing logic
4. Add tests in `test/sql/odata_*.test` files
5. Update predicate pushdown in `odata_predicate_pushdown_helper.hpp` if needed
6. Reuse existing HTTP client abstractions; don't reinvent request handling

### Adding a New SAP Integration
1. Create client module (e.g., `new_service_client.cpp/hpp`) following SAC/Datasphere patterns
2. Implement catalog discovery functions (e.g., `new_service_show_assets()`)
3. Implement data reading with pushdown support
4. Add secret/authentication handling if needed
5. Register functions in extension entry point
6. Create test file in `test/sql/` with appropriate fixtures
7. Reuse `HttpClient` and `SecretProvider` abstractions where possible

## Testing Considerations

- SQL tests use SQLLogicTest format; see `test/README.md`
- C++ unit tests are located in `build/debug/extension/erpl_odata/test/cpp/`
- Remote/API-dependent tests should mock responses or use test fixtures
- OAuth2 flows require local callback server; tests should verify this works in isolation
- SAP tests can use environment variables (e.g., `ERPL_SAP_BASE_URL`, `ERPL_SAP_PASSWORD`)

## Current Modified Files (from git status)
- `src/erpl_odata_extension.cpp` - May have recent extension initialization changes
- `src/include/sac_catalog.hpp` - SAC catalog interface updates
- `src/sac_catalog.cpp` - SAC catalog implementation

Recent work focused on fixing extension entry point initialization and removing invalid parameters.

## When Claude Code Gets Stuck: Consulting Codex

### Philosophy: AI Agent Collaboration

As an AI coding agent, you (Claude Code) have powerful capabilities but also limitations. When facing complex architectural decisions, ambiguous requirements, or deeply technical issues, **consult OpenAI Codex** for a second opinion.

This mirrors successful engineering practices: senior engineers consult each other when stuck. Two AI agents with different strengths can solve problems faster than either alone.

### When to Consult Codex

**Consult Codex when you encounter:**

1. **Architectural Ambiguity**
   - Multiple valid approaches with significant trade-offs
   - Design patterns that could affect long-term maintenance
   - Performance optimization requiring domain expertise
   - Example: "Should we use polling or WebSockets for OData change notifications?"

2. **Deep Technical Unknowns**
   - Behavior of external APIs not documented in training data
   - Platform-specific quirks (e.g., Windows VCPKG linking edge cases)
   - Complex C++ template metaprogramming issues
   - Example: "How does DuckDB's vector processing work with custom table functions?"

3. **Security or Correctness Critical Decisions**
   - OAuth2 flow implementation details
   - Thread safety in concurrent HTTP client
   - Memory management in complex object graphs
   - Example: "Is it safe to share HttpClient instances across threads?"

4. **Build System Configuration Issues**
   - VCPKG dependency resolution failures with unclear root cause
   - CMake configuration that works locally but fails in CI
   - Platform-specific compilation errors requiring deep toolchain knowledge
   - Example: "Why does OpenSSL link correctly on Linux but fail on Windows with MSVC?"

5. **Complex Debugging Scenarios**
   - Segmentation faults with unclear stack traces
   - Race conditions in multi-threaded code
   - Performance bottlenecks requiring profiling interpretation
   - Example: "Why does this code crash only on ARM64 but not x86_64?"

### How to Consult Codex

When stuck, prepare a context file and invoke Codex via bash:

#### **Step 1: Prepare Context File**

Create a consultation file in `.ai/codex_context/` with all relevant information:

```bash
# Create context directory if it doesn't exist
mkdir -p .ai/codex_context

# Write consultation request
cat > .ai/codex_context/$(date +%Y%m%d_%H%M%S)_connection_pooling.md << 'EOF'
# Codex Consultation: HTTP Connection Pooling Strategy

## Context
Working on ERPL-OData DuckDB extension implementing connection pooling for HttpClient
to improve performance when making multiple requests to the same host.

## Current Implementation
File: src/http_client.cpp
- Each http_get() call creates new TCP connection
- No connection reuse across requests
- Performance bottleneck for repeated API calls

## Problem
Not sure how to manage connection lifecycle in DuckDB's execution model.
DuckDB uses vectorized execution with multiple threads.

## Options Considered

### Option 1: Global Connection Pool
Single static pool shared across all threads
- Pros: Simple, efficient reuse
- Cons: Requires thread-safe synchronization, global state

### Option 2: Per-Thread Connection Pool
Thread-local storage for connections
- Pros: No synchronization overhead
- Cons: Less connection reuse, harder to implement cleanup

### Option 3: Per-Query Connection Pool
Pool lives in bind data
- Pros: Clear lifecycle, automatic cleanup
- Cons: No reuse across queries, defeats pooling purpose

## Question for Codex
Which approach aligns best with DuckDB's architecture? How do other DuckDB
extensions (e.g., postgres_scanner) handle persistent connections?

## Constraints
- Must be thread-safe (DuckDB uses parallel execution)
- Should follow RAII principles (no manual cleanup)
- Must not leak resources on query cancellation
- Should follow DuckDB extension best practices

## Relevant Code Snippets

### Current HttpClient (src/http_client.cpp)
```cpp
HttpResponse HttpClient::Get(const std::string& url) {
    // Creates new connection each time
    CURL* curl = curl_easy_init();
    // ... setup and execute
    curl_easy_cleanup(curl);
    return response;
}
```

### DuckDB Bind Data Pattern (example from postgres_scanner)
```cpp
struct PostgresBindData : public TableFunctionData {
    unique_ptr<PostgresConnection> connection;
    // Connection lives for query duration
};
```

## References
- src/http_client.cpp
- src/timeout_http_client.cpp
- src/web_functions.cpp
- DuckDB postgres_scanner: github.com/duckdb/postgres_scanner
EOF
```

#### **Step 2: Invoke Codex**

Use Codex CLI to get consultation:

```bash
# Basic consultation (interactive)
codex chat --file .ai/codex_context/20250129_143022_connection_pooling.md

# Non-interactive consultation (get direct answer)
codex exec --file .ai/codex_context/20250129_143022_connection_pooling.md \
           --prompt "Based on this context, recommend the best approach and explain why." \
           > .ai/codex_context/20250129_143022_connection_pooling_response.md

# View the response
cat .ai/codex_context/20250129_143022_connection_pooling_response.md
```

#### **Step 3: Implement Based on Consultation**

After receiving Codex's response:

1. **Document the consultation**
   ```cpp
   // Using per-query connection pool (Option 3) per Codex consultation
   // Date: 2025-01-29 14:30
   // Rationale: Aligns with DuckDB's bind data architecture, provides
   // automatic cleanup, and avoids global state issues.
   // See: .ai/codex_context/20250129_143022_connection_pooling_response.md
   ```

2. **Implement the solution**
   ```cpp
   struct HttpBindData : public TableFunctionData {
       shared_ptr<HttpConnectionPool> connection_pool;

       HttpBindData() {
           connection_pool = make_shared<HttpConnectionPool>();
       }
   };
   ```

3. **Reference in commit message**
   ```bash
   git commit -m "feat: implement per-query connection pooling

   Use bind data for connection lifecycle per Codex consultation.
   This aligns with DuckDB architecture and ensures proper cleanup.

   Consultation: .ai/codex_context/20250129_143022_connection_pooling.md"
   ```

### Automation: Codex Consultation Helper Script

Create a helper script for quick consultations:

```bash
cat > scripts/consult_codex.sh << 'EOF'
#!/bin/bash
# Helper script to consult Codex when stuck

CONTEXT_DIR=".ai/codex_context"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Ensure context directory exists
mkdir -p "$CONTEXT_DIR"

# Get consultation topic
TOPIC="$1"
if [ -z "$TOPIC" ]; then
    echo "Usage: $0 <topic_slug>"
    echo "Example: $0 connection_pooling"
    exit 1
fi

CONTEXT_FILE="$CONTEXT_DIR/${TIMESTAMP}_${TOPIC}.md"
RESPONSE_FILE="$CONTEXT_DIR/${TIMESTAMP}_${TOPIC}_response.md"

# Check if Codex is available
if ! command -v codex &> /dev/null; then
    echo "Error: Codex CLI not found. Install with: npm install -g @openai/codex"
    exit 1
fi

echo "Context file created: $CONTEXT_FILE"
echo "Please fill in the context file, then run:"
echo "  codex exec --file $CONTEXT_FILE --prompt 'Analyze and recommend' > $RESPONSE_FILE"
EOF

chmod +x scripts/consult_codex.sh
```

Usage:
```bash
# Create context file template
./scripts/consult_codex.sh thread_safety

# Edit the generated file with your question
vim .ai/codex_context/20250129_150000_thread_safety.md

# Get Codex response
codex exec --file .ai/codex_context/20250129_150000_thread_safety.md \
           --prompt "Analyze the problem and recommend the best solution with rationale." \
           > .ai/codex_context/20250129_150000_thread_safety_response.md
```

### Codex Context Template

Standard template for context files:

```markdown
# Codex Consultation: [TOPIC]

## Context
[Brief description of what you're working on]

## Current Implementation
[Current approach, relevant file paths]

## Problem
[What you're stuck on, what you've tried]

## Options Considered
1. **Option A**: [Description]
   - Pros: ...
   - Cons: ...

2. **Option B**: [Description]
   - Pros: ...
   - Cons: ...

## Question for Codex
[Specific question requiring architectural/technical guidance]

## Constraints
- [Constraint 1]
- [Constraint 2]
- Must follow DuckDB extension best practices
- Must follow C++ code standards in this repo

## Relevant Code Snippets
[Include 10-30 lines of most relevant code]

## References
- [File paths]
- [Documentation links]
- [Similar extensions or patterns]
```

### Red Flags: When NOT to Consult Codex

**Don't consult for:**
- ❌ Syntax errors (you can fix these)
- ❌ Simple refactoring tasks
- ❌ Straightforward bug fixes with clear root cause
- ❌ Adding tests for existing functionality
- ❌ Documentation updates
- ❌ Formatting or style issues
- ❌ Tasks that can be solved by reading existing code

**Do consult for:**
- ✅ "Should we...?" architectural questions
- ✅ "How does X work in DuckDB?" when docs are unclear
- ✅ "What's the best practice for...?" design patterns
- ✅ "Why does this fail only on Windows?" platform mysteries
- ✅ "Is this approach thread-safe?" concurrency concerns
- ✅ Complex performance optimization decisions

### Effective Consultation Protocol

1. **Attempt resolution first** (15-20 minutes)
   - Search codebase for similar patterns
   - Review DuckDB core and other extensions
   - Check documentation and GitHub issues
   - Try 2-3 different approaches

2. **Prepare comprehensive context**
   - Document what you've tried
   - Include error messages
   - Note partial successes
   - List constraints clearly

3. **Formulate specific question**
   - Avoid "How do I do X?"
   - Prefer "Given constraints A, B, C, should I do X or Y?"
   - Include code snippets (10-30 lines)

4. **Invoke Codex non-interactively**
   ```bash
   codex exec --file .ai/codex_context/TIMESTAMP_topic.md \
              --prompt "Recommend the best approach with detailed rationale" \
              > .ai/codex_context/TIMESTAMP_topic_response.md
   ```

5. **Review and implement**
   - Read Codex's response carefully
   - Validate against project constraints
   - Document the decision in code
   - Reference consultation in commit

### Consultation Log

Maintain a log in `.ai/codex_consultations.md`:

```markdown
# Codex Consultations Log

## 2025-01-29 14:30: Connection Pooling Strategy
**Question:** Per-thread vs per-query connection pooling for HttpClient
**Codex Recommendation:** Per-query (bind data approach)
**Rationale:** Aligns with DuckDB architecture, automatic cleanup, no global state
**Files Affected:** src/http_client.cpp, src/web_functions.cpp
**Context:** .ai/codex_context/20250129_143022_connection_pooling.md
**Response:** .ai/codex_context/20250129_143022_connection_pooling_response.md

## 2025-01-30 09:15: OAuth2 Token Refresh Thread Safety
**Question:** Synchronous vs asynchronous token refresh, mutex strategy
**Codex Recommendation:** Synchronous with std::mutex on token storage
**Rationale:** Simpler, matches DuckDB's synchronous execution model, avoids callback complexity
**Files Affected:** src/oauth2_flow_v2.cpp, src/datasphere_secret.cpp
**Context:** .ai/codex_context/20250130_091500_oauth_thread_safety.md
**Response:** .ai/codex_context/20250130_091500_oauth_thread_safety_response.md
```

### Success Metrics

Good Codex consultation practices lead to:
- ✅ Fewer architectural reworks
- ✅ More maintainable code
- ✅ Better alignment with DuckDB patterns
- ✅ Clearer documentation of design decisions
- ✅ Faster resolution of complex issues
- ✅ Knowledge capture in consultation logs

### Example: Full Consultation Workflow

```bash
# 1. You're stuck on thread safety in OAuth2 token refresh
mkdir -p .ai/codex_context

# 2. Create context file
cat > .ai/codex_context/20250130_091500_oauth_thread_safety.md << 'EOF'
# Codex Consultation: OAuth2 Token Refresh Thread Safety

## Context
Implementing OAuth2 token refresh in datasphere_secret.cpp. DuckDB can execute
queries in parallel across multiple threads, and multiple threads may call
functions that need to refresh expired OAuth2 tokens simultaneously.

## Problem
Race condition: Two threads detect expired token and both try to refresh,
causing duplicate refresh requests and potential token invalidation.

## Options Considered
1. **std::mutex around entire refresh** - Simple but blocks all threads
2. **atomic<bool> refreshing flag** - Non-blocking but complex state machine
3. **std::shared_mutex** - Read-write lock for token access

## Question for Codex
What's the safest and most idiomatic C++ pattern for thread-safe token refresh?

## Constraints
- Must be thread-safe across DuckDB worker threads
- Should minimize blocking time
- Must use C++17 (DuckDB requirement)
- No external synchronization libraries

## Code Snippet
```cpp
class OAuth2TokenStorage {
    std::string access_token;
    std::chrono::system_clock::time_point expiry;

    std::string GetToken() {
        if (IsExpired()) {
            RefreshToken(); // RACE CONDITION HERE
        }
        return access_token;
    }
};
```

## References
- src/oauth2_flow_v2.cpp
- src/datasphere_secret.cpp
EOF

# 3. Consult Codex
codex exec --file .ai/codex_context/20250130_091500_oauth_thread_safety.md \
           --prompt "Recommend the safest C++17 thread-safety pattern with example code" \
           > .ai/codex_context/20250130_091500_oauth_thread_safety_response.md

# 4. Review response
cat .ai/codex_context/20250130_091500_oauth_thread_safety_response.md

# 5. Implement based on recommendation
# (Codex likely recommends std::mutex with double-checked locking)

# 6. Log consultation
echo "## $(date +%Y-%m-%d\ %H:%M): OAuth2 Token Refresh Thread Safety" >> .ai/codex_consultations.md
echo "**Codex Recommendation:** std::mutex with double-checked locking" >> .ai/codex_consultations.md
echo "**Files Affected:** src/oauth2_flow_v2.cpp" >> .ai/codex_consultations.md
echo "" >> .ai/codex_consultations.md

# 7. Commit with reference
git add src/oauth2_flow_v2.cpp .ai/codex_context/20250130_091500_oauth_thread_safety*
git commit -m "feat: add thread-safe OAuth2 token refresh

Implement mutex-based synchronization per Codex consultation.
Prevents race conditions when multiple threads refresh tokens.

Consultation: .ai/codex_context/20250130_091500_oauth_thread_safety.md"
```

**Remember:** Consulting Codex when stuck is good engineering practice. Two AI perspectives are better than one, and documenting these consultations improves the project's knowledge base.

## Key References

- **README.md** - Complete feature documentation and SQL examples
- **docs/UPDATING.md** - Process for updating DuckDB version dependencies
- **.ai/README.md** - Extension analysis and GitHub Actions documentation
- **DuckDB Extension Template** - https://github.com/duckdb/extension-template
- **Extension CI Tools** - https://github.com/duckdb/extension-ci-tools (supports 2 latest DuckDB versions)
- **DuckDB CONTRIBUTING.md** - https://github.com/duckdb/duckdb/blob/main/CONTRIBUTING.md
- **Community Extensions List** - https://duckdb.org/community_extensions/list_of_extensions (reference implementations)
- **OpenAI Codex** - https://github.com/openai/codex (consult when stuck on complex problems)

## Provisioning an ODP OData service on the A4H sandbox

The ODP tests need a real ODP OData service. A stock ABAP system has none, and SAP ships
**no non-UI way** to create one: `CL_RSODP_ODATA_GENERATOR` only generates the MPC/DPC
classes; the model registration and hub activation live in `CL_RSODP_ODATA_ODP_OUT_GA`,
which is driven by the SEGW guided-activity UI.

`scripts/sap/zcl_odp_provision.abap` does the same three steps headlessly:

1. `CL_RSODP_ODATA_GENERATOR->GENERATE_CLASSES` — generate MPC/DPC
2. `/IWBEP/IF_REGISTER_MDL_SERVICE->REGISTER_SERVICE` — register model + service (backend)
3. `/IWFND/CL_MGW_ACTIVATION_API->ACTIVATE_SERVICE` — add to the Gateway hub and activate the ICF node

Run it with [erpl-adt](https://github.com/DataZooDE/erpl-adt):

```bash
export SAP_PASSWORD=...
erpl-adt object create --type CLAS/OC --name ZCL_ODP_PROVISION --package '$TMP' \
    --description 'ODP provisioning'
erpl-adt source write ZCL_ODP_PROVISION --type CLAS --file scripts/sap/zcl_odp_provision.abap
erpl-adt activate ZCL_ODP_PROVISION
erpl-adt object run ZCL_ODP_PROVISION      # -> generate rc=0 / register OK / activate OK
```

Then point the tests at it:

```bash
make test_debug_sap ERPL_SAP_ODP_SERVICE=Z_ODP_FCT_SRV \
                    ERPL_SAP_ODP_ENTITY_SET=FactsOfZJRODPVSQL \
                    ERPL_SAP_ODP_DELTA_SERVICE=Z_ODP_DL2_SRV \
                    ERPL_SAP_ODP_DELTA_ENTITY_SET=FactsOfZJRODPVSQL
```

The two ODP gates are deliberately separate. `ERPL_SAP_ODP_SERVICE` only needs *an* ODP
service; `ERPL_SAP_ODP_DELTA_SERVICE` must name one whose CDS view is delta-capable, which
is a stricter requirement than it looks — see the next bullet.

**Things that cost time the first time:**

- The ODP name comes from the CDS view's **SQL view name**, not the CDS name, plus a
  category suffix: `$F` for `@Analytics.dataCategory: #CUBE`, `$P` for `#DIMENSION`,
  `$T` for `#TEXT`. List them with
  `SELECT viewname, odpname FROM rsodpabapcdsextb( p_langu = @sy-langu )` — note that view
  takes a **parameter**, and forgetting it fails activation with the unhelpful
  *"The parameter P_LANGU was not bound"*.
- `@Analytics.dataExtraction.delta.byElement` does **not** make an extractor
  delta-capable. It activates cleanly and reads as if it should work, but
  `RODPS_REPL_ODP_GET_DETAIL` still reports `e_supports_delta = ' '`, so the generated
  service exposes no `DeltaLinksOf<EntitySet>` entity set and no delta link is ever
  returned. Use `@Analytics.dataExtraction.delta.changeDataCapture.automatic: true`
  instead; `scripts/sap/zjr_odp_v.ddls.abap` is the working fixture. Check which you have
  with `RODPS_REPL_ODP_GET_DETAIL` before concluding the reader is at fault.
- `TerminateDeltasFor<Entity>` flips `SubscribedFlag` to false but does **not** issue a
  delta link. To recover a token for an already-subscribed queue, read
  `DeltaLinksOf<EntitySet>` instead (GitHub #169).
- Everything must live in `$TMP`. The `@Analytics.dataExtraction.*` annotations and the
  classic ODP APIs are not released for ABAP Cloud, so a HOME-tier package rejects them
  on an ABAP Cloud developer trial.
- A source CDS view is provided as `scripts/sap/zjr_odp_v.ddls.abap` over the table in
  `scripts/sap/zjr_odp_data.tabl.abap`, for a fixture whose contents you control.
- **Delta is a separate matter, but it does work here.** `CL_RSODP_ODATA_ODP_OUT_MPC`
  guards the `DeltaLinksOf*` entity set with `CHECK i_supports_delta = ...`, so everything
  depends on what `RODPS_REPL_ODP_GET_DETAIL` reports. With
  `@Analytics.dataExtraction.delta.byElement` it reports `supports_delta = ' '` and the
  reader correctly stays in initial-load mode, logging *"change tracking not
  established"* — which reads like a reader defect and is not one. Switching the CDS view
  to `delta.changeDataCapture.automatic` flips it, and the full lifecycle then works
  end to end; `test/sql/sap/odp_odata_delta.test` proves it against `Z_ODP_DL2_SRV`.

