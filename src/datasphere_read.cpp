#include "odata_url_helpers.hpp"
#include "datasphere_read.hpp"
#include "datasphere_client.hpp"
#include "odata_read_functions.hpp"
#include "datasphere_secret.hpp"
#include "duckdb_argument_helper.hpp"
#include "duckdb/function/table_function.hpp"
// #include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "tracing.hpp"
#include "telemetry.hpp"
#include <sstream>
#include "erpl_odata_banner.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

namespace {
    // Small helper: ensure trailing asset segment and Datasphere double-segment pattern
    inline void EnsureAssetSegmentPatternImpl(std::string &url, const std::string &asset_id) {
        if (asset_id.empty()) {
            return;
        }

        // Segments go on the PATH, not the end of the string.
        //
        // This appended blindly, so a URL that already carried a query came back corrupted:
        // "https://host/path?$top=5" became "https://host/path?$top=5/A/A", with the
        // segments buried inside the query value. Reachable through the documented
        // absolute-URL hatch, which is exactly what the re-execution tests use.
        // See GitHub #243.
        const auto query_start = url.find_first_of("?#");
        std::string path = (query_start == std::string::npos) ? url : url.substr(0, query_start);
        const std::string tail = (query_start == std::string::npos) ? std::string() : url.substr(query_start);

        // If the path doesn't already end with asset_id, append it
        const bool ends_with_asset = path.size() >= asset_id.size() &&
                                     path.compare(path.size() - asset_id.size(), asset_id.size(), asset_id) == 0;
        if (!ends_with_asset) {
            if (!path.empty() && path.back() != '/') {
                path += "/";
            }
            path += asset_id;
        }
        // Datasphere requires double asset segment (/{asset}/{asset}) for root collection without params
        if (path.find("hcs.cloud.sap") != std::string::npos) {
            const std::string suffix = "/" + asset_id;
            if (path.size() < suffix.size() ||
                path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
                path += suffix;
            }
        }

        url = path + tail;
    }
    // Exposed for testing: see the declaration in datasphere_read.hpp.
    // (Defined inside the same anonymous-namespace block would make it file-local again,
    // so the public definition lives just below, after the block closes.)

    // Helper function to build the analytical data URL
    std::string BuildAnalyticalDataUrl(const std::string& space_id, const std::string& asset_id, 
                                       const std::string& tenant, const std::string& data_center) {
        if (LooksLikeAbsoluteHttpUrl(space_id)) {
            // Same rule as the Business Central and Dataverse hatches: this URL is taken
            // verbatim from the caller and carries an OAuth bearer token, so plain http is
            // allowed only for loopback.
            RequireGatedServiceUrl(space_id, "Datasphere 'space_id'");
            std::string data_url = space_id;
            EnsureAssetSegmentPatternImpl(data_url, asset_id);
            return data_url;
        }
        auto base = DatasphereUrlBuilder::BuildAnalyticalUrl(tenant, data_center, space_id, asset_id);
        EnsureAssetSegmentPatternImpl(base, asset_id);
        return base;
    }
    
    // Helper function to build the data URL
    std::string BuildDataUrl(const std::string& space_id, const std::string& asset_id, 
                            const std::string& tenant, const std::string& data_center) {
        if (LooksLikeAbsoluteHttpUrl(space_id)) {
            RequireGatedServiceUrl(space_id, "Datasphere 'space_id'");
            std::string data_url = space_id;
            EnsureAssetSegmentPatternImpl(data_url, asset_id);
            return data_url;
        }
        auto base = DatasphereUrlBuilder::BuildRelationalUrl(tenant, data_center, space_id, asset_id);
        EnsureAssetSegmentPatternImpl(base, asset_id);
        return base;
    }
    
