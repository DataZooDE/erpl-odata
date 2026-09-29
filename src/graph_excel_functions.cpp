#include "graph_excel_functions.hpp"
#include "graph_client.hpp"
#include "graph_excel_client.hpp"
#include "graph_sharepoint_client.hpp"
#include "graph_excel_secret.hpp"
#include "graph_json_scan.hpp"
#include "graph_output_utils.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "yyjson.hpp"
#include "erpl_odata_banner.hpp"
#include "odata_url_helpers.hpp"

using namespace duckdb_yyjson;

namespace erpl_web {

using namespace duckdb;

// ============================================================================
// Bind Data Structures
// ============================================================================

struct ListFilesBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string folder_path;
    std::string drive_id;
};

struct ExcelTablesBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string file_path;
    std::string drive_id;
};

struct ExcelWorksheetsBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string file_path;
    std::string drive_id;
};

struct ExcelTableDataBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string file_path;
    std::string table_name;
    std::string drive_id;
    // Fetched once during bind to infer the schema, then kept so every execution
    // of the bound plan can re-seed its own scan state from it (GitHub #202).
    std::string prefetched_json;
};

struct ExcelRangeBindData : public TableFunctionData {
    std::string secret_name;
    std::string file_path;
    std::string sheet_name;
    std::string range;  // Optional, empty means usedRange
    std::string drive_id;
    std::string json_response;
    vector<LogicalType> column_types;  // per-column types inferred from valueTypes
};

// Per-execution cursor for graph_excel_read_range. The parsed rows and the
// position in them must not live on the bind data: a bound plan that is
// EXECUTEd twice would otherwise return zero rows the second time (GitHub #202).
struct ExcelRangeScanState : public duckdb::GlobalTableFunctionState {
    std::vector<std::vector<duckdb::Value>> cached_rows;  // cached_rows[row][col]
    size_t current_row = 0;  // index into cached_rows
    bool rows_cached = false;
    bool done = false;

    static duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Init(duckdb::ClientContext &,
                                                                     duckdb::TableFunctionInitInput &) {
        return duckdb::make_uniq<ExcelRangeScanState>();
    }
};


// ============================================================================
// Helper Functions
// ============================================================================

// Year tokens (y/Y) only appear in date format codes, never in pure number/time formats.
// This is the reliable server-side signal that a cell column holds date values.
static bool IsDateFormatCode(const char *fmt) {
    if (!fmt) {
        return false;
    }
    bool in_literal = false;
    char literal_char = 0;
    for (const char *p = fmt; *p; ++p) {
        // Skip quoted literals: "..." or '...'
        if (*p == '"' || *p == '\'') {
            if (!in_literal) {
                in_literal = true;
                literal_char = *p;
            } else if (*p == literal_char) {
                in_literal = false;
            }
            continue;
        }
        if (in_literal) {
            continue;
        }
        if (*p == 'y' || *p == 'Y') {
            return true;
        }
    }
    return false;
}

// Convert a single yyjson cell value to a DuckDB Value of the given column type.
static duckdb::Value ExtractCellValue(yyjson_val *cell_val, yyjson_val *cell_type_val,
                                      const LogicalType &col_type) {
    const bool is_empty = !cell_val || yyjson_is_null(cell_val) ||
                          (cell_type_val && yyjson_is_str(cell_type_val) &&
                           strcmp(yyjson_get_str(cell_type_val), "Empty") == 0);
    if (is_empty) {
        return Value();
    }

    if (col_type.id() == LogicalTypeId::DATE) {
        if (yyjson_is_num(cell_val)) {
            const int32_t epoch_days = static_cast<int32_t>(yyjson_get_num(cell_val)) - 25569;
            return Value::DATE(date_t(epoch_days));
        }
        if (yyjson_is_str(cell_val)) {
            const char *s = yyjson_get_str(cell_val);
            date_t parsed;
            idx_t pos = 0;
            bool special = false;
            if (Date::TryConvertDate(s, strlen(s), pos, parsed, special) == DateCastResult::SUCCESS) {
                return Value::DATE(parsed);
            }
        }
        return Value();
    }

    if (col_type.id() == LogicalTypeId::DOUBLE && yyjson_is_num(cell_val)) {
        return Value::DOUBLE(yyjson_get_num(cell_val));
    }
    if (col_type.id() == LogicalTypeId::BOOLEAN && yyjson_is_bool(cell_val)) {
        return Value::BOOLEAN(yyjson_get_bool(cell_val));
    }

    // Fallback: stringify
    if (yyjson_is_str(cell_val)) {
        return Value(std::string(yyjson_get_str(cell_val)));
    }
    if (yyjson_is_num(cell_val)) {
        return Value(std::to_string(yyjson_get_num(cell_val)));
    }
    if (yyjson_is_bool(cell_val)) {
        return Value(yyjson_get_bool(cell_val) ? "true" : "false");
    }
    return Value();
}

