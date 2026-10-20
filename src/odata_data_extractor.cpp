#include "odata_read_functions.hpp"
#include "duckdb_compat.hpp"
#include "odata_edm.hpp"
#include "yyjson.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"

#include "tracing.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace erpl_web {
namespace {

using JsonDoc = duckdb_yyjson::yyjson_doc;
using JsonValue = duckdb_yyjson::yyjson_val;

struct JsonDocHandle {
  explicit JsonDocHandle(JsonDoc *doc) : doc(doc) {}
  ~JsonDocHandle() {
    if (doc) {
      duckdb_yyjson::yyjson_doc_free(doc);
    }
  }

  JsonDocHandle(const JsonDocHandle &) = delete;
  JsonDocHandle &operator=(const JsonDocHandle &) = delete;

  JsonValue *Root() const {
    return doc ? duckdb_yyjson::yyjson_doc_get_root(doc) : nullptr;
  }

  explicit operator bool() const { return doc != nullptr; }

private:
  JsonDoc *doc;
};

// NumericRangeError comes from conversion_failure_log.hpp: a JSON number that
// cannot be represented in the DuckDB column type the EDM metadata demands.
// It is deliberately distinct from the soft conversion failures the fallback
// paths absorb - silently substituting a truncated or default value here would
// corrupt the user's data with no signal - so it is thrown through the parsing
// chain and recorded against the column by the caller.

static std::string JsonValueToString(JsonValue *value) {
  char *json_str = duckdb_yyjson::yyjson_val_write(value, 0, nullptr);
  if (!json_str) {
    return "";
  }

  std::string result(json_str);
  std::free(json_str);
  return result;
}

static bool TryParseODataV2DateLiteral(const std::string &date_literal,
                                       duckdb::Value &timestamp_value) {
  if (date_literal.size() < 8 || date_literal.rfind("/Date(", 0) != 0 ||
      date_literal.substr(date_literal.size() - 2) != ")/") {
    return false;
  }

  const auto inner = date_literal.substr(6, date_literal.size() - 8);
  const auto timezone_pos = inner.find_first_of("+-", 1);
  const auto millis =
      timezone_pos == std::string::npos ? inner : inner.substr(0, timezone_pos);

  try {
    const auto milliseconds = std::stoll(millis);
    timestamp_value = duckdb::Value::TIMESTAMP(
        duckdb::Timestamp::FromEpochSeconds(milliseconds / 1000));
    return true;
  } catch (...) {
    return false;
  }
}

static std::string NormalizeJsonFieldName(const std::string &field_name,
                                          bool remove_underscores = false) {
  std::string normalized;
  normalized.reserve(field_name.size());
  for (auto c : field_name) {
    if (remove_underscores && c == '_') {
      continue;
    }
    normalized.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return normalized;
}

static bool IsTruthyString(const std::string &value) {
  return value == "true" || value == "1" || value == "yes" || value == "on";
}

static bool IsFalsyString(const std::string &value) {
  return value == "false" || value == "0" || value == "no" || value == "off";
}

} // namespace

// ============================================================================
// ODataDataExtractor Implementation
// ============================================================================

ODataDataExtractor::ODataDataExtractor(
    std::shared_ptr<ODataEntitySetClient> odata_client)
    : odata_client(odata_client) {
    type_resolver = std::make_shared<ODataTypeResolver>(odata_client);
}

void ODataDataExtractor::SetExpandedDataSchema(
    const std::vector<std::string> &expand_paths) {
    expanded_data_schema.clear();
    expanded_data_types.clear();
    this->expand_paths.clear();
    expanded_data_cache.clear();
    
    // Determine proper types for expanded columns using EDM metadata
    expanded_data_types.reserve(expand_paths.size());
    
  for (const auto &expand_path : expand_paths) {
    duckdb::LogicalType column_type =
        type_resolver->ResolveNavigationPropertyType(expand_path);
        
        // Check if type resolution failed (likely V2 without proper metadata)
        if (column_type.id() == duckdb::LogicalTypeId::VARCHAR) {
            // For V2 expands, we know the structure: { "results": [...] }
            // Since we can't determine the exact entity structure from metadata,
      // we'll use LIST(VARCHAR) as a safe fallback that can hold the JSON
      // strings This allows the expand to work while preserving the data
      // structure
            column_type = duckdb::LogicalType::LIST(duckdb::LogicalTypeId::VARCHAR);
      ERPL_TRACE_INFO(
          "DATA_EXTRACTOR",
          "Using V2 fallback type LIST(VARCHAR) for expanded column '" +
              expand_path + "' (EDM metadata unavailable)");
    }

    // If there are nested paths such as "DefaultSystem/Services", and this is
    // the top-level (e.g., "DefaultSystem"), augment the STRUCT type to include
    // a field for each nested navigation as an embedded list/struct, without
    // creating new top-level columns.
    if (!nested_expand_paths.empty()) {
      // Collect immediate nested props for this top-level path
      std::vector<std::string> nested_for_top;
      for (const auto &nested : nested_expand_paths) {
        if (nested.find(expand_path + "/") == 0) {
          auto rest = nested.substr(expand_path.size() + 1);
          auto slash = rest.find('/');
          std::string child =
              (slash == std::string::npos) ? rest : rest.substr(0, slash);
          if (!child.empty())
            nested_for_top.push_back(child);
        }
      }
      if (!nested_for_top.empty()) {
        // Delegate to EDM type builder for augmentation
        try {
          auto edmx_opt = EdmCache::GetInstance().Get(
              odata_client->GetMetadataContextUrl());
          if (edmx_opt) {
            ODataEdmTypeBuilder builder(*edmx_opt.get());
            // Resolve root entity type name from client
            auto root_entity = odata_client->GetCurrentEntityType().name;
            auto built = builder.BuildExpandedColumnType(
                root_entity, expand_path, nested_for_top);
            if (built.id() != duckdb::LogicalTypeId::INVALID) {
              column_type = built;
            }
          }
        } catch (...) {
          // keep previous column_type on failure
        }
      }
        }
        
        expanded_data_schema.push_back(expand_path);
        expanded_data_types.push_back(column_type);
        this->expand_paths.push_back(expand_path);
        
    ERPL_TRACE_INFO("DATA_EXTRACTOR", "Resolved expanded column type for '" +
                                          expand_path +
                                          "': " + column_type.ToString());
  }

  ERPL_TRACE_INFO("DATA_EXTRACTOR",
                  "Set expanded data schema with " +
                      std::to_string(expand_paths.size()) +
                      " navigation properties and proper types");
}

void ODataDataExtractor::SetNestedExpandPaths(
    const std::vector<std::string> &nested_paths) {
  nested_expand_paths = nested_paths;
}

void ODataDataExtractor::UpdateExpandedColumnType(
    size_t index, const duckdb::LogicalType &new_type) {
    if (index < expanded_data_types.size()) {
    ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                     "Updating expanded column type at index " +
                         std::to_string(index) + " from " +
                         expanded_data_types[index].ToString() + " to " +
                         new_type.ToString());
        expanded_data_types[index] = new_type;
    } else {
    ERPL_TRACE_WARN(
        "DATA_EXTRACTOR",
        "Attempted to update expanded column type at invalid index " +
            std::to_string(index));
    }
}