    // Helper function to apply named parameters to bind data
    void ApplyNamedParameters(ODataReadBindData* read_bind, 
                            const duckdb::TableFunctionBindInput& input) {
        // Handle LIMIT and OFFSET
        if (input.named_parameters.find("top") != input.named_parameters.end()) {
            auto limit_value = input.named_parameters["top"].GetValue<duckdb::idx_t>();
            read_bind->PredicatePushdownHelper()->ConsumeLimit(limit_value);
            ERPL_TRACE_DEBUG("DATASPHERE_RELATIONAL_BIND", "Set limit to: " + std::to_string(limit_value));
        }
        
        if (input.named_parameters.find("skip") != input.named_parameters.end()) {
            auto offset_value = input.named_parameters["skip"].GetValue<duckdb::idx_t>();
            read_bind->PredicatePushdownHelper()->ConsumeOffset(offset_value);
            ERPL_TRACE_DEBUG("DATASPHERE_RELATIONAL_BIND", "Set offset to: " + std::to_string(offset_value));
        }
    }
}

// Public wrapper over the file-local implementation, declared in datasphere_read.hpp so the
// query-string behaviour can be tested directly (GitHub #243).
void EnsureAssetSegmentPattern(std::string &url, const std::string &asset_id) {
    EnsureAssetSegmentPatternImpl(url, asset_id);
}

static duckdb::unique_ptr<duckdb::FunctionData> DatasphereReadRelationalBind(duckdb::ClientContext &context,
                                                                           duckdb::TableFunctionBindInput &input,
                                                                           duckdb::vector<duckdb::LogicalType> &return_types,
                                                                           duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("datasphere_read_relational");
    ERPL_TRACE_DEBUG("DATASPHERE_RELATIONAL_BIND", "=== DATASPHERE_RELATIONAL_BIND CALLED ===");
    
    // Extract basic parameters
    auto space_id = input.inputs[0].GetValue<std::string>();
    auto asset_id = input.inputs[1].GetValue<std::string>();
    
    // Extract secret name
    std::string secret_name = "datasphere";
    if (input.inputs.size() > 2) {
        secret_name = input.inputs[2].GetValue<std::string>();
    } else if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters["secret"].GetValue<std::string>();
    }
    
    // Resolve authentication
    auto auth = ResolveDatasphereAuth(context, secret_name);
    ERPL_TRACE_INFO("DATASPHERE_RELATIONAL_BIND", "Using tenant: " + auth.tenant_name + 
                    ", data_center: " + auth.data_center + ", space_id: " + space_id + 
                    ", asset_id: " + asset_id);
    
    // Build data URL
    auto data_url = BuildDataUrl(space_id, asset_id, auth.tenant_name, auth.data_center);
    ERPL_TRACE_INFO("DATASPHERE_RELATIONAL_BIND", "Data URL: " + data_url);
    
    // Create OData bind data
    auto read_bind = ODataReadBindData::FromEntitySetRoot(data_url, auth.auth_params);
    
    // Extract and apply input parameters BEFORE metadata extraction
    if (input.named_parameters.find("params") != input.named_parameters.end()) {
        auto input_params = ExtractInputParameters(input.named_parameters["params"], "DATASPHERE_BIND");
        
        if (!input_params.empty()) {
            // Store parameters in bind data
            read_bind->SetInputParameters(input_params);
            ERPL_TRACE_INFO("DATASPHERE_RELATIONAL_BIND", "Stored " + std::to_string(input_params.size()) + " input parameters");
            
            // Pass parameters to OData client immediately for proper metadata handling
            auto odata_client = read_bind->GetODataClient();
            if (odata_client) {
                odata_client->SetInputParameters(input_params);
                ERPL_TRACE_INFO("DATASPHERE_RELATIONAL_BIND", "Passed input parameters to OData client");
            } else {
                ERPL_TRACE_ERROR("DATASPHERE_RELATIONAL_BIND", "Failed to get OData client");
            }
        }
    }
    
    // Apply other named parameters
    ApplyNamedParameters(read_bind.get(), input);
    
    // Get result schema
    names = read_bind->GetResultNames(false);
    return_types = read_bind->GetResultTypes(false);
    
    ERPL_TRACE_DEBUG("DATASPHERE_RELATIONAL_BIND", "=== DATASPHERE_RELATIONAL_BIND COMPLETED ===");
    return std::move(read_bind);
}