// Resolves a Graph drive ID from either an explicit drive_id named param or
// site + drive named params (name-based lookup). Returns empty string if none given.
static std::string ResolveGraphDriveId(ClientContext &context,
                                       const std::string &secret_name,
                                       const TableFunctionBindInput &input) {
    const bool has_drive = input.named_parameters.find("drive") != input.named_parameters.end();
    const bool has_site  = input.named_parameters.find("site")  != input.named_parameters.end();

    if (!has_drive && !has_site) {
        return "";
    }

    if (has_drive) {
        const std::string drive_param = input.named_parameters.at("drive").GetValue<std::string>();

        // Raw drive ID (b!...) — use directly, no API call needed
        if (GraphSharePointClient::LooksLikeDriveId(drive_param)) {
            return drive_param;
        }

        // Web URL — resolve by matching webUrl field across the site's drives
        if (LooksLikeAbsoluteHttpUrl(drive_param)) {
            auto auth_info = ResolveGraphAuth(context, secret_name);
            GraphSharePointClient sp_client(auth_info.auth_params);
            return sp_client.ResolveDriveIdFromUrl(drive_param);
        }

        // Display name — requires site for lookup
        if (!has_site) {
            throw BinderException(
                "The 'drive' parameter requires 'site' when specifying a drive by name. "
                "Pass a drive ID (b!...) or URL (https://...) to omit 'site'.");
        }
    }

    // Name-based or auto-select: resolve site first, then drive within it
    auto auth_info = ResolveGraphAuth(context, secret_name);
    GraphSharePointClient sp_client(auth_info.auth_params);
    const std::string site_param  = input.named_parameters.at("site").GetValue<std::string>();
    const std::string drive_param = has_drive
        ? input.named_parameters.at("drive").GetValue<std::string>()
        : "";
    const std::string site_id = sp_client.ResolveSiteId(site_param);
    return sp_client.ResolveDriveId(site_id, drive_param);
}

// ============================================================================
// graph_show_files - List files in OneDrive
// ============================================================================

unique_ptr<FunctionData> GraphExcelFunctions::ListFilesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ListFilesBindData>();

    // Optional folder_path is first positional param
    if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
        bind_data->folder_path = input.inputs[0].GetValue<std::string>();
    }

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->drive_id = ResolveGraphDriveId(context, bind_data->secret_name, input);

    // Return schema: id, name, webUrl, size, createdDateTime, lastModifiedDateTime, mimeType, isFolder
    names = {"id", "name", "web_url", "size", "created_at", "modified_at", "mime_type", "is_folder"};
    return_types = {
        LogicalType::VARCHAR,   // id
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // web_url
        LogicalType::BIGINT,    // size
        LogicalType::VARCHAR,   // created_at
        LogicalType::VARCHAR,   // modified_at
        LogicalType::VARCHAR,   // mime_type
        LogicalType::BOOLEAN    // is_folder
    };

    return std::move(bind_data);
}

void GraphExcelFunctions::ListFilesScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ListFilesBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();
    if (state.done) { output.SetCardinality(0); return; }

    if (!state.parsed_doc) {
        if (state.json_response.empty()) {
            auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
            GraphExcelClient client(auth_info.auth_params);
            state.json_response = client.ListDriveFiles(bind_data.folder_path, bind_data.drive_id);
        }
        if (!state.InitIterator()) {
            state.done = true;
            output.SetCardinality(0);
            return;
        }
    }

    idx_t row = 0;
    yyjson_val *item;
    while (row < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "name"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "webUrl"));
        SetInt64Cell(output.data[3], row, yyjson_obj_get(item, "size"));
        SetStrCell(output.data[4], row, yyjson_obj_get(item, "createdDateTime"));
        SetStrCell(output.data[5], row, yyjson_obj_get(item, "lastModifiedDateTime"));
        yyjson_val *file_obj = yyjson_obj_get(item, "file");
        SetStrCell(output.data[6], row, file_obj ? yyjson_obj_get(file_obj, "mimeType") : nullptr);
        SetBoolCellNN(output.data[7], row, yyjson_obj_get(item, "folder") != nullptr);
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_excel_tables - List tables in Excel workbook
// ============================================================================

unique_ptr<FunctionData> GraphExcelFunctions::ExcelTablesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ExcelTablesBindData>();

    // file_path is required positional parameter
    if (input.inputs.empty()) {
        throw BinderException("graph_excel_tables requires a file_path parameter");
    }
    bind_data->file_path = input.inputs[0].GetValue<std::string>();

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->drive_id = ResolveGraphDriveId(context, bind_data->secret_name, input);

    // Return schema: name, id, showHeaders, showTotals
    names = {"name", "id", "show_headers", "show_totals"};
    return_types = {
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // id
        LogicalType::BOOLEAN,   // show_headers
        LogicalType::BOOLEAN    // show_totals
    };

    return std::move(bind_data);
}

void GraphExcelFunctions::ExcelTablesScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ExcelTablesBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();
    if (state.done) { output.SetCardinality(0); return; }

    if (!state.parsed_doc) {
        if (state.json_response.empty()) {
            auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
            GraphExcelClient client(auth_info.auth_params);
            state.json_response = client.ListTablesByPath(bind_data.file_path, bind_data.drive_id);
        }
        if (!state.InitIterator()) {
            state.done = true;
            output.SetCardinality(0);
            return;
        }
    }

    idx_t row = 0;
    yyjson_val *item;
    while (row < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "name"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "id"));
        SetBoolCell(output.data[2], row, yyjson_obj_get(item, "showHeaders"));
        SetBoolCell(output.data[3], row, yyjson_obj_get(item, "showTotals"));
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_excel_worksheets - List worksheets in Excel workbook
// ============================================================================

unique_ptr<FunctionData> GraphExcelFunctions::ExcelWorksheetsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ExcelWorksheetsBindData>();

    // file_path is required positional parameter
    if (input.inputs.empty()) {
        throw BinderException("graph_excel_worksheets requires a file_path parameter");
    }
    bind_data->file_path = input.inputs[0].GetValue<std::string>();

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->drive_id = ResolveGraphDriveId(context, bind_data->secret_name, input);

    // Return schema: name, id, position, visibility
    names = {"name", "id", "position", "visibility"};
    return_types = {
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // id
        LogicalType::INTEGER,   // position
        LogicalType::VARCHAR    // visibility
    };

    return std::move(bind_data);
}

