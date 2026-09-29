#include "graph_sharepoint_functions.hpp"
#include "graph_client.hpp"
#include "graph_sharepoint_client.hpp"
#include "graph_sharepoint_type_mapper.hpp"
#include "graph_excel_secret.hpp"
#include "graph_json_scan.hpp"
#include "graph_output_utils.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "yyjson.hpp"
#include <algorithm>
#include <unordered_set>
#include "erpl_odata_banner.hpp"

using namespace duckdb_yyjson;

namespace erpl_web {

using namespace duckdb;

// ============================================================================
// Bind Data Structures
// ============================================================================

struct ShowSitesBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string search_query;
};

struct ShowListsBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string site_id;
};

struct ShowDrivesBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string site_id;
};

struct DescribeListBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string site_id;
    std::string list_id;
};

struct ListItemsBindData : public duckdb::TableFunctionData {
    std::string secret_name;
    std::string site_id;
    std::string list_id;
    std::vector<std::string> column_names;
    std::vector<LogicalType> column_types;
};

// ============================================================================
// graph_show_sites - Search/list SharePoint sites
// ============================================================================

unique_ptr<FunctionData> GraphSharePointFunctions::ShowSitesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ShowSitesBindData>();

    // Optional search query is first positional param
    if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
        bind_data->search_query = input.inputs[0].GetValue<std::string>();
    }

    // Get secret name from named parameter (optional)
    std::string secret_name;
    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->secret_name = secret_name;

    // Return schema: id, name, displayName, webUrl, createdDateTime
    names = {"id", "name", "display_name", "web_url", "created_at"};
    return_types = {
        LogicalType::VARCHAR,   // id
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // display_name
        LogicalType::VARCHAR,   // web_url
        LogicalType::VARCHAR    // created_at
    };

    return std::move(bind_data);
}

void GraphSharePointFunctions::ShowSitesScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ShowSitesBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

    if (state.done) {
        output.SetCardinality(0);
        return;
    }

    if (!state.parsed_doc && state.json_response.empty()) {
        auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
        GraphSharePointClient client(auth_info.auth_params);
        state.json_response = client.SearchSites(bind_data.search_query);
    }

    if (!state.parsed_doc) {
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
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "webUrl"));
        SetStrCell(output.data[4], row, yyjson_obj_get(item, "createdDateTime"));
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_show_lists - List SharePoint lists in a site
// ============================================================================

unique_ptr<FunctionData> GraphSharePointFunctions::ShowListsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ShowListsBindData>();

    // Accept either a positional site_id or named site := 'name'
    if (input.named_parameters.find("site") != input.named_parameters.end()) {
        bind_data->site_id = input.named_parameters.at("site").GetValue<std::string>();
    } else if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
        bind_data->site_id = input.inputs[0].GetValue<std::string>();
    } else {
        throw BinderException("graph_show_lists requires either a positional site_id or site := 'name'");
    }

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }

    // Return schema: id, name, displayName, webUrl, createdDateTime, lastModifiedDateTime
    names = {"id", "name", "display_name", "description", "web_url", "created_at", "modified_at"};
    return_types = {
        LogicalType::VARCHAR,   // id
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // display_name
        LogicalType::VARCHAR,   // description
        LogicalType::VARCHAR,   // web_url
        LogicalType::VARCHAR,   // created_at
        LogicalType::VARCHAR    // modified_at
    };

    return std::move(bind_data);
}

void GraphSharePointFunctions::ShowListsScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ShowListsBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

    if (state.done) {
        output.SetCardinality(0);
        return;
    }

    if (!state.parsed_doc && state.json_response.empty()) {
        auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
        GraphSharePointClient client(auth_info.auth_params);
        state.json_response = client.ListLists(client.ResolveSiteId(bind_data.site_id));
    }

    if (!state.parsed_doc) {
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
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "description"));
        SetStrCell(output.data[4], row, yyjson_obj_get(item, "webUrl"));
        SetStrCell(output.data[5], row, yyjson_obj_get(item, "createdDateTime"));
        SetStrCell(output.data[6], row, yyjson_obj_get(item, "lastModifiedDateTime"));
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_describe_list - Get list columns/schema
// ============================================================================