static duckdb::unique_ptr<duckdb::GlobalTableFunctionState> DatasphereReadRelationalTableInitGlobalState(duckdb::ClientContext &context,
                                                                                                           duckdb::TableFunctionInitInput &input) 
{
    auto &bind_data = input.bind_data->CastNoConst<ODataReadBindData>();
    auto column_ids = input.column_ids;

    ERPL_TRACE_DEBUG("DATASPHERE_RELATIONAL_INIT", "Initializing with " + std::to_string(column_ids.size()) + " columns");

    // Projection, filter pushdown and the first-page prefetch all mutate scan state, so
    // they run against a private clone and the bind data stays exactly as bind left it.
    // Scanning straight out of the bind data - which is what returning a bare
    // GlobalTableFunctionState made ResolveScanState do - meant the second EXECUTE of a
    // bound plan found the row buffer already drained and returned nothing, silently
    // (GitHub #75).
    auto scan_state = bind_data.CloneForScan();

    scan_state->ActivateColumns(column_ids);
    scan_state->AddFilters(input.filters);

    // Input parameters live on the OData client, and CloneForScan mints a fresh one, so
    // they are re-applied to the clone here. They also survive the pushdown rebuild below,
    // which now carries them explicitly - before that they came back only because
    // PrefetchFirstPage re-applies them afterwards, which is an accident of ordering
    // rather than a property of the re-mint.
    auto input_params = scan_state->GetInputParameters();
    if (!input_params.empty()) {
        auto odata_client = scan_state->GetODataClient();
        if (odata_client) {
            odata_client->SetInputParameters(input_params);
            ERPL_TRACE_INFO("DATASPHERE_RELATIONAL_INIT", "Re-applied " + std::to_string(input_params.size()) + " input parameters");
        }
    }

    scan_state->UpdateUrlFromPredicatePushdown();
    scan_state->PrefetchFirstPage();

    return duckdb::make_uniq<ODataReadGlobalState>(std::move(scan_state));
}