void GraphExcelFunctions::ExcelWorksheetsScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ExcelWorksheetsBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();
    if (state.done) { output.SetCardinality(0); return; }

    if (!state.parsed_doc) {
        if (state.json_response.empty()) {
            auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
            GraphExcelClient client(auth_info.auth_params);
            state.json_response = client.ListWorksheetsByPath(bind_data.file_path, bind_data.drive_id);
        }
        if (!state.InitIterator()) {
            state.done = true;
            output.SetCardinality(0);
            return;
        }
    }

    idx_t row = 0;
    yyjson_val *item;
    while (row < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "name"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "id"));
        SetInt32Cell(output.data[2], row, yyjson_obj_get(item, "position"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "visibility"));
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_excel_range - Read Excel range data
// ============================================================================

unique_ptr<FunctionData> GraphExcelFunctions::ExcelRangeBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ExcelRangeBindData>();

    // file_path and sheet_name are required positional parameters
    if (input.inputs.size() < 2) {
        throw BinderException("graph_excel_range requires file_path and sheet_name parameters");
    }
    bind_data->file_path = input.inputs[0].GetValue<std::string>();
    bind_data->sheet_name = input.inputs[1].GetValue<std::string>();

    // Optional range parameter
    if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
        bind_data->range = input.inputs[2].GetValue<std::string>();
    }

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->drive_id = ResolveGraphDriveId(context, bind_data->secret_name, input);

    // Fetch the range data to determine column count
    auto auth_info = ResolveGraphAuth(context, bind_data->secret_name);
    GraphExcelClient client(auth_info.auth_params);

    if (bind_data->range.empty()) {
        bind_data->json_response = client.GetUsedRangeByPath(bind_data->file_path, bind_data->sheet_name, bind_data->drive_id);
    } else {
        bind_data->json_response = client.GetRangeByPath(bind_data->file_path, bind_data->sheet_name, bind_data->range, bind_data->drive_id);
    }

    // Parse to determine schema
    yyjson_doc *doc = yyjson_read(bind_data->json_response.c_str(), bind_data->json_response.length(), 0);
    if (!doc) {
        throw InvalidInputException("Failed to parse Graph API response");
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *values_arr = yyjson_obj_get(root, "values");

    if (!values_arr || !yyjson_is_arr(values_arr)) {
        yyjson_doc_free(doc);
        // Return minimal schema if no data
        names = {"value"};
        return_types = {LogicalType::VARCHAR};
        bind_data->column_types = {LogicalType::VARCHAR};
        return std::move(bind_data);
    }

    // Get first row to determine column count
    yyjson_val *first_row = yyjson_arr_get_first(values_arr);
    size_t col_count = 0;

    if (first_row && yyjson_is_arr(first_row)) {
        col_count = yyjson_arr_size(first_row);
    }

    if (col_count == 0) {
        yyjson_doc_free(doc);
        names = {"value"};
        return_types = {LogicalType::VARCHAR};
        bind_data->column_types = {LogicalType::VARCHAR};
        return std::move(bind_data);
    }

    // Use first row as headers
    auto headers = GraphJsonStringArray(first_row);

    // Infer column types from valueTypes (parallel 2D array from Graph API)
    // Each cell is one of "String", "Double", "Boolean", "Empty"
    yyjson_val *types_arr = yyjson_obj_get(root, "valueTypes");
    const size_t total_rows = yyjson_arr_size(values_arr);

    // For each column: track whether we've seen only Double, only Boolean, or mixed/String
    enum class ColKind { Unknown, Double, Boolean, String };
    std::vector<ColKind> col_kinds(col_count, ColKind::Unknown);

    // Detect date columns via numberFormat: a column with a date format code (contains
    // year token 'y'/'Y') should be returned as DATE regardless of valueTypes kind.
    // This covers both: real Excel serial dates (valueTypes="Double") and ISO date strings
    // typed into date-formatted cells (valueTypes="String").
    yyjson_val *nfmt_arr = yyjson_obj_get(root, "numberFormat");
    std::vector<bool> col_is_date(col_count, false);

    // Single combined pass over data rows (skip header at index 0) using O(N) iterators.
    // yyjson_arr_get(outer_arr, r) is O(r) on non-flat arrays (row elements are sub-arrays),
    // which makes indexed access O(N²) over 100K rows. Iterators advance O(1) per step.
    {
        const bool has_types = types_arr && yyjson_is_arr(types_arr);
        const bool has_nfmt  = nfmt_arr  && yyjson_is_arr(nfmt_arr);

        yyjson_arr_iter types_iter = {}, nfmt_iter = {};
        if (has_types) {
            yyjson_arr_iter_init(types_arr, &types_iter);
            yyjson_arr_iter_next(&types_iter); // skip header row
        }
        if (has_nfmt) {
            yyjson_arr_iter_init(nfmt_arr, &nfmt_iter);
            yyjson_arr_iter_next(&nfmt_iter);  // skip header row
        }

        for (size_t r = 1; r < total_rows; r++) {
            yyjson_val *type_row = has_types ? yyjson_arr_iter_next(&types_iter) : nullptr;
            yyjson_val *fmt_row  = has_nfmt  ? yyjson_arr_iter_next(&nfmt_iter)  : nullptr;

            const bool type_row_ok = type_row && yyjson_is_arr(type_row);
            const bool fmt_row_ok  = fmt_row  && yyjson_is_arr(fmt_row);

            yyjson_arr_iter cell_type_iter = {}, fmt_cell_iter = {};
            if (type_row_ok) { yyjson_arr_iter_init(type_row, &cell_type_iter); }
            if (fmt_row_ok)  { yyjson_arr_iter_init(fmt_row,  &fmt_cell_iter); }

            for (size_t c = 0; c < col_count; c++) {
                yyjson_val *cell_type = type_row_ok ? yyjson_arr_iter_next(&cell_type_iter) : nullptr;
                yyjson_val *fmt_val   = fmt_row_ok  ? yyjson_arr_iter_next(&fmt_cell_iter)  : nullptr;

                if (col_kinds[c] != ColKind::String && cell_type && yyjson_is_str(cell_type)) {
                    const char *t = yyjson_get_str(cell_type);
                    if (strcmp(t, "Empty") != 0) {
                        if (strcmp(t, "Double") == 0) {
                            if (col_kinds[c] == ColKind::Unknown) { col_kinds[c] = ColKind::Double; }
                            else if (col_kinds[c] != ColKind::Double) { col_kinds[c] = ColKind::String; }
                        } else if (strcmp(t, "Boolean") == 0) {
                            if (col_kinds[c] == ColKind::Unknown) { col_kinds[c] = ColKind::Boolean; }
                            else if (col_kinds[c] != ColKind::Boolean) { col_kinds[c] = ColKind::String; }
                        } else {
                            col_kinds[c] = ColKind::String;
                        }
                    }
                }

                if (!col_is_date[c] && fmt_val && yyjson_is_str(fmt_val)) {
                    if (IsDateFormatCode(yyjson_get_str(fmt_val))) {
                        col_is_date[c] = true;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < col_count; i++) {
        if (i < headers.size() && !headers[i].empty()) {
            names.push_back(headers[i]);
        } else {
            names.push_back("column_" + std::to_string(i));
        }

        LogicalType col_type = LogicalType::VARCHAR;
        if (col_is_date[i]) {
            col_type = LogicalType::DATE;
        } else if (col_kinds[i] == ColKind::Double) {
            col_type = LogicalType::DOUBLE;
        } else if (col_kinds[i] == ColKind::Boolean) {
            col_type = LogicalType::BOOLEAN;
        }
        return_types.push_back(col_type);
        bind_data->column_types.push_back(col_type);
    }

    yyjson_doc_free(doc);
    return std::move(bind_data);
}

void GraphExcelFunctions::ExcelRangeScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->Cast<ExcelRangeBindData>();
    auto &state = data.global_state->Cast<ExcelRangeScanState>();

    if (state.done) {
        output.SetCardinality(0);
        return;
    }

    // Parse the JSON response once per EXECUTION and cache all rows as DuckDB Values, so
    // repeated scan calls within one execution do not reparse it. The raw payload stays on
    // the bind data on purpose - a second execution must be able to parse it again - which
    // is why it is no longer cleared here.
    if (!state.rows_cached) {
        yyjson_doc *doc = yyjson_read(bind_data.json_response.c_str(), bind_data.json_response.length(), 0);
        if (!doc) {
            throw InvalidInputException("Failed to parse Graph API response");
        }

        yyjson_val *root       = yyjson_doc_get_root(doc);
        yyjson_val *values_arr = yyjson_obj_get(root, "values");

        if (!values_arr || !yyjson_is_arr(values_arr)) {
            yyjson_doc_free(doc);
            state.done = true;
            output.SetCardinality(0);
            return;
        }

        yyjson_val *types_arr  = yyjson_obj_get(root, "valueTypes");
        const size_t total_rows = yyjson_arr_size(values_arr);
        const size_t col_count  = bind_data.column_types.size();

        // Skip header row (index 0), extract data rows into cache.
        // Use O(N) iterators — yyjson_arr_get(outer_arr, r) on non-flat arrays is O(r),
        // making indexed access O(N²) for large ranges.
        state.cached_rows.reserve(total_rows > 1 ? total_rows - 1 : 0);

        yyjson_arr_iter row_iter;
        yyjson_arr_iter_init(values_arr, &row_iter);
        yyjson_arr_iter_next(&row_iter); // skip header row

        const bool has_types = types_arr && yyjson_is_arr(types_arr);
        yyjson_arr_iter type_iter = {};
        if (has_types) {
            yyjson_arr_iter_init(types_arr, &type_iter);
            yyjson_arr_iter_next(&type_iter); // skip header row
        }

        yyjson_val *row_arr;
        while ((row_arr = yyjson_arr_iter_next(&row_iter))) {
            yyjson_val *type_row = has_types ? yyjson_arr_iter_next(&type_iter) : nullptr;

            const bool row_ok  = yyjson_is_arr(row_arr);
            const bool type_ok = type_row && yyjson_is_arr(type_row);

            yyjson_arr_iter cell_iter = {}, cell_type_iter = {};
            if (row_ok)  { yyjson_arr_iter_init(row_arr,  &cell_iter); }
            if (type_ok) { yyjson_arr_iter_init(type_row, &cell_type_iter); }

            std::vector<duckdb::Value> row_vals;
            row_vals.reserve(col_count);
            for (size_t c = 0; c < col_count; c++) {
                yyjson_val *cell_val  = row_ok  ? yyjson_arr_iter_next(&cell_iter)      : nullptr;
                yyjson_val *cell_type = type_ok ? yyjson_arr_iter_next(&cell_type_iter) : nullptr;
                row_vals.push_back(ExtractCellValue(cell_val, cell_type, bind_data.column_types[c]));
            }
            state.cached_rows.push_back(std::move(row_vals));
        }

        yyjson_doc_free(doc);
        state.rows_cached = true;
    }

    // Serve rows from cache in STANDARD_VECTOR_SIZE chunks
    const size_t start     = state.current_row;
    const size_t remaining = (start < state.cached_rows.size())
        ? state.cached_rows.size() - start : 0;
    const size_t data_rows = std::min(remaining, static_cast<size_t>(STANDARD_VECTOR_SIZE));
    const size_t col_count = output.ColumnCount();

    output.SetCardinality(data_rows);
    for (size_t row_idx = 0; row_idx < data_rows; row_idx++) {
        const auto &row = state.cached_rows[start + row_idx];
        for (size_t col = 0; col < col_count; col++) {
            output.SetValue(col, row_idx, col < row.size() ? row[col] : Value());
        }
    }

    state.current_row += data_rows;
    if (state.current_row >= state.cached_rows.size()) {
        state.done = true;
        // The materialised Values are no longer reachable; a self-join holds one of these
        // per scan, so release them rather than keeping them until the query ends.
        state.cached_rows.clear();
        state.cached_rows.shrink_to_fit();
    }
}

// ============================================================================
// graph_excel_read - Read Excel table data
// ============================================================================

unique_ptr<FunctionData> GraphExcelFunctions::ExcelTableDataBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ExcelTableDataBindData>();

    // file_path and table_name are required positional parameters
    if (input.inputs.size() < 2) {
        throw BinderException("graph_excel_read requires file_path and table_name parameters");
    }
    bind_data->file_path = input.inputs[0].GetValue<std::string>();
    bind_data->table_name = input.inputs[1].GetValue<std::string>();

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->drive_id = ResolveGraphDriveId(context, bind_data->secret_name, input);

    // Fetch the table data to determine schema
    auto auth_info = ResolveGraphAuth(context, bind_data->secret_name);
    GraphExcelClient client(auth_info.auth_params);
    bind_data->prefetched_json = client.GetTableRowsByPath(bind_data->file_path, bind_data->table_name, bind_data->drive_id);

    // Parse to determine schema from first row
    yyjson_doc *doc = yyjson_read(bind_data->prefetched_json.c_str(), bind_data->prefetched_json.length(), 0);
    if (!doc) {
        throw InvalidInputException("Failed to parse Graph API response");
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *value_arr = yyjson_obj_get(root, "value");

    if (!value_arr || !yyjson_is_arr(value_arr) || yyjson_arr_size(value_arr) == 0) {
        yyjson_doc_free(doc);
        // Return minimal schema if no data
        names = {"value"};
        return_types = {LogicalType::VARCHAR};
        return std::move(bind_data);
    }

    // Get first row to determine column count
    yyjson_val *first_item = yyjson_arr_get_first(value_arr);
    yyjson_val *first_row_values = first_item ? yyjson_obj_get(first_item, "values") : nullptr;

    size_t col_count = 0;
    if (first_row_values && yyjson_is_arr(first_row_values)) {
        yyjson_val *inner_arr = yyjson_arr_get_first(first_row_values);
        if (inner_arr && yyjson_is_arr(inner_arr)) {
            col_count = yyjson_arr_size(inner_arr);
        }
    }

    if (col_count == 0) {
        yyjson_doc_free(doc);
        names = {"value"};
        return_types = {LogicalType::VARCHAR};
        return std::move(bind_data);
    }

    // Generate column names (table rows don't include headers, so use generic names)
    for (size_t i = 0; i < col_count; i++) {
        names.push_back("column_" + std::to_string(i));
        return_types.push_back(LogicalType::VARCHAR);
    }

    yyjson_doc_free(doc);
    return std::move(bind_data);
}

void GraphExcelFunctions::ExcelTableDataScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ExcelTableDataBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();
    if (state.done) { output.SetCardinality(0); return; }

    if (state.NeedsFetch()) {
        state.SeedFrom(bind_data.prefetched_json);
    }

    if (!state.parsed_doc) {
        if (!state.InitIterator()) {
            state.done = true;
            output.SetCardinality(0);
            return;
        }
    }

    const size_t col_count = output.ColumnCount();
    idx_t row_idx = 0;
    yyjson_val *item;

    while (row_idx < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
        yyjson_val *values_arr = yyjson_obj_get(item, "values");
        yyjson_val *inner_arr  = (values_arr && yyjson_is_arr(values_arr))
                                     ? yyjson_arr_get_first(values_arr) : nullptr;

        if (!inner_arr || !yyjson_is_arr(inner_arr)) {
            for (size_t col = 0; col < col_count; col++) {
                duckdb::FlatVector::Validity(output.data[col]).SetInvalid(row_idx);
            }
            row_idx++;
            continue;
        }

        // Each row is values: [[cell1, cell2, ...]] — iterate inner array cells
        yyjson_arr_iter cell_iter;
        yyjson_arr_iter_init(inner_arr, &cell_iter);
        for (size_t col = 0; col < col_count; col++) {
            yyjson_val *cell = yyjson_arr_iter_next(&cell_iter);
            SetStrCell(output.data[col], row_idx, cell);
        }
        row_idx++;
    }

    if (row_idx < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row_idx);
}

// ============================================================================
// graph_excel_add_rows - Append pre-serialized rows to an Excel table
// ============================================================================

struct AddRowsBindData : public TableFunctionData {
    std::string file_path;
    std::string table_name;
    std::string rows_json;   // {"values": [[...],...]}
    std::string drive_id;
    std::string secret_name;
    bool done = false;
};

unique_ptr<FunctionData> GraphExcelFunctions::AddRowsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.size() < 2) {
        throw BinderException("graph_excel_add_rows requires file_path and table_name parameters");
    }
    auto bind = make_uniq<AddRowsBindData>();
    bind->file_path  = input.inputs[0].GetValue<std::string>();
    bind->table_name = input.inputs[1].GetValue<std::string>();

    if (input.named_parameters.count("data")) {
        bind->rows_json = input.named_parameters.at("data").GetValue<std::string>();
    }
    if (input.named_parameters.count("secret")) {
        bind->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind->drive_id = ResolveGraphDriveId(context, bind->secret_name, input);

    return_types = {LogicalType::BIGINT};
    names        = {"rows_added"};
    return std::move(bind);
}

void GraphExcelFunctions::AddRowsScan(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output) {

    // One-shot by design: the write happens here, so the flag lives on the bind data
    // deliberately and a second EXECUTE of a bound plan adds nothing further rather than
    // duplicating rows in the workbook. This is NOT the #202 read defect.
    auto &bind = data_p.bind_data->CastNoConst<AddRowsBindData>();
    if (bind.done) { output.SetCardinality(0); return; }
    bind.done = true;

    if (bind.rows_json.empty()) {
        output.SetCardinality(1);
        output.SetValue(0, 0, Value::BIGINT(0));
        return;
    }

    auto auth_info = ResolveGraphAuth(context, bind.secret_name);
    GraphExcelClient client(auth_info.auth_params);
    const idx_t n = client.AddTableRows(bind.file_path, bind.table_name, bind.rows_json, bind.drive_id);
    output.SetCardinality(1);
    output.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(n)));
}