std::vector<std::string> ODataDataExtractor::GetExpandedDataSchema() const {
    return expanded_data_schema;
}

std::vector<duckdb::LogicalType>
ODataDataExtractor::GetExpandedDataTypes() const {
    return expanded_data_types;
}

std::vector<std::string> ODataDataExtractor::GetNestedExpandPaths() const {
  return nested_expand_paths;
}

bool ODataDataExtractor::HasExpandedData() const {
    return !expanded_data_schema.empty();
}

void ODataDataExtractor::ExtractExpandedDataFromResponse(
    const std::string &response_content) {
    if (!HasExpandedData() || expand_paths.empty()) {
        return;
    }
    
  ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                   "Extracting expanded data from response of " +
                       std::to_string(response_content.size()) + " bytes");
    
  JsonDocHandle doc(duckdb_yyjson::yyjson_read(response_content.c_str(),
                                               response_content.size(), 0));
    if (!doc) {
    ERPL_TRACE_WARN(
        "DATA_EXTRACTOR",
        "Failed to parse JSON response for expanded data extraction");
        return;
    }
    
    auto root = doc.Root();
    
    try {
        // Handle OData V4 format
        auto value_arr = duckdb_yyjson::yyjson_obj_get(root, "value");
        if (value_arr && duckdb_yyjson::yyjson_is_arr(value_arr)) {
      ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                       "Processing OData V4 response format for expanded data");
            ProcessODataV4ExpandedData(value_arr);
        } else {
            // Handle OData V2 format
            auto data_obj = duckdb_yyjson::yyjson_obj_get(root, "d");
            if (data_obj && duckdb_yyjson::yyjson_is_obj(data_obj)) {
                auto results_arr = duckdb_yyjson::yyjson_obj_get(data_obj, "results");
                if (results_arr && duckdb_yyjson::yyjson_is_arr(results_arr)) {
          ERPL_TRACE_DEBUG(
              "DATA_EXTRACTOR",
              "Processing OData V2 response format for expanded data");
                    ProcessODataV2ExpandedData(results_arr);
                }
            }
        }
  } catch (const StrictTypingViolation &) {
    // strict_typing must fail the query, not be downgraded to a warning.
    throw;
  } catch (const std::exception &e) {
    ERPL_TRACE_WARN("DATA_EXTRACTOR",
                    "Error processing expanded data: " + std::string(e.what()));
    }
}

duckdb::Value
ODataDataExtractor::ExtractExpandedDataForRow(duckdb::idx_t row_index,
                                              const std::string &expand_path) {
    auto it = expanded_data_cache.find(expand_path);
    if (it != expanded_data_cache.end()) {
        // The vectors are indexed relative to the oldest row still held: anything before
        // that has been released and must read as NULL rather than as whatever value now
        // occupies that offset.
        if (row_index >= expanded_data_base_row_) {
            const auto local_index = static_cast<size_t>(row_index - expanded_data_base_row_);
            if (local_index < it->second.size()) {
                return it->second[local_index];
            }
        }
        {
            // fall through to NULL
        }
    }
    return duckdb::Value();
}

void ODataDataExtractor::ProcessODataV4ExpandedData(
    duckdb_yyjson::yyjson_val *value_arr) {
  ProcessExpandedDataRows(value_arr, "OData V4");
}

void ODataDataExtractor::ProcessODataV2ExpandedData(
    duckdb_yyjson::yyjson_val *results_arr) {
  ProcessExpandedDataRows(results_arr, "OData V2");
}

duckdb::LogicalType ODataDataExtractor::GetExpandedTargetType(
    const std::string &expand_path) const {
  for (size_t i = 0; i < expanded_data_schema.size(); ++i) {
    if (expanded_data_schema[i] == expand_path &&
        i < expanded_data_types.size()) {
      return expanded_data_types[i];
    }
  }
  return duckdb::LogicalTypeId::VARCHAR;
}

