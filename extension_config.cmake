# Keep release SQL unittest statically linked so `require erpl_odata` runs the
# suite. The loadable .duckdb_extension is still built and smoke tested by CI.
duckdb_extension_load(erpl_odata
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS)

duckdb_extension_load(json)
