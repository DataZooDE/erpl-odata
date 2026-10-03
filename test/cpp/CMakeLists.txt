find_package(OpenSSL REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)

include_directories(./include
                    ../../src/include
                    ../../duckdb/third_party/catch/
                    ../../duckdb/third_party/httplib/
                    ../../duckdb/test/include
                    ../../duckdb/extension/json/include
                    ../../duckdb/extension/json/yyjson/include
                    ../../duckdb/extension/core_functions/include
                    ../../duckdb/extension/parquet/include
                    ${OPENSSL_INCLUDE_DIR})

# On DuckDB v1.5+, LoadExtension/LoadAllExtensions are stubs in libduckdb_static.a
# that return NOT_LOADED for all extensions. We provide a custom loader that maps
# extension names to statically-linked Load functions.
# On DuckDB v1.4.x, these functions are already fully implemented in libduckdb_static.a
# using the generated loader; including our file would cause duplicate symbol errors.
set(TEST_SOURCES "")
if(EXISTS "${CMAKE_SOURCE_DIR}/src/include/duckdb/main/extension_callback_manager.hpp")
    list(APPEND TEST_SOURCES erpl_odata_extension_loader.cpp)
endif()

# Test sources are globbed rather than listed by hand. A hand-maintained list silently
# drops any file someone forgets to add: three test files (test_odata_expand_parser,
# test_odata_read_expand, test_odata_predicate_pushdown_expand) sat unregistered, and so
# uncompiled and unrun, for their whole lifetime - one of them with nine already-failing
# assertions nobody could see. CONFIGURE_DEPENDS makes the build re-glob when the set of
# matching files changes. See GitHub #115.
file(GLOB ERPL_ODATA_TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# The harness support file is not a test_*.cpp, so it is named explicitly.
list(APPEND TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/odata_test_server.cpp)

if(NOT ERPL_ODATA_TEST_FILES)
    message(FATAL_ERROR "No test_*.cpp files found in ${CMAKE_CURRENT_SOURCE_DIR} - the test glob is broken")
endif()

list(APPEND TEST_SOURCES ${ERPL_ODATA_TEST_FILES})

add_executable(erpl_odata_tests ${TEST_SOURCES})
if(EXISTS "${CMAKE_SOURCE_DIR}/src/include/duckdb/main/extension_callback_manager.hpp")
    # DuckDB v1.5+: link extension libraries explicitly (custom loader calls their Load fns)
    target_link_libraries(erpl_odata_tests
                          ${EXTENSION_NAME}
                          test_helpers
                          core_functions_extension
                          json_extension
                          parquet_extension
                          OpenSSL::SSL OpenSSL::Crypto
                          tinyxml2::tinyxml2)
else()
    # DuckDB v1.4.x: generated loader handles extensions; no explicit extension libs needed
    target_link_libraries(erpl_odata_tests
                          ${EXTENSION_NAME}
                          test_helpers
                          OpenSSL::SSL OpenSSL::Crypto
                          tinyxml2::tinyxml2)
endif()

# Register with CTest and inject sanitizer env overrides
include(CTest)
if (BUILD_TESTING)
    add_test(NAME erpl_odata_tests COMMAND erpl_odata_tests)
    set_property(TEST erpl_odata_tests PROPERTY ENVIRONMENT
        "ASAN_OPTIONS=detect_odr_violation=0:detect_leaks=0:halt_on_error=0:abort_on_error=0;UBSAN_OPTIONS=halt_on_error=0:print_stacktrace=0;ERPL_DISABLE_TELEMETRY=1")
endif()

# Enable C++17
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if (WIN32)
    target_link_libraries(erpl_odata_tests iphlpapi)
endif()

if(MINGW)
    find_package(ZLIB)
    target_link_libraries(erpl_odata_tests ZLIB::ZLIB -lcrypt32)
endif()