void ODataDataExtractor::ProcessExpandedDataRows(
    duckdb_yyjson::yyjson_val *rows_arr, const std::string &response_format) {
  ERPL_TRACE_INFO("DATA_EXTRACTOR",
                  "Processing " + response_format + " expanded data with " +
                      std::to_string(expand_paths.size()) + " expand paths");

  duckdb_yyjson::yyjson_arr_iter arr_it;
  duckdb_yyjson::yyjson_arr_iter_init(rows_arr, &arr_it);

  duckdb_yyjson::yyjson_val *row;
  size_t row_index = 0;
  while ((row = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
    if (duckdb_yyjson::yyjson_is_obj(row)) {
      for (const auto &expand_path : expand_paths) {
        auto expand_data =
            duckdb_yyjson::yyjson_obj_get(row, expand_path.c_str());
        if (!expand_data) {
          expanded_data_cache[expand_path].push_back(duckdb::Value());
          continue;
        }

        current_column_context_ = expand_path;
        try {
          auto parsed_value = ParseExpandedDataRecursively(
              expand_data, expand_path, GetExpandedTargetType(expand_path));
          expanded_data_cache[expand_path].push_back(parsed_value);
          ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                           "Extracted expanded data for path '" + expand_path +
                               "' at row " + std::to_string(row_index));
        } catch (const StrictTypingViolation &) {
          // strict_typing fails the whole query, so cache alignment no longer
          // matters - let it out.
          throw;
        } catch (const NumericRangeError &e) {
          // A value that does not fit its column is a data-integrity problem, so
          // it is recorded against the column rather than wrapped around into a
          // different number.
          //
          // The cache is indexed by row position, so this row must still
          // contribute exactly one entry per expand path - skipping it would
          // shift every later row onto the wrong expanded value, and re-throwing
          // would leave this page's cache short and misalign the pages after it.
          // So the failure is logged and an aligned NULL is pushed.
          // Recorded for the end-of-scan report the user sees, and retained in
          // GetLastError() for programmatic diagnosis.
          LogError("EXPANDED_DATA_RANGE",
                   "Dropping expanded value for path '" + expand_path +
                       "' at row " + std::to_string(row_index) + ": " +
                       e.what());
          RecordConversionFailure(expand_path, "", e.what());
          expanded_data_cache[expand_path].push_back(duckdb::Value());
        } catch (const std::exception &e) {
          ERPL_TRACE_WARN("DATA_EXTRACTOR",
                          "Failed to parse expand data for path '" +
                              expand_path + "': " + e.what());
          // The exception MUST stop here: expanded_data_cache is indexed by
          // row position, so skipping the push_back would leave this page's
          // cache short and misalign every later page onto the wrong rows.
          // Alignment is preserved by pushing a NULL in place; visibility is
          // preserved by recording the failure for the end-of-scan report.
          RecordConversionFailure(expand_path, "<expanded payload>", e.what());
          expanded_data_cache[expand_path].push_back(duckdb::Value());
        }
      }
    }
    row_index++;
  }

  ERPL_TRACE_INFO("DATA_EXTRACTOR",
                  "Processed " + std::to_string(row_index) + " " +
                      response_format + " rows with expanded data");
}

// Parse expanded data recursively, handling nested expands like
// Products/Suppliers
duckdb::Value ODataDataExtractor::ParseExpandedDataRecursively(
    duckdb_yyjson::yyjson_val *expand_data, const std::string &expand_path,
    const duckdb::LogicalType &target_type) {
  if (!expand_data) {
    return duckdb::Value();
  }

  auto payload = expand_data;
  if (duckdb_yyjson::yyjson_is_obj(expand_data)) {
    auto results_field = duckdb_yyjson::yyjson_obj_get(expand_data, "results");
    if (results_field && duckdb_yyjson::yyjson_is_arr(results_field)) {
      payload = results_field;
    }
  }

  if (target_type.id() != duckdb::LogicalTypeId::LIST) {
    auto json_string = JsonValueToString(payload);
    return json_string.empty()
               ? duckdb::Value()
               : ParseJsonToDuckDBValue(json_string, target_type);
  }

  auto child_type = duckdb::ListType::GetChildType(target_type);
  if (!duckdb_yyjson::yyjson_is_arr(payload)) {
    return duckdb::Value::LIST(child_type, {});
  }

  if (child_type.id() != duckdb::LogicalTypeId::VARCHAR) {
    return ParseJsonArray(payload, target_type);
  }

  auto first_item = duckdb_yyjson::yyjson_arr_get_first(payload);
  if (!first_item || !duckdb_yyjson::yyjson_is_obj(first_item)) {
    return ParseJsonArray(payload, target_type);
  }

  child_type =
      InferStructTypeFromJsonObjectWithNestedExpands(first_item, expand_path);
  auto inferred_target_type = duckdb::LogicalType::LIST(child_type);
  bool updated_stored_type = false;
  for (size_t i = 0; i < expanded_data_schema.size() &&
                     i < expanded_data_types.size(); ++i) {
    if (expanded_data_schema[i] == expand_path) {
      expanded_data_types[i] = inferred_target_type;
      updated_stored_type = true;
      break;
    }
  }
  if (!updated_stored_type) {
    for (auto &expanded_type : expanded_data_types) {
      if (expanded_type.id() == duckdb::LogicalTypeId::LIST &&
          duckdb::ListType::GetChildType(expanded_type).id() ==
              duckdb::LogicalTypeId::VARCHAR) {
        expanded_type = inferred_target_type;
        break;
      }
    }
  }

  ERPL_TRACE_DEBUG("DATA_EXTRACTOR", "Inferred struct type for '" + expand_path +
                                         "': " + child_type.ToString());
  return ParseJsonArray(payload, inferred_target_type);
}

duckdb::Value ODataDataExtractor::ParseJsonToDuckDBValue(
    const std::string &json_str, const duckdb::LogicalType &target_type) {
  if (json_str.empty()) {
    return duckdb::Value();
  }

  JsonDocHandle doc(
      duckdb_yyjson::yyjson_read(json_str.c_str(), json_str.length(), 0));
  if (!doc) {
    throw std::runtime_error("Failed to parse JSON string");
  }

  auto root = doc.Root();
  if (!root) {
    throw std::runtime_error("Failed to get JSON root");
  }

  return ParseJsonValueToDuckDBValue(root, target_type);
}

