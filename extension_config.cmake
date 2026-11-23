# Keep release SQL unittest statically linked so `require erpl_odata` runs the
# suite. The loadable .duckdb_extension is still built and smoke tested by CI.

# GCC 14+ (the ci-tools Linux images) emits STB_GNU_UNIQUE definitions for ODR-used
# DuckDB class statics (e.g. BufferedFileWriter::DEFAULT_OPEN_FLAGS), which collide with
# core's strong definitions when DuckDB's own tools/plan_serializer links ("multiple
# definition"). Apply -fno-gnu-unique globally on GCC so they become weak COMDAT
# definitions. It lives here, not in MainDistributionPipeline.yml's extra_extension_config,
# so builders that don't pass one, notably duckdb/community-extensions, get it too.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-gnu-unique")
endif()

# DuckDB v1.5.6 stamps the short commit SHA instead of the release tag: its duckdb_extension_generate_version
# passes --match '${VERSIONING_TAG_MATCH}' with literal single quotes, so no tag ever matches. Pass the version
# explicitly when HEAD is on a vX.Y.Z tag (2.0.x detects it correctly too; this keeps both lines identical).
set(ERPL_ODATA_EXT_VERSION "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "v*.*.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE ERPL_ODATA_GIT_RESULT
        OUTPUT_VARIABLE ERPL_ODATA_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ERPL_ODATA_GIT_RESULT EQUAL 0 AND ERPL_ODATA_GIT_DESCRIBE MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[a-zA-Z0-9\\.]+)?$")
        set(ERPL_ODATA_EXT_VERSION "${ERPL_ODATA_GIT_DESCRIBE}")
    endif()
endif()
if(ERPL_ODATA_EXT_VERSION STREQUAL "")
    duckdb_extension_load(erpl_odata
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS)
else()
    message(STATUS "erpl_odata extension version ${ERPL_ODATA_EXT_VERSION}")
    duckdb_extension_load(erpl_odata
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        EXTENSION_VERSION ${ERPL_ODATA_EXT_VERSION}
        LOAD_TESTS)
endif()

duckdb_extension_load(json)