unique_ptr<FunctionData> GraphSharePointFunctions::DescribeListBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<DescribeListBindData>();

    // site_id and list_id are required positional parameters
    if (input.inputs.size() < 2) {
        throw BinderException("graph_describe_list requires site_id and list_id parameters");
    }
    bind_data->site_id = input.inputs[0].GetValue<std::string>();
    bind_data->list_id = input.inputs[1].GetValue<std::string>();

    // Get secret name from named parameter (optional)
    std::string secret_name;
    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->secret_name = secret_name;

    // Resolve site name/URL and list name to GUIDs here rather than in the scan: bind data
    // is shared across executions, so a scan that wrote resolved ids back into it would
    // re-issue both round trips on every execution and mutate shared state (ListItemsBind
    // already resolves at bind for the same reason).
    {
        auto auth_info = ResolveGraphAuth(context, bind_data->secret_name);
        GraphSharePointClient client(auth_info.auth_params);
        bind_data->site_id = client.ResolveSiteId(bind_data->site_id);
        bind_data->list_id = client.ResolveListId(bind_data->site_id, bind_data->list_id);
    }

    // Return schema: name, displayName, columnType, description, required
    names = {"name", "display_name", "column_type", "description", "required"};
    return_types = {
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // display_name
        LogicalType::VARCHAR,   // column_type
        LogicalType::VARCHAR,   // description
        LogicalType::BOOLEAN    // required
    };

    return std::move(bind_data);
}

void GraphSharePointFunctions::DescribeListScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->Cast<DescribeListBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

    if (state.done) {
        output.SetCardinality(0);
        return;
    }

    if (!state.parsed_doc && state.json_response.empty()) {
        auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
        GraphSharePointClient client(auth_info.auth_params);
        state.json_response = client.GetListColumns(bind_data.site_id, bind_data.list_id);
    }

    if (!state.parsed_doc) {
        if (!state.InitIterator()) {
            state.done = true;
            output.SetCardinality(0);
            return;
        }
    }

    idx_t row = 0;
    yyjson_val *item;
    while (row < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
        // name
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "name"));

        // displayName
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "displayName"));

        // Column type - check various type indicators
        const char *column_type = "unknown";
        if (yyjson_obj_get(item, "text")) {
            column_type = "text";
        } else if (yyjson_obj_get(item, "number")) {
            column_type = "number";
        } else if (yyjson_obj_get(item, "dateTime")) {
            column_type = "dateTime";
        } else if (yyjson_obj_get(item, "boolean")) {
            column_type = "boolean";
        } else if (yyjson_obj_get(item, "choice")) {
            column_type = "choice";
        } else if (yyjson_obj_get(item, "lookup")) {
            column_type = "lookup";
        } else if (yyjson_obj_get(item, "personOrGroup")) {
            column_type = "personOrGroup";
        } else if (yyjson_obj_get(item, "currency")) {
            column_type = "currency";
        } else if (yyjson_obj_get(item, "calculated")) {
            column_type = "calculated";
        }
        SetStrCellNN(output.data[2], row, column_type);

        // description
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "description"));

        // required
        yyjson_val *req_val = yyjson_obj_get(item, "required");
        if (req_val && yyjson_is_bool(req_val)) {
            SetBoolCellNN(output.data[4], row, yyjson_get_bool(req_val));
        } else {
            SetBoolCellNN(output.data[4], row, false);
        }

        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_sharepoint_list_read - Read list items
// ============================================================================