duckdb::Value ODataDataExtractor::ParseJsonValueToDuckDBValue(
    duckdb_yyjson::yyjson_val *value, const duckdb::LogicalType &target_type) {
  if (!value) {
    ERPL_TRACE_WARN("DATA_EXTRACTOR", "Null JSON value passed to parser");
    return duckdb::Value();
  }

  if (duckdb_yyjson::yyjson_is_null(value)) {
    return duckdb::Value();
  }

  try {
    switch (target_type.id()) {
    case duckdb::LogicalTypeId::LIST:
      return ConvertList(value, target_type);
    case duckdb::LogicalTypeId::STRUCT:
      return ConvertStruct(value, target_type);
    case duckdb::LogicalTypeId::VARCHAR:
      return ConvertVarchar(value);
    case duckdb::LogicalTypeId::INTEGER:
      return ConvertInteger(value, target_type);
    case duckdb::LogicalTypeId::BIGINT:
      return ConvertBigint(value);
    case duckdb::LogicalTypeId::FLOAT:
    case duckdb::LogicalTypeId::DOUBLE:
      return ConvertFloatLike(value);
    case duckdb::LogicalTypeId::DECIMAL:
      return ConvertDecimal(value, target_type);
    case duckdb::LogicalTypeId::BOOLEAN:
      return ConvertBoolean(value);
    case duckdb::LogicalTypeId::TIMESTAMP:
      return ConvertTimestamp(value);
    default:
      return ConvertFallbackAsString(value, target_type);
    }
  } catch (const StrictTypingViolation &) {
    throw;
  } catch (const NumericRangeError &) {
    throw;
  } catch (const std::exception &e) {
    LogError("JSON_PARSING",
             "Unexpected error parsing JSON value: " + std::string(e.what()));
    return CreateFallbackValue(target_type, e.what());
  }
}

duckdb::Value
ODataDataExtractor::ConvertList(duckdb_yyjson::yyjson_val *value,
                                const duckdb::LogicalType &target_type) {
  if (duckdb_yyjson::yyjson_is_arr(value)) {
    return ParseJsonArray(value, target_type);
  }
  ERPL_TRACE_WARN("DATA_EXTRACTOR",
                  "Expected array for LIST type but got different JSON type");
  return CreateFallbackValue(target_type);
}

duckdb::Value
ODataDataExtractor::ConvertStruct(duckdb_yyjson::yyjson_val *value,
                                  const duckdb::LogicalType &target_type) {
  if (duckdb_yyjson::yyjson_is_obj(value)) {
    return ParseJsonObject(value, target_type);
  }
  ERPL_TRACE_WARN("DATA_EXTRACTOR",
                  "Expected object for STRUCT type but got different JSON type");
  return CreateFallbackValue(target_type);
}

duckdb::Value
ODataDataExtractor::ConvertVarchar(duckdb_yyjson::yyjson_val *value) {
  if (duckdb_yyjson::yyjson_is_str(value)) {
    std::string string_value = duckdb_yyjson::yyjson_get_str(value);
    duckdb::Value timestamp_value;
    if (TryParseODataV2DateLiteral(string_value, timestamp_value)) {
      return timestamp_value.DefaultCastAs(duckdb::LogicalTypeId::VARCHAR);
    }
    return duckdb::Value(string_value);
  }

  auto json_string = JsonValueToString(value);
  return duckdb::Value(json_string);
}

duckdb::Value ODataDataExtractor::ConvertInteger(
    duckdb_yyjson::yyjson_val *value,
    const duckdb::LogicalType &target_type) {
  // yyjson_is_int() is true for both the signed and the unsigned number
  // subtype, so the unsigned case has to be tested first. yyjson_get_int()
  // returns a plain `int` and would truncate anything wider, which is exactly
  // the silent corruption this function must avoid: read the full 64-bit value
  // and range-check it against the INTEGER column instead.
  if (duckdb_yyjson::yyjson_is_uint(value)) {
    const uint64_t uint_value = duckdb_yyjson::yyjson_get_uint(value);
    if (uint_value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw NumericRangeError(
          "OData value " + std::to_string(uint_value) +
          " does not fit into a DuckDB INTEGER column (INT32 range is " +
          std::to_string(std::numeric_limits<int32_t>::min()) + " .. " +
          std::to_string(std::numeric_limits<int32_t>::max()) + ")");
    }
    return duckdb::Value(static_cast<int32_t>(uint_value));
  }
  if (duckdb_yyjson::yyjson_is_sint(value)) {
    const int64_t int_value = duckdb_yyjson::yyjson_get_sint(value);
    if (int_value < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        int_value > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
      throw NumericRangeError(
          "OData value " + std::to_string(int_value) +
          " does not fit into a DuckDB INTEGER column (INT32 range is " +
          std::to_string(std::numeric_limits<int32_t>::min()) + " .. " +
          std::to_string(std::numeric_limits<int32_t>::max()) + ")");
    }
    return duckdb::Value(static_cast<int32_t>(int_value));
  }
  if (duckdb_yyjson::yyjson_is_str(value)) {
    const std::string string_value = duckdb_yyjson::yyjson_get_str(value);
    try {
      const int64_t parsed_value = std::stoll(string_value);
      if (parsed_value < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
          parsed_value > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        throw NumericRangeError(
            "OData value '" + string_value +
            "' does not fit into a DuckDB INTEGER column (INT32 range is " +
            std::to_string(std::numeric_limits<int32_t>::min()) + " .. " +
            std::to_string(std::numeric_limits<int32_t>::max()) + ")");
      }
      return duckdb::Value(static_cast<int32_t>(parsed_value));
    } catch (const std::out_of_range &) {
      throw NumericRangeError(
          "OData value '" + string_value +
          "' does not fit into a DuckDB INTEGER column (INT32 range is " +
          std::to_string(std::numeric_limits<int32_t>::min()) + " .. " +
          std::to_string(std::numeric_limits<int32_t>::max()) + ")");
    } catch (const std::invalid_argument &e) {
      LogError("INTEGER_CONVERSION", "Failed to convert string '" +
                                         string_value +
                                         "' to integer: " + e.what());
    }
  }
  return CreateFallbackValue(target_type);
}

