#pragma once

#include "duckdb/function/table_function.hpp"

namespace erpl_web {

// Registration functions (called from erpl_odata_extension.cpp)
duckdb::TableFunctionSet CreateDeltaShareShowSharesFunction();
duckdb::TableFunctionSet CreateDeltaShareShowSchemasFunction();
duckdb::TableFunctionSet CreateDeltaShareShowTablesFunction();

} // namespace erpl_web