// ============================================================================
// graph_excel_delete_rows - Delete rows matching a column value
// ============================================================================

struct DeleteRowsBindData : public TableFunctionData {
    std::string file_path;
    std::string table_name;
    idx_t col_index = 0;
    std::string col_value;
    std::string drive_id;
    std::string secret_name;
    bool by_name = false;   // when true, col_name is resolved to col_index at scan time
    std::string col_name;
    bool done = false;
};

unique_ptr<FunctionData> GraphExcelFunctions::DeleteRowsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.size() < 4) {
        throw BinderException("graph_excel_delete_rows requires file_path, table_name, col_index, col_value");
    }
    auto bind = make_uniq<DeleteRowsBindData>();
    bind->file_path  = input.inputs[0].GetValue<std::string>();
    bind->table_name = input.inputs[1].GetValue<std::string>();
    bind->col_index  = static_cast<idx_t>(input.inputs[2].GetValue<int64_t>());
    bind->col_value  = input.inputs[3].GetValue<std::string>();

    if (input.named_parameters.count("secret")) {
        bind->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind->drive_id = ResolveGraphDriveId(context, bind->secret_name, input);

    return_types = {LogicalType::BIGINT};
    names        = {"rows_deleted"};
    return std::move(bind);
}

unique_ptr<FunctionData> GraphExcelFunctions::DeleteRowsByNameBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.size() < 4) {
        throw BinderException("graph_excel_delete_rows requires file_path, table_name, column, col_value");
    }
    auto bind = make_uniq<DeleteRowsBindData>();
    bind->file_path  = input.inputs[0].GetValue<std::string>();
    bind->table_name = input.inputs[1].GetValue<std::string>();
    bind->by_name    = true;
    bind->col_name   = input.inputs[2].GetValue<std::string>();
    bind->col_value  = input.inputs[3].GetValue<std::string>();

    if (input.named_parameters.count("secret")) {
        bind->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind->drive_id = ResolveGraphDriveId(context, bind->secret_name, input);

    return_types = {LogicalType::BIGINT};
    names        = {"rows_deleted"};
    return std::move(bind);
}