duckdb::Value
ODataDataExtractor::ConvertBigint(duckdb_yyjson::yyjson_val *value) {
  // Unsigned first: yyjson_is_int() also matches the unsigned subtype.
  if (duckdb_yyjson::yyjson_is_uint(value)) {
    const uint64_t uint_value = duckdb_yyjson::yyjson_get_uint(value);
    if (uint_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw NumericRangeError(
          "OData value " + std::to_string(uint_value) +
          " does not fit into a DuckDB BIGINT column (INT64 maximum is " +
          std::to_string(std::numeric_limits<int64_t>::max()) + ")");
    }
    return duckdb::Value(static_cast<int64_t>(uint_value));
  }
  if (duckdb_yyjson::yyjson_is_sint(value)) {
    // yyjson_get_int() returns a plain `int` and would truncate the value to
    // 32 bits before it ever reaches the BIGINT column.
    return duckdb::Value(
        static_cast<int64_t>(duckdb_yyjson::yyjson_get_sint(value)));
  }
  if (duckdb_yyjson::yyjson_is_str(value)) {
    try {
      return duckdb::Value(
          static_cast<int64_t>(std::stoll(duckdb_yyjson::yyjson_get_str(value))));
    } catch (const std::exception &e) {
      LogError("BIGINT_CONVERSION",
               "Failed to convert string '" +
                   std::string(duckdb_yyjson::yyjson_get_str(value)) +
                   "' to bigint: " + e.what());
    }
  }
  return CreateFallbackValue(duckdb::LogicalType(duckdb::LogicalTypeId::BIGINT));
}

duckdb::Value
ODataDataExtractor::ConvertFloatLike(duckdb_yyjson::yyjson_val *value) {
  if (duckdb_yyjson::yyjson_is_real(value)) {
    return duckdb::Value(duckdb_yyjson::yyjson_get_real(value));
  }
  if (duckdb_yyjson::yyjson_is_uint(value)) {
    return duckdb::Value(
        static_cast<double>(duckdb_yyjson::yyjson_get_uint(value)));
  }
  if (duckdb_yyjson::yyjson_is_sint(value)) {
    // yyjson_get_int() truncates to 32 bits; yyjson_get_sint() keeps all 64.
    return duckdb::Value(
        static_cast<double>(duckdb_yyjson::yyjson_get_sint(value)));
  }
  if (duckdb_yyjson::yyjson_is_str(value)) {
    try {
      return duckdb::Value(std::stod(duckdb_yyjson::yyjson_get_str(value)));
    } catch (const std::exception &e) {
      LogError("FLOAT_CONVERSION",
               "Failed to convert string '" +
                   std::string(duckdb_yyjson::yyjson_get_str(value)) +
                   "' to float: " + e.what());
    }
  }
  return CreateFallbackValue(duckdb::LogicalType(duckdb::LogicalTypeId::DOUBLE));
}

duckdb::Value ODataDataExtractor::ConvertDecimal(
    duckdb_yyjson::yyjson_val *value,
    const duckdb::LogicalType &target_type) {
  try {
    if (duckdb_yyjson::yyjson_is_str(value)) {
      return duckdb::Value(duckdb_yyjson::yyjson_get_str(value))
          .DefaultCastAs(target_type);
    }
    if (duckdb_yyjson::yyjson_is_int(value)) {
      return duckdb::Value::BIGINT(duckdb_yyjson::yyjson_get_int(value))
          .DefaultCastAs(target_type);
    }
    if (duckdb_yyjson::yyjson_is_uint(value)) {
      return duckdb::Value::UBIGINT(duckdb_yyjson::yyjson_get_uint(value))
          .DefaultCastAs(target_type);
    }
    if (duckdb_yyjson::yyjson_is_real(value)) {
      return duckdb::Value::DOUBLE(duckdb_yyjson::yyjson_get_real(value))
          .DefaultCastAs(target_type);
    }
  } catch (const std::exception &e) {
    LogError("DECIMAL_CONVERSION", e.what());
  }
  return CreateFallbackValue(target_type);
}

duckdb::Value
ODataDataExtractor::ConvertBoolean(duckdb_yyjson::yyjson_val *value) {
  if (duckdb_yyjson::yyjson_is_bool(value)) {
    return duckdb::Value(duckdb_yyjson::yyjson_get_bool(value));
  }
  if (duckdb_yyjson::yyjson_is_str(value)) {
    auto string_value =
        NormalizeJsonFieldName(duckdb_yyjson::yyjson_get_str(value));
    if (IsTruthyString(string_value)) {
      return duckdb::Value(true);
    }
    if (IsFalsyString(string_value)) {
      return duckdb::Value(false);
    }
    LogError("BOOLEAN_CONVERSION",
             "Failed to convert string '" + string_value + "' to boolean");
  }
  return CreateFallbackValue(duckdb::LogicalType(duckdb::LogicalTypeId::BOOLEAN));
}

duckdb::Value
ODataDataExtractor::ConvertTimestamp(duckdb_yyjson::yyjson_val *value) {
  if (duckdb_yyjson::yyjson_is_str(value)) {
    std::string timestamp_str = duckdb_yyjson::yyjson_get_str(value);
    duckdb::Value timestamp_value;
    if (TryParseODataV2DateLiteral(timestamp_str, timestamp_value)) {
      return timestamp_value;
    }

    if (timestamp_str.length() < 19) {
      LogError("TIMESTAMP_CONVERSION",
               "Timestamp string too short: '" + timestamp_str + "'");
      return CreateFallbackValue(duckdb::LogicalType(duckdb::LogicalTypeId::TIMESTAMP));
    }

    try {
      std::string duckdb_format = timestamp_str.substr(0, 19);
      std::replace(duckdb_format.begin(), duckdb_format.end(), 'T', ' ');
      auto parsed_timestamp = duckdb::Value::TIMESTAMP(
          duckdb::Timestamp::FromString(duckdb_format, false));
      ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                       "Successfully parsed timestamp: '" + timestamp_str +
                           "' -> '" + duckdb_format + "'");
      return parsed_timestamp;
    } catch (const std::exception &e) {
      LogError("TIMESTAMP_CONVERSION",
               "Failed to parse timestamp '" + timestamp_str + "': " +
                   e.what());
      return CreateFallbackValue(duckdb::LogicalType(duckdb::LogicalTypeId::TIMESTAMP));
    }
  }

  if (duckdb_yyjson::yyjson_is_int(value)) {
    try {
      const auto unix_timestamp = duckdb_yyjson::yyjson_get_int(value);
      auto parsed_timestamp = duckdb::Value::TIMESTAMP(
          duckdb::Timestamp::FromEpochSeconds(unix_timestamp));
      ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                       "Successfully parsed Unix timestamp: " +
                           std::to_string(unix_timestamp));
      return parsed_timestamp;
    } catch (const std::exception &e) {
      LogError("TIMESTAMP_CONVERSION",
               std::string("Failed to parse Unix timestamp: ") + e.what());
    }
  }

  return CreateFallbackValue(duckdb::LogicalType(duckdb::LogicalTypeId::TIMESTAMP));
}