unique_ptr<FunctionData> GraphSharePointFunctions::ListItemsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ListItemsBindData>();

    // site_id and list_id are required positional parameters
    if (input.inputs.size() < 2) {
        throw BinderException("graph_sharepoint_list_read requires site_id and list_id parameters");
    }
    bind_data->site_id = input.inputs[0].GetValue<std::string>();
    bind_data->list_id = input.inputs[1].GetValue<std::string>();

    // Get secret name from named parameter (optional)
    std::string secret_name;
    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }
    bind_data->secret_name = secret_name;

    // Resolve site URL/name → GUID and list name → GUID
    auto auth_info = ResolveGraphAuth(context, bind_data->secret_name);
    GraphSharePointClient client(auth_info.auth_params);

    bind_data->site_id = client.ResolveSiteId(bind_data->site_id);
    bind_data->list_id = client.ResolveListId(bind_data->site_id, bind_data->list_id);

    // Get columns
    auto columns_json = client.GetListColumns(bind_data->site_id, bind_data->list_id);

    yyjson_doc *col_doc = yyjson_read(columns_json.c_str(), columns_json.length(), 0);
    if (!col_doc) {
        throw InvalidInputException("Failed to parse list columns response");
    }

    yyjson_val *col_root = yyjson_doc_get_root(col_doc);
    yyjson_val *col_arr = yyjson_obj_get(col_root, "value");

    // Always include id as first column (always VARCHAR — it's the Graph item GUID)
    names.push_back("id");
    return_types.push_back(LogicalType::VARCHAR);
    bind_data->column_names.push_back("id");
    bind_data->column_types.push_back(LogicalType::VARCHAR);

    // Track seen names (lowercase) to prevent duplicate column errors
    std::unordered_set<std::string> seen_names = {"id"};

    if (col_arr && yyjson_is_arr(col_arr)) {
        size_t idx, max;
        yyjson_val *col;

        yyjson_arr_foreach(col_arr, idx, max, col) {
            yyjson_val *name_val = yyjson_obj_get(col, "name");
            if (name_val && yyjson_is_str(name_val)) {
                std::string col_name = yyjson_get_str(name_val);

                // Skip internal columns
                if (col_name.empty() || col_name[0] == '_') continue;
                if (col_name == "Edit" || col_name == "LinkTitle" ||
                    col_name == "LinkTitleNoMenu" || col_name == "DocIcon" ||
                    col_name == "ItemChildCount" || col_name == "FolderChildCount" ||
                    col_name == "AppAuthor" || col_name == "AppEditor") {
                    continue;
                }

                // Skip case-insensitive duplicates (e.g. "ID" collides with "id")
                std::string lower_name = col_name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                if (!seen_names.insert(lower_name).second) continue;

                const LogicalType col_type = SharePointColumnToDuckDBType(col);
                names.push_back(col_name);
                return_types.push_back(col_type);
                bind_data->column_names.push_back(col_name);
                bind_data->column_types.push_back(col_type);
            }
        }
    }

    yyjson_doc_free(col_doc);

    // If we only have the id column, add a default "fields" column
    if (names.size() == 1) {
        names.push_back("fields");
        return_types.push_back(LogicalType::VARCHAR);
        bind_data->column_names.push_back("fields");
        bind_data->column_types.push_back(LogicalType::VARCHAR);
    }

    return std::move(bind_data);
}

void GraphSharePointFunctions::ListItemsScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ListItemsBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

    if (state.done) {
        output.SetCardinality(0);
        return;
    }

    if (state.NeedsFetch()) {
        auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
        GraphSharePointClient client(auth_info.auth_params);
        state.json_response = client.GetListItems(bind_data.site_id, bind_data.list_id);
    }

    if (!state.parsed_doc) {
        if (!state.InitIterator()) {
            state.done = true;
            output.SetCardinality(0);
            return;
        }
    }

    const size_t col_count = bind_data.column_names.size();
    idx_t row = 0;
    yyjson_val *item;

    while (row < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
        yyjson_val *fields_obj = yyjson_obj_get(item, "fields");

        for (size_t col = 0; col < col_count; col++) {
            const std::string &col_name = bind_data.column_names[col];
            auto &vec = output.data[col];

            yyjson_val *field_val = (col_name == "id")
                ? yyjson_obj_get(item, "id")
                : (fields_obj ? yyjson_obj_get(fields_obj, col_name.c_str()) : nullptr);

            WriteSharePointField(vec, row, field_val, bind_data.column_types[col]);
        }
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_show_drives - List document library drives in a SharePoint site
// ============================================================================

unique_ptr<FunctionData> GraphSharePointFunctions::ShowDrivesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data = make_uniq<ShowDrivesBindData>();

    // Accept either a positional site_id or named site := 'name'
    if (input.named_parameters.find("site") != input.named_parameters.end()) {
        bind_data->site_id = input.named_parameters.at("site").GetValue<std::string>();
    } else if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
        bind_data->site_id = input.inputs[0].GetValue<std::string>();
    } else {
        throw BinderException("graph_show_drives requires either a positional site_id or site := 'name'");
    }

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }

    names = {"id", "name", "drive_type", "web_url", "created_at", "modified_at"};
    return_types = {
        LogicalType::VARCHAR,   // id
        LogicalType::VARCHAR,   // name
        LogicalType::VARCHAR,   // drive_type
        LogicalType::VARCHAR,   // web_url
        LogicalType::VARCHAR,   // created_at
        LogicalType::VARCHAR    // modified_at
    };

    return std::move(bind_data);
}

