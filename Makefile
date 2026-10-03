PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=erpl_odata
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

VCPKG_TOOLCHAIN_PATH?=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

ifeq ($(OS),Windows_NT)
    SKIP_TESTS=1
endif

# Fast incremental build for local development.
# Skips cmake reconfiguration (and the vcpkg compiler-hash check that comes with it)
# when the build directory is already configured. Ninja detects CMakeLists.txt changes
# on its own and re-runs cmake automatically when needed.
# (Keyed on CMakeCache.txt, not build.ninja: the default Makefiles generator never writes build.ninja,
# which made every plain 'make dev' reconfigure.)
# Use 'make debug' to force a full reconfigure (e.g. after adding a new dependency).
# Requires the extension tests to be enabled (the default): it builds unittest and erpl_odata_tests.
#
# Builds the three binaries this workflow runs (the 'duckdb' shell, the SQLLogicTest runner
# and the C++ tests) instead of 'all'. 'all' also builds DuckDB-internal tools such as
# plan_serializer, whose link can fail on a newer toolchain (duplicate
# BufferedFileWriter::DEFAULT_OPEN_FLAGS with gcc 16). That failure aborts the build before
# the shell is relinked, leaving ./build/debug/duckdb stale and silently running old code.
.PHONY: dev
dev:
	@if [ ! -f build/debug/CMakeCache.txt ]; then \
		echo "Build directory not configured yet — running full cmake configure..."; \
		mkdir -p build/debug && \
		cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) $(VCPKG_MANIFEST_FLAGS) \
			-DCMAKE_BUILD_TYPE=Debug -S $(DUCKDB_SRCDIR) -B build/debug; \
	fi
	cmake --build build/debug --config Debug --parallel --target shell unittest erpl_odata_tests

# Relink ./build/debug/test/unittest, the SQLLogicTest runner.
#
# 'duckdb', 'unittest' and 'erpl_odata_tests' are separate ninja targets. Building one does
# NOT build the others, so a session that builds a single target after a source change
# leaves the other binaries stale - and a stale unittest runs your SQL tests against code
# from before the fix, which looks exactly like a real behavioural bug (GitHub #172).
# Every target that invokes the runner depends on this one.
.PHONY: unittest
unittest:
	cmake --build build/debug --config Debug --parallel --target unittest

release_win: ${EXTENSION_CONFIG_STEP}
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Release -S $(DUCKDB_SRCDIR) -B build/release
	cmake --build build/release --config Release --parallel

# Run the SAP integration tests against the local A4H sandbox.
#
# The ODP cases additionally need a provisioned ODP service, which a stock system does
# not have. Name it to opt in; leave the variables unset and those cases skip with a
# stated reason instead of failing:
#   ERPL_SAP_ODP_SERVICE     - e.g. Z_ODP_BW_1_SRV
#   ERPL_SAP_ODP_ENTITY_SET  - e.g. FactsOf0D_NW_C01
# The variables must be ABSENT, not empty, when there is no ODP service: sqllogictest's
# require-env skips only when getenv returns null, so exporting an empty value would run
# the ODP cases and fail them - exactly what the gate exists to prevent.
#
# Delta is a separate gate again, because being delta-capable is a property of the
# extractor rather than of ODP: only a CDS view carrying
# @Analytics.dataExtraction.delta.changeDataCapture.automatic makes the service expose a
# DeltaLinksOf entity set at all. A service named here must be one of those.
#   ERPL_SAP_ODP_DELTA_SERVICE     - e.g. Z_ODP_DL2_SRV
#   ERPL_SAP_ODP_DELTA_ENTITY_SET  - e.g. FactsOfZJRODPVSQL
# An empty variable inherited from the caller's environment must be removed, not just left
# out of the command line, or require-env still sees it as set.
ERPL_SAP_ODP_UNSET := $(foreach v,ERPL_SAP_ODP_SERVICE ERPL_SAP_ODP_ENTITY_SET ERPL_SAP_ODP_DELTA_SERVICE ERPL_SAP_ODP_DELTA_ENTITY_SET,$(if $($(v)),,-u $(v)))
ERPL_SAP_ODP_ENV := $(if $(ERPL_SAP_ODP_SERVICE),ERPL_SAP_ODP_SERVICE='$(ERPL_SAP_ODP_SERVICE)') \
                    $(if $(ERPL_SAP_ODP_ENTITY_SET),ERPL_SAP_ODP_ENTITY_SET='$(ERPL_SAP_ODP_ENTITY_SET)') \
                    $(if $(ERPL_SAP_ODP_DELTA_SERVICE),ERPL_SAP_ODP_DELTA_SERVICE='$(ERPL_SAP_ODP_DELTA_SERVICE)') \
                    $(if $(ERPL_SAP_ODP_DELTA_ENTITY_SET),ERPL_SAP_ODP_DELTA_ENTITY_SET='$(ERPL_SAP_ODP_DELTA_ENTITY_SET)')