duckdb::Value ODataDataExtractor::ConvertFallbackAsString(
    duckdb_yyjson::yyjson_val *value,
    const duckdb::LogicalType &target_type) {
  auto json_string = JsonValueToString(value);
  if (json_string.empty()) {
    return CreateFallbackValue(target_type);
  }

  if (target_type.id() == duckdb::LogicalTypeId::VARCHAR) {
    return duckdb::Value(json_string);
  }

  try {
    return duckdb::Value(json_string).DefaultCastAs(target_type);
  } catch (...) {
    return CreateFallbackValue(target_type);
  }
}

// Enhanced parsing methods for better type resolution
duckdb::Value
ODataDataExtractor::ParseJsonArray(duckdb_yyjson::yyjson_val *array_val,
                                   const duckdb::LogicalType &target_type) {
    auto child_type = duckdb::ListType::GetChildType(target_type);
    duckdb::vector<duckdb::Value> list_values;
    
    try {
        duckdb_yyjson::yyjson_arr_iter arr_it;
        duckdb_yyjson::yyjson_arr_iter_init(array_val, &arr_it);
        
    // Every element is converted. An earlier version stopped after batch_size_
    // items, which dropped the remainder of every larger expanded collection
    // with nothing but a trace warning to show for it - the caller received a
    // short list and had no way to tell it was incomplete.
    list_values.reserve(duckdb_yyjson::yyjson_arr_size(array_val));

    duckdb_yyjson::yyjson_val *item;
        while ((item = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
            try {
                list_values.push_back(ParseJsonValueToDuckDBValue(item, child_type));
            } catch (const StrictTypingViolation &) {
                throw;
            } catch (const NumericRangeError &e) {
                // One unrepresentable element must not discard the rest of the
                // collection, and must not shorten it either: a missing element
                // would re-index every element after it. So the failure is
                // recorded and a typed NULL takes its place, in position.
                LogError("ARRAY_ELEMENT_RANGE", e.what());
                // Attribute the failure to the column being extracted, not to the
                // element's type, so the end-of-scan report names something the
                // user can act on.
                RecordConversionFailure(current_column_context_.empty()
                                            ? child_type.ToString()
                                            : current_column_context_,
                                        JsonValueToString(item), e.what());
                list_values.push_back(duckdb::Value(child_type));
            }
        }

        return duckdb::Value::LIST(child_type, list_values);

  } catch (const StrictTypingViolation &) {
    throw;
  } catch (const std::exception &e) {
    LogError("ARRAY_PARSING",
             "Failed to parse JSON array: " + std::string(e.what()));
        return CreateFallbackValue(target_type);
    }
}

duckdb::Value
ODataDataExtractor::ParseJsonObject(duckdb_yyjson::yyjson_val *obj_val,
                                    const duckdb::LogicalType &target_type) {
    try {
    auto &struct_types = duckdb::StructType::GetChildTypes(target_type);
        duckdb::vector<duckdb::Value> struct_values;
        struct_values.resize(struct_types.size(), duckdb::Value());
        
        // Build child name map (lowercase and underscore-stripped) -> index
        std::unordered_map<std::string, size_t> child_index;
        for (size_t i = 0; i < struct_types.size(); ++i) {
            const auto &field_name = struct_types[i].first;
            child_index.emplace(NormalizeJsonFieldName(duckdb_compat::Str(field_name)), i);
            child_index.emplace(NormalizeJsonFieldName(duckdb_compat::Str(field_name), true), i);
        }
        
        // Iterate JSON object keys and assign where matching
        size_t idx, max;
        duckdb_yyjson::yyjson_val *json_key, *json_val;
        yyjson_obj_foreach(obj_val, idx, max, json_key, json_val) {
            if (!duckdb_yyjson::yyjson_is_str(json_key))
        continue;
            std::string key = duckdb_yyjson::yyjson_get_str(json_key);
            auto lower = NormalizeJsonFieldName(key);
            auto it = child_index.find(lower);
            if (it == child_index.end()) {
                it = child_index.find(NormalizeJsonFieldName(key, true));
            }
            if (it != child_index.end()) {
                size_t pos = it->second;
                const auto &field_type = struct_types[pos].second;
                try {
                    struct_values[pos] = ParseJsonValueToDuckDBValue(json_val, field_type);
                } catch (const StrictTypingViolation &) {
                    throw;
                } catch (const NumericRangeError &e) {
                    // Only the offending field becomes NULL. Discarding the whole
                    // record because one number does not fit would throw away the
                    // fields that are perfectly good, and the record still has to
                    // occupy its position so later rows are not re-indexed.
                    LogError("STRUCT_FIELD_RANGE",
                             "Field '" + duckdb_compat::Str(struct_types[pos].first) + "': " + e.what());
                    RecordConversionFailure(duckdb_compat::Str(struct_types[pos].first),
                                            JsonValueToString(json_val), e.what());
                    struct_values[pos] = duckdb::Value(field_type);
                }
            }
        }
        
        return duckdb::Value::STRUCT(target_type, struct_values);

  } catch (const StrictTypingViolation &) {
    throw;
  } catch (const std::exception &e) {
    LogError("OBJECT_PARSING",
             "Failed to parse JSON object: " + std::string(e.what()));
        return CreateFallbackValue(target_type);
    }
}

// Infer DuckDB struct type from JSON object
duckdb::LogicalType ODataDataExtractor::InferStructTypeFromJsonObject(
    duckdb_yyjson::yyjson_val *obj_val) {
    if (!obj_val || !duckdb_yyjson::yyjson_is_obj(obj_val)) {
        return duckdb::LogicalTypeId::VARCHAR; // Fallback to VARCHAR if not an object
    }
    
    duckdb::child_list_t<duckdb::LogicalType> struct_fields;
    
    size_t idx, max;
    duckdb_yyjson::yyjson_val *json_key, *json_val;
    yyjson_obj_foreach(obj_val, idx, max, json_key, json_val) {
    if (!duckdb_yyjson::yyjson_is_str(json_key))
      continue;
        
        std::string field_name = duckdb_yyjson::yyjson_get_str(json_key);
        
        // Skip metadata fields that are OData-specific
        if (field_name == "__metadata" || field_name == "__deferred") {
            continue;
        }
        
        // Infer the type for this field
        duckdb::LogicalType field_type = InferTypeFromJsonValue(json_val);
        
        // Add the field to our struct
        struct_fields.emplace_back(field_name, field_type);
    }
    
    if (struct_fields.empty()) {
        return duckdb::LogicalTypeId::VARCHAR; // Fallback if no fields
    }
    
    return duckdb::LogicalType::STRUCT(struct_fields);
}

// Infer DuckDB type from JSON value
duckdb::LogicalType
ODataDataExtractor::InferTypeFromJsonValue(duckdb_yyjson::yyjson_val *value) {
    if (!value) {
        return duckdb::LogicalTypeId::VARCHAR;
    }
    
    if (duckdb_yyjson::yyjson_is_null(value)) {
        return duckdb::LogicalTypeId::VARCHAR; // NULL values get VARCHAR type
    }
    
    if (duckdb_yyjson::yyjson_is_str(value)) {
        std::string str_val = duckdb_yyjson::yyjson_get_str(value);
        
        // Check for special OData V2 date format: "/Date(timestamp)/"
    if (str_val.length() > 8 && str_val.substr(0, 6) == "/Date(" &&
        str_val.substr(str_val.length() - 2) == ")/") {
            return duckdb::LogicalTypeId::TIMESTAMP;
        }
        
        // Check if it looks like a date
        if (str_val.length() == 10 && str_val[4] == '-' && str_val[7] == '-') {
            return duckdb::LogicalTypeId::DATE;
        }
        
        // Check if it looks like a timestamp
    if (str_val.length() >= 19 && str_val[4] == '-' && str_val[7] == '-' &&
        str_val[10] == 'T') {
            return duckdb::LogicalTypeId::TIMESTAMP;
        }
        
        return duckdb::LogicalTypeId::VARCHAR;
    }
    
    // Unsigned before signed: yyjson_is_int() matches both subtypes. Reading
    // the value with yyjson_get_int() would truncate it to 32 bits and could
    // classify a wide number as INTEGER, which then loses data on conversion.
    if (duckdb_yyjson::yyjson_is_uint(value)) {
        const uint64_t uint_val = duckdb_yyjson::yyjson_get_uint(value);
        if (uint_val <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            return duckdb::LogicalTypeId::INTEGER;
        }
        return duckdb::LogicalTypeId::BIGINT;
    }

    if (duckdb_yyjson::yyjson_is_sint(value)) {
        const int64_t int_val = duckdb_yyjson::yyjson_get_sint(value);
        if (int_val >= static_cast<int64_t>(std::numeric_limits<int32_t>::min()) &&
            int_val <= static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
            return duckdb::LogicalTypeId::INTEGER;
        }
        return duckdb::LogicalTypeId::BIGINT;
    }

    if (duckdb_yyjson::yyjson_is_real(value)) {
        return duckdb::LogicalTypeId::DOUBLE;
    }
    
    if (duckdb_yyjson::yyjson_is_bool(value)) {
        return duckdb::LogicalTypeId::BOOLEAN;
    }
    
    if (duckdb_yyjson::yyjson_is_arr(value)) {
        // For arrays, we'll use LIST(VARCHAR) as a safe default
        // The actual item types could be inferred later if needed
        return duckdb::LogicalType::LIST(duckdb::LogicalTypeId::VARCHAR);
    }
    
    if (duckdb_yyjson::yyjson_is_obj(value)) {
        // For objects, we could recursively infer the struct type
        // But for now, we'll use VARCHAR to avoid infinite recursion
        return duckdb::LogicalTypeId::VARCHAR;
    }
    
    return duckdb::LogicalTypeId::VARCHAR; // Default fallback
}

// This method was removed as it required HTTP requests that aren't available in
// the current architecture

// Error handling helpers
void ODataDataExtractor::LogError(const std::string &context,
                                  const std::string &error_msg) const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    
    last_error_ = "[" + context + "] " + error_msg;
    error_counts_[context]++;
    
    // Rate-limit error logging to avoid spam
    if (error_counts_[context] <= 10 || error_counts_[context] % 100 == 0) {
    ERPL_TRACE_ERROR("DATA_EXTRACTOR",
                     last_error_ + " (count: " +
                         std::to_string(error_counts_[context]) + ")");
    }
}

