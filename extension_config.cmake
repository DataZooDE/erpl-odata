if(CMAKE_BUILD_TYPE STREQUAL "Release")
    duckdb_extension_load(erpl_odata
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS
        DONT_LINK)
else()
    duckdb_extension_load(erpl_odata
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS)
endif()

duckdb_extension_load(json)