test_debug_sap: unittest
	env $(ERPL_SAP_ODP_UNSET) ERPL_SAP_BASE_URL='http://localhost:50000' ERPL_SAP_PASSWORD='ABAPtr2023#00' \
		$(ERPL_SAP_ODP_ENV) ./build/debug/test/unittest "[sap]"

# Run Microsoft 365 / Graph API integration tests against a real tenant.
# Requires environment variables sourced from an Azure App Registration with
# application permissions granted and admin-consented. See .env.microsoft.
#   ERPL_MS_TENANT_ID           - Directory (tenant) ID
#   ERPL_MS_CLIENT_ID           - Application (client) ID
#   ERPL_MS_CLIENT_SECRET       - Client secret value (not the secret ID)
#   ERPL_MS_SHAREPOINT_SITE_ID  - SharePoint site composite ID for SP tests
#   ERPL_MS_SHAREPOINT_LIST_ID  - SharePoint list ID within that site for SP tests
#   ERPL_MS_PLANNER_GROUP_ID    - Microsoft 365 group ID for Planner tests
#   ERPL_MS_PLANNER_PLAN_ID     - Planner plan ID within that group
#   ERPL_MS_EXCEL_SITE_ID       - SharePoint site ID containing the Excel test file
#   ERPL_MS_EXCEL_DRIVE_ID      - Drive ID within that site (from graph_show_drives)
#   ERPL_MS_EXCEL_FILE_PATH     - Relative path to .xlsx file within the drive
# Usage:
#   source .env.microsoft && make test_debug_ms
test_debug_ms: unittest
	./build/debug/test/unittest "test/sql/graph_entra_integration.test"
	./build/debug/test/unittest "test/sql/graph_planner_integration.test"
	./build/debug/test/unittest "test/sql/graph_sharepoint_integration.test"
	./build/debug/test/unittest "test/sql/graph_excel_integration.test"
	./build/debug/test/unittest "test/sql/graph_outlook_integration.test"
	./build/debug/test/unittest "test/sql/graph_teams_integration.test"
	./build/debug/test/unittest "test/sql/graph_excel_error_integration.test"
	./build/debug/test/unittest "test/sql/graph_sharepoint_error_integration.test"
	./build/debug/test/unittest "test/sql/graph_sharepoint_typed_read_integration.test"

# The Microsoft integration tests that CREATE, UPDATE or DELETE data in the tenant. They are
# kept out of test_debug_ms so a read-only run never mutates anything; they additionally need
# the dedicated ERPL_MS_SHAREPOINT_TESTLIST_* / ERPL_MS_EXCEL_* targets to be sacrificial.
.PHONY: test_debug_ms_write
test_debug_ms_write: unittest
	./build/debug/test/unittest "test/sql/graph_excel_write_integration.test"
	./build/debug/test/unittest "test/sql/graph_sharepoint_write_integration.test"
	./build/debug/test/unittest "test/sql/graph_sharepoint_scalar_write_integration.test"
	./build/debug/test/unittest "test/sql/graph_sharepoint_attach_rw_integration.test"
	./build/debug/test/unittest "test/sql/graph_excel_workbook_attach_integration.test"
	./build/debug/test/unittest "test/sql/graph_excel_workbook_attach_name_integration.test"