idx_t GraphExcelFunctions::ResolveColumnIndex(const std::vector<std::string> &columns,
                                              const std::string &column_ref) {
    // Exact name match first.
    for (idx_t i = 0; i < columns.size(); i++) {
        if (columns[i] == column_ref) {
            return i;
        }
    }
    // Case-insensitive name match.
    const std::string lowered_ref = StringUtil::Lower(column_ref);
    for (idx_t i = 0; i < columns.size(); i++) {
        if (StringUtil::Lower(columns[i]) == lowered_ref) {
            return i;
        }
    }
    // Fallback: a purely numeric reference is treated as a 0-based index.
    if (!column_ref.empty() &&
        column_ref.find_first_not_of("0123456789") == std::string::npos) {
        return static_cast<idx_t>(std::stoull(column_ref));
    }
    std::string available;
    for (idx_t i = 0; i < columns.size(); i++) {
        if (i > 0) { available += ", "; }
        available += columns[i];
    }
    throw InvalidInputException(
        "graph_excel_delete_rows: column '%s' not found. Available columns: %s",
        column_ref, available);
}

void GraphExcelFunctions::DeleteRowsScan(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output) {

    // One-shot by design - see the note in AddRowsScan. A second EXECUTE deletes nothing
    // further rather than deleting another row.
    auto &bind = data_p.bind_data->CastNoConst<DeleteRowsBindData>();
    if (bind.done) { output.SetCardinality(0); return; }
    bind.done = true;

    auto auth_info = ResolveGraphAuth(context, bind.secret_name);
    GraphExcelClient client(auth_info.auth_params);

    idx_t col_index = bind.col_index;
    if (bind.by_name) {
        const auto columns = client.GetTableColumnsByPath(bind.file_path, bind.table_name, bind.drive_id);
        col_index = ResolveColumnIndex(columns, bind.col_name);
        ERPL_TRACE_DEBUG("GRAPH_EXCEL",
                         "Resolved delete column '" + bind.col_name + "' to index " + std::to_string(col_index));
    }

    const idx_t n = client.DeleteTableRowsMatchingColumn(
        bind.file_path, bind.table_name, col_index, bind.col_value, bind.drive_id);
    output.SetCardinality(1);
    output.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(n)));
}

