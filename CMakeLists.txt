cmake_minimum_required(VERSION 3.5...3.29)

# NOTE: do NOT redeclare BUILD_UNITTESTS here. DuckDB core owns this flag
# (duckdb/CMakeLists.txt) and deliberately sets it FALSE for library / extension-config
# builds. Redeclaring it forced our dev-only C++ test target onto consumers who statically
# link erpl_odata (GitHub #45), where the duckdb submodule the tests include from is absent.
# We honor DuckDB's value at the bottom of this file instead.

# Set extension name here
set(TARGET_NAME erpl_odata)

find_package(OpenSSL REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_package(cpptrace CONFIG REQUIRED)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) #Optional
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
#set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-deprecated-declarations)
endif()

if(MSVC)
    add_compile_options(/wd4996)
    # Disable min/max macros to prevent conflicts with std::min/std::max
    add_compile_definitions(NOMINMAX)
endif()

# Detect DuckDB v1.5+ API availability (ExtensionCallbackManager replaces DBConfig::storage_extensions)
if(EXISTS "${CMAKE_SOURCE_DIR}/src/include/duckdb/main/extension_callback_manager.hpp")
    add_compile_definitions(DUCKDB_HAS_EXTENSION_CALLBACK_MANAGER=1)
endif()

# Try to find mold binary
include(CheckCXXCompilerFlag)
find_program(MOLD_LINKER mold)

if(MOLD_LINKER)
    message(STATUS "Found mold: ${MOLD_LINKER}")
    
    # Check if compiler supports -fuse-ld=mold
    check_cxx_compiler_flag("-fuse-ld=mold" HAS_FUSE_LD_MOLD)
    if(HAS_FUSE_LD_MOLD)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=mold")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=mold")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fuse-ld=mold")
        message(STATUS "Using mold as linker with -fuse-ld=mold")
    else()
        message(WARNING "Compiler does not support -fuse-ld=mold")
    endif()
else()
    message(STATUS "mold linker not found, using default linker")
endif()


project(${TARGET_NAME})
include_directories(
    src/include
    ./duckdb/src/include
    ./duckdb/third_party/
    ./duckdb/third_party/httplib
    posthog-telemetry/include
)

# Add PostHog telemetry library
add_subdirectory(posthog-telemetry)

# Feedback banner (header-only). Telemetry ON so a printed banner counts as a
# banner_shown event through the instance configured above.
set(DATAZOO_BANNER_TELEMETRY ON CACHE BOOL "" FORCE)
set(DATAZOO_BANNER_TELEMETRY_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include CACHE PATH "" FORCE)
add_subdirectory(datazoo-banner)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/datazoo-banner/include)