void GraphSharePointFunctions::ShowDrivesScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &bind_data = data.bind_data->CastNoConst<ShowDrivesBindData>();
    auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

    if (state.done) {
        output.SetCardinality(0);
        return;
    }

    if (!state.parsed_doc && state.json_response.empty()) {
        auto auth_info = ResolveGraphAuth(context, bind_data.secret_name);
        GraphSharePointClient client(auth_info.auth_params);
        state.json_response = client.ListDrives(client.ResolveSiteId(bind_data.site_id));
    }

    if (!state.parsed_doc) {
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
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "driveType"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "webUrl"));
        SetStrCell(output.data[4], row, yyjson_obj_get(item, "createdDateTime"));
        SetStrCell(output.data[5], row, yyjson_obj_get(item, "lastModifiedDateTime"));
        row++;
    }

    if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
    output.SetCardinality(row);
}

// ============================================================================
// graph_sharepoint_create_item / update_item / delete_item
// ============================================================================

struct CreateItemBindData : public TableFunctionData {
    std::string secret_name;
    std::string site_id;
    std::string list_id;
    std::string fields_json;
    std::string new_item_id;
};

unique_ptr<FunctionData> GraphSharePointFunctions::CreateItemBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.size() < 3) {
        throw BinderException("graph_sharepoint_create_item requires site, list, and fields_json parameters");
    }

    auto bind_data = make_uniq<CreateItemBindData>();
    bind_data->site_id    = input.inputs[0].GetValue<std::string>();
    bind_data->list_id    = input.inputs[1].GetValue<std::string>();
    bind_data->fields_json = input.inputs[2].GetValue<std::string>();

    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        bind_data->secret_name = input.named_parameters.at("secret").GetValue<std::string>();
    }

    auto auth_info = ResolveGraphAuth(context, bind_data->secret_name);
    GraphSharePointClient client(auth_info.auth_params);
    bind_data->site_id = client.ResolveSiteId(bind_data->site_id);
    bind_data->list_id = client.ResolveListId(bind_data->site_id, bind_data->list_id);
    bind_data->new_item_id = client.CreateListItem(bind_data->site_id, bind_data->list_id, bind_data->fields_json);

    names        = {"item_id"};
    return_types = {LogicalType::VARCHAR};

    return std::move(bind_data);
}

void GraphSharePointFunctions::CreateItemScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output) {

    // The item is created in CreateItemBind, so this scan only emits the resulting id.
    // The one-shot flag is therefore a per-execution emit cursor: a second EXECUTE of the
    // bound plan re-emits the same id and creates nothing further.
    auto &bind_data = data.bind_data->Cast<CreateItemBindData>();
    auto &state = data.global_state->Cast<ScanRowCursorState>();
    if (state.finished) {
        output.SetCardinality(0);
        return;
    }
    state.finished = true;
    output.SetValue(0, 0, Value(bind_data.new_item_id));
    output.SetCardinality(1);
}

