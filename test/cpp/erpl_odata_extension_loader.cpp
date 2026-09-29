//
// Custom extension loader for C++ unit tests.
// Loads erpl_odata and core DuckDB extensions needed for SQL functions (now(), etc.)
//
#include "duckdb/main/extension/generated_extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "erpl_odata_extension.hpp"
#include "core_functions_extension.hpp"
#include "json_extension.hpp"
#include "parquet_extension.hpp"

namespace duckdb {

ExtensionLoadResult ExtensionHelper::LoadExtension(DuckDB &db, const std::string &extension) {
    if (extension == "erpl_odata") {
        db.LoadStaticExtension<ErplOdataExtension>();
        return ExtensionLoadResult::LOADED_EXTENSION;
    }
    if (extension == "core_functions") {
        db.LoadStaticExtension<CoreFunctionsExtension>();
        return ExtensionLoadResult::LOADED_EXTENSION;
    }
    if (extension == "json") {
        db.LoadStaticExtension<JsonExtension>();
        return ExtensionLoadResult::LOADED_EXTENSION;
    }
    if (extension == "parquet") {
        db.LoadStaticExtension<ParquetExtension>();
        return ExtensionLoadResult::LOADED_EXTENSION;
    }
    return ExtensionLoadResult::NOT_LOADED;
}

vector<string> LinkedExtensions() {
    return {"erpl_odata", "core_functions", "json", "parquet"};
}

void ExtensionHelper::LoadAllExtensions(DuckDB &db) {
    for (auto &ext_name : LinkedExtensions()) {
        LoadExtension(db, ext_name);
    }
}

#ifdef DUCKDB_HAS_EXTENSION_CALLBACK_MANAGER
// Added in DuckDB v1.5.0
vector<string> ExtensionHelper::LoadedExtensionTestPaths() {
    return {};
}
#endif

} // namespace duckdb