duckdb::TableFunctionSet CreateDatasphereReadRelationalFunction() {
    ERPL_TRACE_DEBUG("DATASPHERE_FUNCTION_REGISTRATION", "=== REGISTERING DATASPHERE_RELATIONAL FUNCTION ===");
    
    duckdb::TableFunctionSet set("datasphere_read_relational");
    
    // 2-parameter version (space_id, asset_id)
    duckdb::TableFunction relational_function_2_params(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},
        DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereReadRelationalBind), DatasphereReadRelationalTableInitGlobalState);
    relational_function_2_params.filter_pushdown = true;
    relational_function_2_params.projection_pushdown = true;
    relational_function_2_params.table_scan_progress = ODataReadTableProgress;
    relational_function_2_params.named_parameters["top"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    relational_function_2_params.named_parameters["skip"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    relational_function_2_params.named_parameters["params"] = duckdb::LogicalType::MAP(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                                                                                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    relational_function_2_params.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    set.AddFunction(relational_function_2_params);
    
    // 3-parameter version (space_id, asset_id, secret_name)
    duckdb::TableFunction relational_function_3_params(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},
        DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereReadRelationalBind), DatasphereReadRelationalTableInitGlobalState);
    relational_function_3_params.filter_pushdown = true;
    relational_function_3_params.projection_pushdown = true;
    relational_function_3_params.table_scan_progress = ODataReadTableProgress;
    relational_function_3_params.named_parameters["top"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    relational_function_3_params.named_parameters["skip"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    relational_function_3_params.named_parameters["params"] = duckdb::LogicalType::MAP(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                                                                                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    set.AddFunction(relational_function_3_params);
    
    ERPL_TRACE_DEBUG("DATASPHERE_FUNCTION_REGISTRATION", "=== DATASPHERE_RELATIONAL FUNCTION REGISTRATION COMPLETED ===");
    return set;
}

// ================================================================================================
// Analytical read (metrics/dimensions -> $select)
// ================================================================================================

namespace { }

static duckdb::unique_ptr<duckdb::FunctionData> DatasphereReadAnalyticalBind(duckdb::ClientContext &context,
                                                                             duckdb::TableFunctionBindInput &input,
                                                                             duckdb::vector<duckdb::LogicalType> &return_types,
                                                                             duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("datasphere_read_analytical");
    ERPL_TRACE_DEBUG("DATASPHERE_ANALYTICAL_BIND", "=== DATASPHERE_ANALYTICAL_BIND CALLED ===");

    // Extract basic parameters
    auto space_id = input.inputs[0].GetValue<std::string>();
    auto asset_id = input.inputs[1].GetValue<std::string>();

    // Extract secret name
    std::string secret_name = "datasphere";
    if (input.inputs.size() > 2) {
        secret_name = input.inputs[2].GetValue<std::string>();
    } else if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters["secret"].GetValue<std::string>();
    }

    // Resolve authentication
    auto auth = ResolveDatasphereAuth(context, secret_name);
    ERPL_TRACE_INFO("DATASPHERE_ANALYTICAL_BIND", "Using tenant: " + auth.tenant_name + 
                    ", data_center: " + auth.data_center + ", space_id: " + space_id + 
                    ", asset_id: " + asset_id);

    // Build data URL
    auto data_url = BuildAnalyticalDataUrl(space_id, asset_id, auth.tenant_name, auth.data_center);

    // Parse metrics and dimensions named parameters and translate to $select
    std::vector<std::string> select_fields;
    if (input.named_parameters.find("dimensions") != input.named_parameters.end()) {
        auto dims = erpl_web::GetStringList(input.named_parameters["dimensions"]);
        select_fields.insert(select_fields.end(), dims.begin(), dims.end());
    }
    if (input.named_parameters.find("metrics") != input.named_parameters.end()) {
        auto mets = erpl_web::GetStringList(input.named_parameters["metrics"]);
        select_fields.insert(select_fields.end(), mets.begin(), mets.end());
    }
    if (!select_fields.empty()) {
        std::ostringstream ss;
        for (size_t i = 0; i < select_fields.size(); ++i) {
            if (i > 0) ss << ",";
            ss << select_fields[i];
        }
        if (data_url.find('?') == std::string::npos) {
            data_url += "?";
        } else if (!data_url.empty() && data_url.back() != '?' && data_url.back() != '&') {
            data_url += "&";
        }
        // Use standard OData $select; ApplyFiltersToUrl will skip adding another when present
        data_url += "$select=" + ss.str();
        ERPL_TRACE_INFO("DATASPHERE_ANALYTICAL_BIND", "Applied $select from metrics/dimensions: " + ss.str());
    }

    ERPL_TRACE_INFO("DATASPHERE_ANALYTICAL_BIND", "Data URL: " + data_url);

    // Create OData bind data
    auto read_bind = ODataReadBindData::FromEntitySetRoot(data_url, auth.auth_params);

    // Extract and apply input parameters BEFORE metadata extraction
    if (input.named_parameters.find("params") != input.named_parameters.end()) {
        auto input_params = ExtractInputParameters(input.named_parameters["params"], "DATASPHERE_BIND");
        if (!input_params.empty()) {
            read_bind->SetInputParameters(input_params);
            auto odata_client = read_bind->GetODataClient();
            if (odata_client) {
                odata_client->SetInputParameters(input_params);
                ERPL_TRACE_INFO("DATASPHERE_ANALYTICAL_BIND", "Passed input parameters to OData client");
            } else {
                ERPL_TRACE_ERROR("DATASPHERE_ANALYTICAL_BIND", "Failed to get OData client");
            }
        }
    }

    // Apply other named parameters
    ApplyNamedParameters(read_bind.get(), input);

    // Get result schema
    names = read_bind->GetResultNames(false);
    return_types = read_bind->GetResultTypes(false);

    ERPL_TRACE_DEBUG("DATASPHERE_ANALYTICAL_BIND", "=== DATASPHERE_ANALYTICAL_BIND COMPLETED ===");
    return std::move(read_bind);
}

static duckdb::unique_ptr<duckdb::GlobalTableFunctionState> DatasphereReadAnalyticalTableInitGlobalState(duckdb::ClientContext &context,
                                                                                                         duckdb::TableFunctionInitInput &input) 
{
    auto &bind_data = input.bind_data->CastNoConst<ODataReadBindData>();
    auto column_ids = input.column_ids;

    ERPL_TRACE_DEBUG("DATASPHERE_ANALYTICAL_INIT", "Initializing with " + std::to_string(column_ids.size()) + " columns");

    // Private per-execution scan state; see the relational reader above (GitHub #75).
    auto scan_state = bind_data.CloneForScan();

    scan_state->ActivateColumns(column_ids);
    scan_state->AddFilters(input.filters);

    auto input_params = scan_state->GetInputParameters();
    if (!input_params.empty()) {
        auto odata_client = scan_state->GetODataClient();
        if (odata_client) {
            odata_client->SetInputParameters(input_params);
            ERPL_TRACE_INFO("DATASPHERE_ANALYTICAL_INIT", "Re-applied " + std::to_string(input_params.size()) + " input parameters");
        }
    }

    scan_state->UpdateUrlFromPredicatePushdown();
    scan_state->PrefetchFirstPage();

    return duckdb::make_uniq<ODataReadGlobalState>(std::move(scan_state));
}

duckdb::TableFunctionSet CreateDatasphereReadAnalyticalFunction() {
    ERPL_TRACE_DEBUG("DATASPHERE_FUNCTION_REGISTRATION", "=== REGISTERING DATASPHERE_ANALYTICAL FUNCTION ===");

    duckdb::TableFunctionSet set("datasphere_read_analytical");

    // 2-parameter version (space_id, asset_id)
    duckdb::TableFunction analytical_function_2_params(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},
        DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereReadAnalyticalBind), DatasphereReadAnalyticalTableInitGlobalState);
    analytical_function_2_params.filter_pushdown = true;
    analytical_function_2_params.projection_pushdown = true;
    analytical_function_2_params.table_scan_progress = ODataReadTableProgress;
    analytical_function_2_params.named_parameters["top"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    analytical_function_2_params.named_parameters["skip"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    analytical_function_2_params.named_parameters["params"] = duckdb::LogicalType::MAP(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                                                                                         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    analytical_function_2_params.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    analytical_function_2_params.named_parameters["metrics"] = duckdb::LogicalType::LIST(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    analytical_function_2_params.named_parameters["dimensions"] = duckdb::LogicalType::LIST(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    set.AddFunction(analytical_function_2_params);

    // 3-parameter version (space_id, asset_id, secret_name)
    duckdb::TableFunction analytical_function_3_params(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},
        DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereReadAnalyticalBind), DatasphereReadAnalyticalTableInitGlobalState);
    analytical_function_3_params.filter_pushdown = true;
    analytical_function_3_params.projection_pushdown = true;
    analytical_function_3_params.table_scan_progress = ODataReadTableProgress;
    analytical_function_3_params.named_parameters["top"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    analytical_function_3_params.named_parameters["skip"] = duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT);
    analytical_function_3_params.named_parameters["params"] = duckdb::LogicalType::MAP(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                                                                                         duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    analytical_function_3_params.named_parameters["metrics"] = duckdb::LogicalType::LIST(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    analytical_function_3_params.named_parameters["dimensions"] = duckdb::LogicalType::LIST(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    set.AddFunction(analytical_function_3_params);

    ERPL_TRACE_DEBUG("DATASPHERE_FUNCTION_REGISTRATION", "=== DATASPHERE_ANALYTICAL FUNCTION REGISTRATION COMPLETED ===");
    return set;
}

} // namespace erpl_web