void GraphSharePointFunctions::UpdateItemExecute(
    DataChunk &args,
    ExpressionState &state,
    Vector &result) {

    auto &context = state.GetContext();
    idx_t count = args.size();

    std::string secret_name;
    if (args.ColumnCount() > 4 && count > 0) {
        UnifiedVectorFormat secret_fmt;
        args.data[4].ToUnifiedFormat(count, secret_fmt);
        auto idx = secret_fmt.sel->get_index(0);
        if (secret_fmt.validity.RowIsValid(idx)) {
            secret_name = ((string_t *)secret_fmt.data)[idx].GetString(); // NOLINT
        }
    }

    auto auth_info = ResolveGraphAuth(context, secret_name);
    GraphSharePointClient client(auth_info.auth_params);

    UnifiedVectorFormat site_fmt, list_fmt, item_fmt, fields_fmt;
    args.data[0].ToUnifiedFormat(count, site_fmt);
    args.data[1].ToUnifiedFormat(count, list_fmt);
    args.data[2].ToUnifiedFormat(count, item_fmt);
    args.data[3].ToUnifiedFormat(count, fields_fmt);

    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto result_data = FlatVector::GetData<bool>(result);
    auto &result_validity = FlatVector::Validity(result);

    for (idx_t i = 0; i < count; i++) {
        auto site_idx   = site_fmt.sel->get_index(i);
        auto list_idx   = list_fmt.sel->get_index(i);
        auto item_idx   = item_fmt.sel->get_index(i);
        auto fields_idx = fields_fmt.sel->get_index(i);

        if (!site_fmt.validity.RowIsValid(site_idx) || !list_fmt.validity.RowIsValid(list_idx) ||
            !item_fmt.validity.RowIsValid(item_idx) || !fields_fmt.validity.RowIsValid(fields_idx)) {
            result_validity.SetInvalid(i);
            continue;
        }

        std::string site_id    = ((string_t *)site_fmt.data)[site_idx].GetString(); // NOLINT
        std::string list_id    = ((string_t *)list_fmt.data)[list_idx].GetString(); // NOLINT
        std::string item_id    = ((string_t *)item_fmt.data)[item_idx].GetString(); // NOLINT
        std::string fields_json = ((string_t *)fields_fmt.data)[fields_idx].GetString(); // NOLINT

        auto resolved_site = client.ResolveSiteId(site_id);
        auto resolved_list = client.ResolveListId(resolved_site, list_id);
        client.UpdateListItem(resolved_site, resolved_list, item_id, fields_json);
        result_data[i] = true;
    }
}

void GraphSharePointFunctions::DeleteItemExecute(
    DataChunk &args,
    ExpressionState &state,
    Vector &result) {

    auto &context = state.GetContext();
    idx_t count = args.size();

    std::string secret_name;
    if (args.ColumnCount() > 3 && count > 0) {
        UnifiedVectorFormat secret_fmt;
        args.data[3].ToUnifiedFormat(count, secret_fmt);
        auto idx = secret_fmt.sel->get_index(0);
        if (secret_fmt.validity.RowIsValid(idx)) {
            secret_name = ((string_t *)secret_fmt.data)[idx].GetString(); // NOLINT
        }
    }

    auto auth_info = ResolveGraphAuth(context, secret_name);
    GraphSharePointClient client(auth_info.auth_params);

    UnifiedVectorFormat site_fmt, list_fmt, item_fmt;
    args.data[0].ToUnifiedFormat(count, site_fmt);
    args.data[1].ToUnifiedFormat(count, list_fmt);
    args.data[2].ToUnifiedFormat(count, item_fmt);

    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto result_data = FlatVector::GetData<bool>(result);
    auto &result_validity = FlatVector::Validity(result);

    for (idx_t i = 0; i < count; i++) {
        auto site_idx = site_fmt.sel->get_index(i);
        auto list_idx = list_fmt.sel->get_index(i);
        auto item_idx = item_fmt.sel->get_index(i);

        if (!site_fmt.validity.RowIsValid(site_idx) || !list_fmt.validity.RowIsValid(list_idx) ||
            !item_fmt.validity.RowIsValid(item_idx)) {
            result_validity.SetInvalid(i);
            continue;
        }

        std::string site_id = ((string_t *)site_fmt.data)[site_idx].GetString(); // NOLINT
        std::string list_id = ((string_t *)list_fmt.data)[list_idx].GetString(); // NOLINT
        std::string item_id = ((string_t *)item_fmt.data)[item_idx].GetString(); // NOLINT

        auto resolved_site = client.ResolveSiteId(site_id);
        auto resolved_list = client.ResolveListId(resolved_site, list_id);
        client.DeleteListItem(resolved_site, resolved_list, item_id);
        result_data[i] = true;
    }
}

// ============================================================================
// Registration
// ============================================================================