# Ensure posthog_telemetry has access to OpenSSL headers (required for httplib SSL support)
target_link_libraries(posthog_telemetry PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# ---------------------------------------------------------------------------
# datazoo-oauth2 -- the shared OAuth2 stack. Extracted FROM this repo, and now
# consumed back from it (REQ-A-01/A-02; design in duckdb-gdrive's docs/hld.md
# section 5.4). The copies that used to live in src/ are deleted; this is the
# single implementation both erpl-odata and duckdb-gdrive build against.
#
# Pinned by commit, not tracked by branch: each consumer pins its own, so
# neither is forced to upgrade on the other's schedule (R-9).
#
# DATAZOO_OAUTH2_USE_HOST_TRACING: this repo has a real ErplTracer singleton
# and its own ERPL_TRACE_* macros. Without this the library's shim defines
# both again, and every translation unit that sees both headers fails with a
# macro redefinition and a type conflict.
# ---------------------------------------------------------------------------
add_subdirectory(datazoo-oauth2)
target_compile_definitions(datazoo_oauth2 PUBLIC DATAZOO_OAUTH2_USE_HOST_TRACING)
target_include_directories(datazoo_oauth2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/include)

set(EXTENSION_SOURCES
    src/tracing.cpp
    src/conversion_failure_log.cpp
    src/duckdb_argument_helper.cpp
    src/odata_attach_functions.cpp
    src/odata_catalog.cpp
    src/odata_client.cpp
    src/odata_content.cpp
    src/odata_edm.cpp
    src/odata_predicate_pushdown_helper.cpp
    src/odata_expand_parser.cpp
    src/odata_data_extractor.cpp
    src/odata_describe_functions.cpp
    src/odata_read_functions.cpp
    src/odata_read_bind_data.cpp
    src/odata_read_scan_state.cpp
    src/odata_read_pushdown.cpp
    src/odata_type_resolver.cpp
    src/odata_error_mapping.cpp
    src/odata_storage.cpp
    src/odata_transaction_manager.cpp
    src/secret_functions.cpp
    src/web_functions.cpp
    src/erpl_odata_extension.cpp
    src/datasphere_client.cpp
    src/datasphere_catalog.cpp
    src/datasphere_read.cpp
    src/datasphere_types.cpp
    src/datasphere_local_server.cpp
    src/odata_odp_functions.cpp
    src/datasphere_secret.cpp
    src/odata_url_helpers.cpp
    src/parameter_validation.cpp
    src/error_context.cpp
    src/url_parser.cpp
    src/pattern_matcher.cpp
    src/odp_http_request_factory.cpp
    src/odp_client_integration.cpp
    src/odp_subscription_repository.cpp
    src/odp_subscription_state_manager.cpp
    src/odp_request_orchestrator.cpp
    src/odp_odata_read_bind_data.cpp
    src/odp_odata_read_functions.cpp
    src/odp_pragma_functions.cpp
    src/sac_url_builder.cpp
    src/sac_client.cpp
    src/sac_catalog.cpp
    src/sac_catalog_bind_helper.cpp
    src/sac_attach_functions.cpp
    src/sac_secret_helper.cpp
    src/sac_secret.cpp
    src/sac_http_pool.cpp
    src/sac_read_functions.cpp
    src/delta_share_client.cpp
    src/delta_share_scan.cpp
    src/delta_share_catalog.cpp
    src/delta_share_storage.cpp
    src/microsoft_entra_secret.cpp
    src/business_central_secret.cpp
    src/business_central_client.cpp
    src/business_central_functions.cpp
    src/business_central_catalog.cpp
    src/business_central_storage.cpp
    src/dataverse_secret.cpp
    src/dataverse_client.cpp
    src/dataverse_functions.cpp
    src/graph_client.cpp
    src/graph_excel_secret.cpp
    src/graph_excel_client.cpp
    src/graph_excel_functions.cpp
    src/graph_excel_copy.cpp
    src/graph_sharepoint_client.cpp
    src/graph_sharepoint_functions.cpp
    src/graph_sharepoint_catalog.cpp
    src/graph_sharepoint_storage.cpp
    src/graph_sharepoint_copy.cpp
    src/graph_excel_catalog.cpp
    src/graph_excel_storage.cpp
    src/graph_planner_client.cpp
    src/graph_planner_functions.cpp
    src/graph_outlook_client.cpp
    src/graph_outlook_functions.cpp
    src/graph_entra_client.cpp
    src/graph_entra_functions.cpp
    src/graph_teams_client.cpp
    src/graph_teams_functions.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto tinyxml2::tinyxml2 cpptrace::cpptrace posthog_telemetry datazoo_oauth2)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto tinyxml2::tinyxml2 cpptrace::cpptrace posthog_telemetry datazoo_oauth2)

# Enable C++17
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if (WIN32)
      target_link_libraries(${EXTENSION_NAME} iphlpapi)
      target_link_libraries(${LOADABLE_EXTENSION_NAME} iphlpapi)
endif()

if(MINGW)
   find_package(ZLIB)
   target_link_libraries(${EXTENSION_NAME} ZLIB::ZLIB -lcrypt32)
   target_link_libraries(${LOADABLE_EXTENSION_NAME} ZLIB::ZLIB -lcrypt32)
endif()

if(APPLE)
   find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
   find_library(APPLICATIONSERVICES_FRAMEWORK ApplicationServices)
   target_link_libraries(${EXTENSION_NAME} ${COREFOUNDATION_FRAMEWORK} ${APPLICATIONSERVICES_FRAMEWORK})
   target_link_libraries(${LOADABLE_EXTENSION_NAME} ${COREFOUNDATION_FRAMEWORK} ${APPLICATIONSERVICES_FRAMEWORK})
endif()

# C++ unit tests are a developer-only artifact. They are only buildable from our own
# source tree, where DuckDB is checked out as the `duckdb` git submodule: test/cpp
# includes catch.hpp / core_functions_extension.hpp via paths relative to that submodule
# (../../duckdb/...). When erpl_odata is consumed as a dependency (e.g. via
# duckdb_extension_load / FetchContent), only the extension source is fetched — the
# duckdb submodule is absent — so those headers cannot be found and the test target
# fails to compile (GitHub #45). BUILD_UNITTESTS does NOT distinguish the two cases: it
# is TRUE in both our build and a consumer's main DuckDB build. Gate on the submodule's
# presence (same if(EXISTS ...) idiom used for extension_callback_manager.hpp above),
# while still honoring BUILD_UNITTESTS so developers can opt out.
if(BUILD_UNITTESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/catch/catch.hpp")
    add_subdirectory(test)
else()
    message(STATUS "erpl_odata: skipping C++ unit tests (BUILD_UNITTESTS off, or duckdb test "
                   "sources not present in-tree — e.g. erpl_odata is consumed as a dependency; GitHub #45)")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