// ============================================================================
// Registration
// ============================================================================

void GraphExcelFunctions::Register(ExtensionLoader &loader) {
    ERPL_TRACE_INFO("GRAPH_EXCEL", "Registering Microsoft Graph Excel functions");

    {
        TableFunction list_files("graph_show_files", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ListFilesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ListFilesBind));
        list_files.varargs = LogicalType::VARCHAR;
        list_files.named_parameters["secret"] = LogicalType::VARCHAR;
        list_files.named_parameters["drive"] = LogicalType::VARCHAR;
        list_files.named_parameters["site"] = LogicalType::VARCHAR;
        list_files.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(list_files);
        FunctionDescription desc;
        desc.description = "List files and folders in OneDrive or a SharePoint document library. "
                           "Returns id, name, web_url, size, created_at, modified_at, mime_type, is_folder. "
                           "Locate the drive with graph_show_drives() first. "
                           "Pass drive as a drive ID (b!...), a web URL, or a display name combined with site.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples = {
            "SELECT * FROM graph_show_files(drive := 'b!abc...', secret := 'ms_graph')",
            "SELECT * FROM graph_show_files(drive := 'https://tenant.sharepoint.com/Shared%20Documents', secret := 'ms_graph')",
            "SELECT * FROM graph_show_files(site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "excel"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction excel_tables("graph_excel_tables", {LogicalType::VARCHAR},
                                   DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelTablesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelTablesBind));
        excel_tables.named_parameters["secret"] = LogicalType::VARCHAR;
        excel_tables.named_parameters["drive"] = LogicalType::VARCHAR;
        excel_tables.named_parameters["site"] = LogicalType::VARCHAR;
        excel_tables.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(excel_tables);
        FunctionDescription desc;
        desc.description = "List all named tables in a Microsoft Excel workbook. "
                           "Returns table_name, sheet_name, row_count, column_count. "
                           "Use the returned table_name with graph_excel_read() to read table data.";
        desc.parameter_names = {"file_path", "secret", "drive", "site"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                 LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_excel_tables('test.xlsx', drive := 'b!abc...', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_tables('test.xlsx', drive := 'https://tenant.sharepoint.com/Shared%20Documents', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_tables('test.xlsx', site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "excel"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction excel_worksheets("graph_excel_worksheets", {LogicalType::VARCHAR},
                                       DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelWorksheetsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelWorksheetsBind));
        excel_worksheets.named_parameters["secret"] = LogicalType::VARCHAR;
        excel_worksheets.named_parameters["drive"] = LogicalType::VARCHAR;
        excel_worksheets.named_parameters["site"] = LogicalType::VARCHAR;
        excel_worksheets.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(excel_worksheets);
        FunctionDescription desc;
        desc.description = "List all worksheets in a Microsoft Excel workbook. "
                           "Returns sheet_name, position, visibility. "
                           "Use the returned sheet_name with graph_excel_range() to read a cell range.";
        desc.parameter_names = {"file_path", "secret", "drive", "site"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                 LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_excel_worksheets('test.xlsx', drive := 'b!abc...', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_worksheets('test.xlsx', drive := 'https://tenant.sharepoint.com/Shared%20Documents', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_worksheets('test.xlsx', site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "excel"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction excel_range("graph_excel_range",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                  DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelRangeScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelRangeBind));
        excel_range.varargs = LogicalType::VARCHAR;
        excel_range.named_parameters["secret"] = LogicalType::VARCHAR;
        excel_range.named_parameters["drive"] = LogicalType::VARCHAR;
        excel_range.named_parameters["site"] = LogicalType::VARCHAR;
        excel_range.init_global = ExcelRangeScanState::Init;
        CreateTableFunctionInfo info(excel_range);
        FunctionDescription desc;
        desc.description = "Read a cell range from a worksheet in a Microsoft Excel workbook. "
                           "Returns dynamic columns named by the first header row. "
                           "sheet_name must match exactly (use graph_excel_worksheets() to discover names). "
                           "Optionally pass a range address as a third positional argument (e.g. 'A1:D100'); "
                           "omit it to read the sheet's used range. "
                           "The range is fetched once while the statement is bound, in order to infer the "
                           "column types; re-executing a prepared statement replays that snapshot rather "
                           "than re-reading the workbook.";
        desc.parameter_names = {"file_path", "sheet_name", "secret", "drive", "site"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                                 LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_excel_range('test.xlsx', 'Sheet1', drive := 'b!abc...', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_range('test.xlsx', 'Sheet1', drive := 'https://tenant.sharepoint.com/Shared%20Documents', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_range('test.xlsx', 'Sheet1', site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "excel"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction excel_table_data("graph_excel_read",
                                       {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                       DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelTableDataScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ExcelTableDataBind));
        excel_table_data.named_parameters["secret"] = LogicalType::VARCHAR;
        excel_table_data.named_parameters["drive"] = LogicalType::VARCHAR;
        excel_table_data.named_parameters["site"] = LogicalType::VARCHAR;
        excel_table_data.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(excel_table_data);
        FunctionDescription desc;
        desc.description = "Read all rows from a named table in a Microsoft Excel workbook. "
                           "Returns dynamic columns matching the table's header row. "
                           "table_name must match a name returned by graph_excel_tables(). "
                           "Use graph_excel_tables() to discover available table names first. "
                           "The rows are fetched once while the statement is bound, in order to infer the "
                           "column types; re-executing a prepared statement replays that snapshot rather "
                           "than re-reading the workbook.";
        desc.parameter_names = {"file_path", "table_name", "secret", "drive", "site"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                                 LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_excel_read('test.xlsx', 'SalesTable', drive := 'b!abc...', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_read('test.xlsx', 'SalesTable', drive := 'https://tenant.sharepoint.com/Shared%20Documents', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_read('test.xlsx', 'SalesTable', site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "excel"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }

    // graph_excel_add_rows: append rows to an Excel table (see AddRowsBind/AddRowsScan below)
    {
        TableFunction add_rows("graph_excel_add_rows",
                               {LogicalType::VARCHAR, LogicalType::VARCHAR},
                               GraphExcelFunctions::AddRowsScan,
                               GraphExcelFunctions::AddRowsBind);
        add_rows.named_parameters["data"]   = LogicalType::VARCHAR;
        add_rows.named_parameters["drive"]  = LogicalType::VARCHAR;
        add_rows.named_parameters["secret"] = LogicalType::VARCHAR;

        CreateTableFunctionInfo info(add_rows);
        FunctionDescription desc;
        desc.description = "Append rows to a named table in a Microsoft Excel workbook. "
                           "Returns rows_added. "
                           "data is a JSON 2-D array where each inner array is one row in column order: "
                           "'[[v1,v2,...],[v1,v2,...],...]'. "
                           "Column order must match the table's column order (use graph_excel_tables() to verify).";
        desc.parameter_names = {"file_path", "table_name"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_excel_add_rows('report.xlsx', 'Sales', "
            "data := '[[1,\"Alice\",100.0],[2,\"Bob\",200.0]]', "
            "drive := 'https://tenant.sharepoint.com/Shared%20Documents', secret := 'ms_graph')",
            "SELECT * FROM graph_excel_add_rows('report.xlsx', 'Sales', "
            "data := '[[1,\"Alice\"]]', "
            "site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "excel"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }

    // graph_excel_delete_rows: delete rows matching a column value. Two overloads share the same
    // name: the column may be given as a 0-based index (BIGINT) or as a column name (VARCHAR),
    // which is resolved against the table header at scan time.
    {
        const auto add_named_params = [](TableFunction &fn) {
            fn.named_parameters["drive"]  = LogicalType::VARCHAR;
            fn.named_parameters["secret"] = LogicalType::VARCHAR;
            fn.named_parameters["site"]   = LogicalType::VARCHAR;
        };

        TableFunctionSet del_rows_set("graph_excel_delete_rows");

        TableFunction del_by_index("graph_excel_delete_rows",
                                   {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                    LogicalType::BIGINT,  LogicalType::VARCHAR},
                                   GraphExcelFunctions::DeleteRowsScan,
                                   GraphExcelFunctions::DeleteRowsBind);
        add_named_params(del_by_index);
        del_rows_set.AddFunction(del_by_index);

        TableFunction del_by_name("graph_excel_delete_rows",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::VARCHAR, LogicalType::VARCHAR},
                                  GraphExcelFunctions::DeleteRowsScan,
                                  GraphExcelFunctions::DeleteRowsByNameBind);
        add_named_params(del_by_name);
        del_rows_set.AddFunction(del_by_name);

        CreateTableFunctionInfo info(del_rows_set);

        // ONE description per overload, each keyed by its exact parameter_types.
        //
        // There used to be a single description pinned to all-VARCHAR types. That
        // matched del_by_name and was DISQUALIFIED for del_by_index, whose third
        // argument is BIGINT -- CalcDescriptionSpecificity rejects a concrete type
        // mismatch outright. The effect is silent: the description is simply not
        // applied, and duckdb_functions() reports that overload as undocumented while
        // the source plainly documents it.
        const string shared =
            "Delete all rows in an Excel table where a column value matches. "
            "Returns rows_deleted. "
            "col_value is always compared as a string; cast numeric IDs to VARCHAR if needed.";
        {
            FunctionDescription desc;
            desc.description = shared + " The column is given by NAME, resolved against the table header.";
            desc.parameter_names = {"file_path", "table_name", "column", "col_value"};
            desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                    LogicalType::VARCHAR, LogicalType::VARCHAR};
            desc.examples = {
                "SELECT * FROM graph_excel_delete_rows('report.xlsx', 'Sales', 'Region', 'North', "
                "drive := 'b!abc...', secret := 'ms_graph')"
            };
            desc.categories = {"microsoft", "graph", "excel"};
            info.descriptions.push_back(std::move(desc));
        }
        {
            FunctionDescription desc;
            desc.description = shared + " The column is given by 0-based integer INDEX.";
            desc.parameter_names = {"file_path", "table_name", "column_index", "col_value"};
            desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                    LogicalType::BIGINT, LogicalType::VARCHAR};
            desc.examples = {
                "SELECT * FROM graph_excel_delete_rows('report.xlsx', 'Sales', 0, 'obsolete_row', "
                "site := 'Finance', drive := 'Documents', secret := 'ms_graph')"
            };
            desc.categories = {"microsoft", "graph", "excel"};
            info.descriptions.push_back(std::move(desc));
        }
        loader.RegisterFunction(std::move(info));
    }

    ERPL_TRACE_INFO("GRAPH_EXCEL", "Successfully registered Microsoft Graph Excel functions");
}

} // namespace erpl_web