# Run Business Central integration tests against a live BC environment.
# Requires environment variables sourced from an Azure App Registration registered in BC.
# See .env.business_central for the required variables:
#   ERPL_BC_TENANT_ID    - Azure Directory (tenant) ID
#   ERPL_BC_CLIENT_ID    - Application (client) ID
#   ERPL_BC_CLIENT_SECRET - Client secret value
#   ERPL_BC_ENVIRONMENT  - BC environment name (e.g. 'Production')
#   ERPL_BC_COMPANY_ID   - Company GUID to run entity-read tests against
# Usage:
#   source .env.business_central && make test_debug_bc
test_debug_bc: unittest
	./build/debug/test/unittest "test/sql/business_central_integration.test"

# Regression test for GitHub #45: when erpl_odata is consumed as a dependency (the duckdb
# submodule is absent from the fetched source), its dev-only C++ test target must NOT be
# built — otherwise test/cpp fails with "catch.hpp file not found". Pure CMake script,
# no toolchain/DuckDB configure required, so it runs identically on every platform.
.PHONY: test_build_guard
test_build_guard:
	cmake -DREPO_DIR='${PROJ_DIR}' -P test/cmake/test_cpp_tests_guard.cmake

# Build and run datazoo-oauth2's own Catch2 suite.
#
# That target is guarded on the submodule being the TOP-LEVEL cmake project, and erpl-odata
# add_subdirectory()s it - so the guard is false in every build this repo makes, and the
# suite was never compiled, let alone run. It is the only coverage of
# OAuth2CallbackHandler::ValidateState, the CSRF defence of the whole authorization_code
# flow, and of the PKCE/state token generator and the callback error page's HTML escaping.
#
# Configured standalone in its own build directory rather than folded into erpl_odata_tests:
# standalone is the configuration the guard already supports, so nothing in the submodule
# has to change. It needs Catch2 3 on the system; the target says so rather than silently
# doing nothing, which is how this went unnoticed in the first place. See GitHub #245.
OAUTH2_BUILD_DIR?=build/datazoo-oauth2-tests

test_oauth2:
	@cmake -S datazoo-oauth2 -B ${OAUTH2_BUILD_DIR} > /dev/null || 		(echo "ERROR: could not configure datazoo-oauth2 standalone (is Catch2 3 installed?)"; exit 1)
	@cmake --build ${OAUTH2_BUILD_DIR} --parallel 4
	@test -x ${OAUTH2_BUILD_DIR}/test/datazoo_oauth2_unit_tests || 		(echo "ERROR: datazoo_oauth2_unit_tests was not built. Catch2 3 was probably not found, which is exactly the silent skip this target exists to prevent."; exit 1)
	${OAUTH2_BUILD_DIR}/test/datazoo_oauth2_unit_tests

# Run C++ unit tests. ASAN_OPTIONS=detect_odr_violation=0 suppresses a false-positive ODR
# warning that arises from DuckDB being compiled into both the static test binary and
# libduckdb.so. The duplicate symbol (LOOKUP_TABLE in nested_to_varchar_cast.cpp) is
# benign—both copies are identical—but ASAN fires anyway due to debug linking strategy.
test_cpp: ${EXTENSION_CONFIG_STEP}
	cmake --build build/debug --config Debug --parallel --target erpl_odata_tests
	ASAN_OPTIONS=detect_odr_violation=0 ./build/debug/extension/erpl_odata/test/cpp/erpl_odata_tests