void GraphSharePointFunctions::Register(ExtensionLoader &loader) {
    ERPL_TRACE_INFO("GRAPH_SHAREPOINT", "Registering Microsoft Graph SharePoint functions");

    {
        TableFunction show_sites("graph_show_sites", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ShowSitesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ShowSitesBind));
        show_sites.varargs = LogicalType::VARCHAR;
        show_sites.named_parameters["secret"] = LogicalType::VARCHAR;
        show_sites.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(show_sites);
        FunctionDescription desc;
        desc.description = "Search and list SharePoint sites accessible via Microsoft Graph. "
                           "Returns id, name, display_name, web_url, created_at, last_modified_at. "
                           "Pass an optional search term as the first positional argument to filter by name; "
                           "omit it to list all accessible sites.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples = {
            "SELECT * FROM graph_show_sites(secret := 'ms_graph')",
            "SELECT * FROM graph_show_sites('Finance', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction show_drives("graph_show_drives", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ShowDrivesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ShowDrivesBind));
        show_drives.varargs = LogicalType::VARCHAR;
        show_drives.named_parameters["secret"] = LogicalType::VARCHAR;
        show_drives.named_parameters["site"] = LogicalType::VARCHAR;
        show_drives.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(show_drives);
        FunctionDescription desc;
        desc.description = "List document library drives in a SharePoint site. "
                           "Returns id, name, drive_type, web_url. "
                           "Pass a composite site_id (guid,guid,guid), a site display name, or a site URL "
                           "as the first positional argument, or use the site named parameter instead.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples = {
            "SELECT * FROM graph_show_drives(site := 'Finance', secret := 'ms_graph')",
            "SELECT * FROM graph_show_drives('https://tenant.sharepoint.com/sites/Finance', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction show_lists("graph_show_lists", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ShowListsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ShowListsBind));
        show_lists.varargs = LogicalType::VARCHAR;
        show_lists.named_parameters["secret"] = LogicalType::VARCHAR;
        show_lists.named_parameters["site"] = LogicalType::VARCHAR;
        show_lists.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(show_lists);
        FunctionDescription desc;
        desc.description = "List all lists in a SharePoint site. "
                           "Returns id, name, display_name, list_template, web_url. "
                           "Pass a composite site_id (guid,guid,guid), a site display name, or a site URL "
                           "as the first positional argument, or use the site named parameter instead. "
                           "Use the returned id or display_name as the list argument of "
                           "graph_sharepoint_list_read(), graph_describe_list(), etc.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples = {
            "SELECT * FROM graph_show_lists(site := 'Finance', secret := 'ms_graph')",
            "SELECT * FROM graph_show_lists('https://tenant.sharepoint.com/sites/Finance', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction describe_list("graph_describe_list",
                                    {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                    DATAZOO_GUARD(ERPL_ODATA_BANNER, DescribeListScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DescribeListBind));
        describe_list.named_parameters["secret"] = LogicalType::VARCHAR;
        describe_list.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(describe_list);
        FunctionDescription desc;
        desc.description = "Describe the column schema of a SharePoint list. "
                           "Returns column_name, column_type, is_required, is_hidden. "
                           "Both arguments accept a GUID, display name, or site URL. "
                           "Use this to discover field names before calling "
                           "graph_sharepoint_create_item() or graph_sharepoint_update_item().";
        desc.parameter_names = {"site_id_or_url", "list_id_or_name", "secret"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_describe_list('Finance', 'Budget', secret := 'ms_graph')",
            "SELECT * FROM graph_describe_list('https://tenant.sharepoint.com/sites/Finance', 'Budget', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction list_items("graph_sharepoint_list_read",
                                 {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                 DATAZOO_GUARD(ERPL_ODATA_BANNER, ListItemsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ListItemsBind));
        list_items.named_parameters["secret"] = LogicalType::VARCHAR;
        list_items.init_global = GraphJsonArrayScanState::Init;
        CreateTableFunctionInfo info(list_items);
        FunctionDescription desc;
        desc.description = "Read all items from a SharePoint list. "
                           "Returns dynamic columns matching the list schema (use graph_describe_list() to inspect them). "
                           "Also returns item_id (SharePoint internal integer ID) on every row. "
                           "Both arguments accept a GUID, display name, or site URL.";
        desc.parameter_names = {"site_id_or_url", "list_id_or_name", "secret"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_sharepoint_list_read('Finance', 'Budget', secret := 'ms_graph')",
            "SELECT * FROM graph_sharepoint_list_read('https://tenant.sharepoint.com/sites/Finance', 'Budget', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }

    {
        TableFunction create_item("graph_sharepoint_create_item",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                                  DATAZOO_GUARD(ERPL_ODATA_BANNER, CreateItemScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, CreateItemBind));
        create_item.named_parameters["secret"] = LogicalType::VARCHAR;
        create_item.init_global = ScanRowCursorState::Init;
        CreateTableFunctionInfo info(create_item);
        FunctionDescription desc;
        desc.description = "Create a new item in a SharePoint list. "
                           "Returns item_id (the newly created item's integer ID). "
                           "fields_json is a JSON object of SharePoint internal field name/value pairs "
                           "(use graph_describe_list() to find field names). "
                           "Both site and list accept a GUID, display name, or URL.";
        desc.parameter_names = {"site_id_or_url", "list_id_or_name", "fields_json", "secret"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_sharepoint_create_item('Finance', 'Budget', '{\"Title\":\"Q1\",\"Status\":\"Active\"}', secret := 'ms_graph')",
            "SELECT * FROM graph_sharepoint_create_item('https://tenant.sharepoint.com/sites/Finance', 'Budget', '{\"Title\":\"Q2\"}', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        ScalarFunctionSet update_set("graph_sharepoint_update_item");
        // Overload without secret (uses ambient Graph auth)
        ScalarFunction update_no_secret(
            {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
            LogicalType::BOOLEAN, UpdateItemExecute);
        update_no_secret.stability = FunctionStability::VOLATILE;
        update_set.AddFunction(update_no_secret);
        // Overload with explicit secret name
        ScalarFunction update_with_secret(
            {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
             LogicalType::VARCHAR},
            LogicalType::BOOLEAN, UpdateItemExecute);
        update_with_secret.stability = FunctionStability::VOLATILE;
        update_set.AddFunction(update_with_secret);

        CreateScalarFunctionInfo info(std::move(update_set));
        FunctionDescription desc;
        desc.description = "Update fields of an existing SharePoint list item. Returns true on success. "
                           "item_id is the SharePoint integer ID (from the item_id column of "
                           "graph_sharepoint_list_read()). "
                           "fields_json is a JSON object of SharePoint internal field name/value pairs. "
                           "secret is positional (last argument), not a named parameter.";
        desc.parameter_names = {"site_id_or_url", "list_id_or_name", "item_id", "fields_json", "secret"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                                 LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT graph_sharepoint_update_item('Finance', 'Budget', '42', '{\"Status\":\"Approved\"}', 'ms_graph')",
            "UPDATE items SET ok = graph_sharepoint_update_item('Finance', 'Budget', item_id::VARCHAR, json_object('Status', new_status), 'ms_graph') FROM items"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        ScalarFunctionSet delete_set("graph_sharepoint_delete_item");
        // Overload without secret
        ScalarFunction delete_no_secret(
            {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
            LogicalType::BOOLEAN, DeleteItemExecute);
        delete_no_secret.stability = FunctionStability::VOLATILE;
        delete_set.AddFunction(delete_no_secret);
        // Overload with explicit secret name
        ScalarFunction delete_with_secret(
            {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
            LogicalType::BOOLEAN, DeleteItemExecute);
        delete_with_secret.stability = FunctionStability::VOLATILE;
        delete_set.AddFunction(delete_with_secret);

        CreateScalarFunctionInfo info(std::move(delete_set));
        FunctionDescription desc;
        desc.description = "Delete an item from a SharePoint list by its item ID. Returns true on success. "
                           "item_id is the SharePoint integer ID (from the item_id column of "
                           "graph_sharepoint_list_read()). "
                           "secret is positional (last argument), not a named parameter.";
        desc.parameter_names = {"site_id_or_url", "list_id_or_name", "item_id", "secret"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                                 LogicalType::VARCHAR};
        desc.examples = {
            "SELECT graph_sharepoint_delete_item('Finance', 'Budget', '42', 'ms_graph')",
            "SELECT graph_sharepoint_delete_item('Finance', 'Budget', item_id::VARCHAR, 'ms_graph') FROM graph_sharepoint_list_read('Finance', 'Budget', secret := 'ms_graph') WHERE Title = 'OldEntry'"
        };
        desc.categories = {"microsoft", "graph", "sharepoint"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }

    ERPL_TRACE_INFO("GRAPH_SHAREPOINT", "Successfully registered Microsoft Graph SharePoint functions");
}

} // namespace erpl_web