bool ODataDataExtractor::ShouldRetryAfterError(
    const std::string &context) const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    auto it = error_counts_.find(context);
    return it == error_counts_.end() || it->second < 3; // Retry up to 3 times
}

void ODataDataExtractor::SetConversionFailureLog(
    std::shared_ptr<ConversionFailureLog> log) {
  conversion_failure_log_ = std::move(log);
}

void ODataDataExtractor::RecordConversionFailure(
    const std::string &column_name, const std::string &offending_value,
    const std::string &error_message) const {
  if (!conversion_failure_log_) {
    return;
  }
  conversion_failure_log_->RecordFailure(column_name, offending_value,
                                         error_message);
}

// A value we could not build becomes a NULL, never a fabricated 0 / "" / false.
// Substituting a zero into a numeric column makes a conversion failure
// indistinguishable from a real zero the server sent, which is exactly the
// silent substitution the rest of this file refuses to do.
duckdb::Value ODataDataExtractor::CreateFallbackValue(
    const duckdb::LogicalType &target_type, const std::string &reason,
    const std::string &offending_value) const {
  const auto message =
      reason.empty()
          ? "value could not be converted to " + target_type.ToString()
          : reason;
  RecordConversionFailure(current_column_context_, offending_value, message);
  return duckdb::Value(target_type);
}

