#pragma once
#include "duckdb_compat.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "yyjson.hpp"

// Typed FlatVector write helpers used by all graph_* scan functions.
// Replaces output.SetValue(col, row, Value(...)) — avoids generic Value boxing/unboxing.
// All helpers write to an already-flat Vector; validity defaults to all-valid so only
// nulls need explicit SetInvalid.

namespace erpl_web {

using namespace duckdb_yyjson;

inline void SetStrCell(duckdb::Vector &vec, duckdb::idx_t row, yyjson_val *val) {
    if (val && yyjson_is_str(val)) {
        duckdb_compat::FlatData<duckdb::string_t>(vec)[row] =
            duckdb::StringVector::AddString(vec, yyjson_get_str(val));
    } else {
        duckdb_compat::MutableValidity(vec).SetInvalid(row);
    }
}

inline void SetStrCellNN(duckdb::Vector &vec, duckdb::idx_t row, const char *str) {
    duckdb_compat::FlatData<duckdb::string_t>(vec)[row] =
        duckdb::StringVector::AddString(vec, str);
}

inline void SetBoolCell(duckdb::Vector &vec, duckdb::idx_t row, yyjson_val *val) {
    if (val && yyjson_is_bool(val)) {
        duckdb_compat::FlatData<bool>(vec)[row] = yyjson_get_bool(val);
    } else {
        duckdb_compat::MutableValidity(vec).SetInvalid(row);
    }
}

inline void SetBoolCellNN(duckdb::Vector &vec, duckdb::idx_t row, bool b) {
    duckdb_compat::FlatData<bool>(vec)[row] = b;
}

inline void SetInt64Cell(duckdb::Vector &vec, duckdb::idx_t row, yyjson_val *val) {
    if (val && yyjson_is_num(val)) {
        duckdb_compat::FlatData<int64_t>(vec)[row] = yyjson_get_sint(val);
    } else {
        duckdb_compat::MutableValidity(vec).SetInvalid(row);
    }
}

inline void SetInt32Cell(duckdb::Vector &vec, duckdb::idx_t row, yyjson_val *val) {
    if (val && yyjson_is_num(val)) {
        duckdb_compat::FlatData<int32_t>(vec)[row] = yyjson_get_int(val);
    } else {
        duckdb_compat::MutableValidity(vec).SetInvalid(row);
    }
}

} // namespace erpl_web
