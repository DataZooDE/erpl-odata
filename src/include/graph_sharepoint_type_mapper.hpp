#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "yyjson.hpp"
#include <cstring>
#include <string>

namespace erpl_web {

using namespace duckdb_yyjson;

// Maps a Graph API column-definition object (from /lists/{id}/columns) to a DuckDB type.
// The Graph API uses presence of a type-facet key (e.g. "number", "dateTime") to signal type.
inline duckdb::LogicalType SharePointColumnToDuckDBType(yyjson_val *col_def) {
    if (!col_def) {
        return duckdb::LogicalType::VARCHAR;
    }
    if (yyjson_obj_get(col_def, "number")) {
        return duckdb::LogicalType::DOUBLE;
    }
    if (yyjson_obj_get(col_def, "currency")) {
        return duckdb::LogicalType::DOUBLE;
    }
    if (yyjson_obj_get(col_def, "boolean")) {
        return duckdb::LogicalType::BOOLEAN;
    }
    if (yyjson_obj_get(col_def, "dateTime")) {
        return duckdb::LogicalType::TIMESTAMP;
    }
    // text, note, choice, lookup, personOrGroup, calculated, hyperOrPicture, geolocation, …
    return duckdb::LogicalType::VARCHAR;
}

// Writes a single field value into a FlatVector at the given row, dispatching on the column type.
// Handles null/missing values by marking the validity mask invalid.
inline void WriteSharePointField(
    duckdb::Vector &vec,
    duckdb::idx_t row,
    yyjson_val *field_val,
    const duckdb::LogicalType &col_type)
{
    if (!field_val || yyjson_is_null(field_val)) {
        duckdb_compat::MutableValidity(vec).SetInvalid(row);
        return;
    }

    switch (col_type.id()) {

    case duckdb::LogicalTypeId::DOUBLE:
        if (yyjson_is_num(field_val)) {
            duckdb_compat::FlatData<double>(vec)[row] = yyjson_get_num(field_val);
        } else {
            duckdb_compat::MutableValidity(vec).SetInvalid(row);
        }
        break;

    case duckdb::LogicalTypeId::BOOLEAN:
        if (yyjson_is_bool(field_val)) {
            duckdb_compat::FlatData<bool>(vec)[row] = yyjson_get_bool(field_val);
        } else {
            duckdb_compat::MutableValidity(vec).SetInvalid(row);
        }
        break;

    case duckdb::LogicalTypeId::TIMESTAMP: {
        // SharePoint returns dateTime fields as ISO 8601 strings, e.g. "2026-05-04T00:00:00Z"
        if (!yyjson_is_str(field_val)) {
            duckdb_compat::MutableValidity(vec).SetInvalid(row);
            break;
        }
        const char *s = yyjson_get_str(field_val);
        const duckdb::idx_t s_len = std::strlen(s);
        duckdb::timestamp_t ts;
        // use_offset=true so the trailing 'Z' (UTC) is accepted
        auto rc = duckdb::Timestamp::TryConvertTimestamp(s, s_len, ts, true);
        if (rc == duckdb::TimestampCastResult::SUCCESS) {
            duckdb_compat::FlatData<duckdb::timestamp_t>(vec)[row] = ts;
        } else {
            duckdb_compat::MutableValidity(vec).SetInvalid(row);
        }
        break;
    }

    default: { // VARCHAR — handles text, note, choice, lookup, personOrGroup, …
        if (yyjson_is_str(field_val)) {
            duckdb_compat::FlatData<duckdb::string_t>(vec)[row] =
                duckdb::StringVector::AddString(vec, yyjson_get_str(field_val));
        } else if (yyjson_is_num(field_val)) {
            const std::string s = std::to_string(yyjson_get_num(field_val));
            duckdb_compat::FlatData<duckdb::string_t>(vec)[row] =
                duckdb::StringVector::AddString(vec, s);
        } else if (yyjson_is_bool(field_val)) {
            duckdb_compat::FlatData<duckdb::string_t>(vec)[row] =
                duckdb::StringVector::AddString(vec, yyjson_get_bool(field_val) ? "true" : "false");
        } else {
            // Complex value (array, object) — serialise as JSON string
            size_t json_len = 0;
            char *json_str = yyjson_val_write(field_val, 0, &json_len);
            if (json_str) {
                duckdb_compat::FlatData<duckdb::string_t>(vec)[row] =
                    duckdb::StringVector::AddString(vec, json_str, json_len);
                free(json_str);
            } else {
                duckdb_compat::MutableValidity(vec).SetInvalid(row);
            }
        }
        break;
    }

    } // switch
}

} // namespace erpl_web