// Performance and memory management methods
void ODataDataExtractor::SetBatchSize(size_t batch_size) {
    batch_size_ = batch_size;
  ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                   "Set batch size to " + std::to_string(batch_size));
}

void ODataDataExtractor::EnableCompression(bool enable) {
    compression_enabled_ = enable;
  ERPL_TRACE_DEBUG("DATA_EXTRACTOR",
                   "Compression " +
                       std::string(enable ? "enabled" : "disabled"));
}

void ODataDataExtractor::ReleaseExpandedDataBefore(duckdb::idx_t row_index) {
    if (row_index <= expanded_data_base_row_) {
        return;
    }
    const auto to_release = static_cast<size_t>(row_index - expanded_data_base_row_);

    for (auto &[path, values] : expanded_data_cache) {
        const auto erase_count = std::min(to_release, values.size());
        values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(erase_count));
    }
    expanded_data_base_row_ = row_index;
}

void ODataDataExtractor::ClearCache() {
    expanded_data_cache.clear();
    expanded_data_base_row_ = 0;
    ERPL_TRACE_DEBUG("DATA_EXTRACTOR", "Cleared expanded data cache");
}

size_t ODataDataExtractor::GetCacheSize() const {
    size_t total_size = 0;
  for (const auto &[path, values] : expanded_data_cache) {
        total_size += values.size();
    }
    return total_size;
}

bool ODataDataExtractor::ValidateExpandedData(
    const std::string &expand_path) const {
    auto it = expanded_data_cache.find(expand_path);
    return it != expanded_data_cache.end() && !it->second.empty();
}

std::string ODataDataExtractor::GetLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void ODataDataExtractor::ResetErrorState() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
    error_counts_.clear();
}

void ODataDataExtractor::OptimizeCacheMemory() {
    // Remove empty cache entries
  for (auto it = expanded_data_cache.begin();
       it != expanded_data_cache.end();) {
        if (it->second.empty()) {
            it = expanded_data_cache.erase(it);
        } else {
            ++it;
        }
    }
    
    ERPL_TRACE_DEBUG("DATA_EXTRACTOR", "Optimized cache memory usage");
}

void ODataDataExtractor::CompressCacheData() {
    if (!compression_enabled_) {
        return;
    }
    
    // This is a placeholder for potential compression logic
    // In a real implementation, we could serialize and compress the data
    ERPL_TRACE_DEBUG("DATA_EXTRACTOR", "Cache compression completed");
}

// Infer struct type from JSON object, handling nested expands
duckdb::LogicalType
ODataDataExtractor::InferStructTypeFromJsonObjectWithNestedExpands(
    duckdb_yyjson::yyjson_val *obj_val, const std::string &expand_path) {
    if (!obj_val || !duckdb_yyjson::yyjson_is_obj(obj_val)) {
        return duckdb::LogicalTypeId::VARCHAR; // Fallback to VARCHAR if not an object
    }
    
    duckdb::child_list_t<duckdb::LogicalType> struct_fields;
    
    size_t idx, max;
    duckdb_yyjson::yyjson_val *json_key, *json_val;
    yyjson_obj_foreach(obj_val, idx, max, json_key, json_val) {
    if (!duckdb_yyjson::yyjson_is_str(json_key))
      continue;
        
        std::string field_name = duckdb_yyjson::yyjson_get_str(json_key);
        
        // Skip metadata fields that are OData-specific
        if (field_name == "__metadata" || field_name == "__deferred") {
            continue;
        }
        
    // Check if this field is a nested expand (e.g., if expand_path is
    // "Products/Suppliers" and we're processing "Products")
        bool is_nested_expand = false;
    for (const auto &path : expand_paths) {
      if (path.find(expand_path + "/") == 0 &&
          path.substr(expand_path.length() + 1) == field_name) {
                is_nested_expand = true;
                break;
            }
        }
        
        duckdb::LogicalType field_type;
        if (is_nested_expand) {
            // This is a nested expand, so we need to handle it recursively
            if (duckdb_yyjson::yyjson_is_obj(json_val)) {
                // Check if it has a 'results' array (OData V2 collection)
                auto results_field = duckdb_yyjson::yyjson_obj_get(json_val, "results");
                if (results_field && duckdb_yyjson::yyjson_is_arr(results_field)) {
                    // It's a collection, so use LIST(STRUCT)
          auto nested_struct_type =
              InferStructTypeFromJsonObjectWithNestedExpands(
                  json_val, expand_path + "/" + field_name);
                    field_type = duckdb::LogicalType::LIST(nested_struct_type);
                } else {
                    // It's a single entity, so use STRUCT
          field_type = InferStructTypeFromJsonObjectWithNestedExpands(
              json_val, expand_path + "/" + field_name);
                }
            } else {
                field_type = duckdb::LogicalTypeId::VARCHAR; // Fallback
            }
        } else {
            // Regular field, infer the type normally
            field_type = InferTypeFromJsonValue(json_val);
        }
        
        // Add the field to our struct
        struct_fields.emplace_back(field_name, field_type);
    }
    
    if (struct_fields.empty()) {
        return duckdb::LogicalTypeId::VARCHAR; // Fallback if no fields
    }
    
    return duckdb::LogicalType::STRUCT(struct_fields);
}

} // namespace erpl_web
