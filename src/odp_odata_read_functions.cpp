#include "odp_odata_read_functions.hpp"
#include "duckdb_compat.hpp"
#include "odp_odata_read_bind_data.hpp"
#include "odata_read_functions.hpp"
#include "tracing.hpp"
#include "duckdb_argument_helper.hpp"
#include "telemetry.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

// ============================================================================
// ODP OData Read Function Implementation
// ============================================================================

duckdb::unique_ptr<duckdb::FunctionData> OdpODataReadBind(duckdb::ClientContext &context,
                                                          duckdb::TableFunctionBindInput &input,
                                                          duckdb::vector<duckdb::LogicalType> &return_types,
                                                          duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("odp_odata_read");
    ERPL_TRACE_DEBUG("ODP_ODATA_READ_BIND", "=== BINDING ODP_ODATA_READ FUNCTION ===");
    
    // Extract required parameter: entity_set_url
    if (input.inputs.empty()) {
        throw duckdb::InvalidInputException("odp_odata_read requires at least one parameter: entity_set_url");
    }
    
    std::string entity_set_url = input.inputs[0].ToString();
    ERPL_TRACE_INFO("ODP_ODATA_READ_BIND", "Entity set URL: " + entity_set_url);
    
    // Extract optional named parameters
    std::string secret_name = "";
    bool force_full_load = false;
    std::string import_delta_token = "";
    std::optional<uint32_t> max_page_size = std::nullopt;
    
    // Process named parameters
    for (auto &kv : input.named_parameters) {
        if (kv.first == "secret") {
            secret_name = kv.second.ToString();
            ERPL_TRACE_DEBUG("ODP_ODATA_READ_BIND", "Using secret: " + secret_name);
        } else if (kv.first == "force_full_load") {
            force_full_load = kv.second.GetValue<bool>();
            ERPL_TRACE_DEBUG("ODP_ODATA_READ_BIND", "Force full load: " + std::string(force_full_load ? "true" : "false"));
        } else if (kv.first == "import_delta_token") {
            import_delta_token = kv.second.ToString();
            ERPL_TRACE_DEBUG("ODP_ODATA_READ_BIND", "Import delta token: " + import_delta_token);
        } else if (kv.first == "max_page_size") {
            // Shared with odata_read: a bare GetValue<uint32_t> let max_page_size=0 through
            // and put `Prefer: odata.maxpagesize=0` on the wire, turning a bad argument into
            // a service-side failure (GitHub #185).
            max_page_size = ValidateMaxPageSizeParameter(kv.second);
            ERPL_TRACE_DEBUG("ODP_ODATA_READ_BIND", "Max page size: " + std::to_string(max_page_size.value()));
        }
    }
    
    try {
        // Create ODP bind data
        auto odp_bind_data = duckdb::make_uniq<OdpODataReadBindData>(
            context, entity_set_url, secret_name, force_full_load, import_delta_token, max_page_size);
        
        // Initialize the ODP components
        odp_bind_data->Initialize();
        
        // Get the underlying OData bind data for schema information
        auto& odata_bind_data = odp_bind_data->GetODataBindData();
        
        // Set up return types and column names from the OData schema
        return_types = odata_bind_data.GetResultTypes();
        duckdb_compat::AssignBindNames(names, odata_bind_data.GetResultNames());
        
        ERPL_TRACE_INFO("ODP_ODATA_READ_BIND", duckdb::StringUtil::Format(
            "Bound ODP function with %zu columns", names.size()));
        
        return std::move(odp_bind_data);
        
    } catch (const duckdb::InvalidInputException& e) {
        // Re-throw DuckDB exceptions as-is (these are already well-formatted)
        throw;
    } catch (const std::runtime_error& e) {
        // Use shared error handling utility for HTTP errors
        ERPL_TRACE_ERROR("ODP_ODATA_READ_BIND", "Binding failed: " + std::string(e.what()));
        throw ODataErrorHandling::ConvertHttpErrorToUserFriendly(e, entity_set_url, "ODP OData", "sap_odp_odata_show()");
    } catch (const std::exception& e) {
        // Handle other exceptions with context
        ERPL_TRACE_ERROR("ODP_ODATA_READ_BIND", "Binding failed: " + std::string(e.what()));
        throw duckdb::InvalidInputException("Failed to bind ODP OData function for URL: " + entity_set_url + 
            ". Error: " + std::string(e.what()));
    }
}

namespace {

// Owns the private scan state for one execution of a bound plan (GitHub #146).
class OdpODataReadGlobalState : public duckdb::GlobalTableFunctionState {
public:
    explicit OdpODataReadGlobalState(duckdb::unique_ptr<OdpODataReadBindData> scan_state)
        : scan_state(std::move(scan_state)) {}

    OdpODataReadBindData &Scan() { return *scan_state; }

private:
    duckdb::unique_ptr<OdpODataReadBindData> scan_state;
};

// Returns the object that owns the scan state for this execution. A caller with no such
// state used to fall back to the shared bind data - the historical behaviour that made the
// second EXECUTE of a bound plan return nothing, silently. Failing loudly instead turns
// the next mis-wired reader into a diagnosable error rather than a wrong answer.
OdpODataReadBindData &ResolveOdpScanState(const duckdb::FunctionData *bind_data,
                                          const duckdb::GlobalTableFunctionState *global_state) {
    auto *odp_global = dynamic_cast<const OdpODataReadGlobalState *>(global_state);
    if (odp_global == nullptr) {
        throw duckdb::NotImplementedException(
            "ODP scan has no per-execution state: the table function must register "
            "OdpODataReadGlobalState as its init_global");
    }
    return const_cast<OdpODataReadGlobalState *>(odp_global)->Scan();
}

} // namespace

void OdpODataReadScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data, duckdb::DataChunk &output) {
    auto &bind_data = ResolveOdpScanState(data.bind_data.get(), data.global_state.get());

    ERPL_TRACE_DEBUG("ODP_ODATA_READ_SCAN", "Starting ODP scan operation");
    
    try {
        // Check if we have more results using ODP logic
        if (!bind_data.HasMoreResults()) {
            ERPL_TRACE_DEBUG("ODP_ODATA_READ_SCAN", "No more results available");
            // This is a real end of scan, and often the ONLY one: when the buffer is
            // already drained the scan returns here without a final zero-row fetch, so
            // the staged delta token has to be committed from this exit too.
            bind_data.FinalizeScan();
            return;
        }
        
        // Use the ODP bind data's fetch method which handles delta replication
        ERPL_TRACE_DEBUG("ODP_ODATA_READ_SCAN", "Fetching next result set via ODP logic");
        unsigned int rows_fetched = bind_data.FetchNextResult(output);
        
        ERPL_TRACE_INFO("ODP_ODATA_READ_SCAN", duckdb::StringUtil::Format("Fetched %d rows", rows_fetched));

        if (rows_fetched == 0) {
            bind_data.FinalizeScan();
        }
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_ODATA_READ_SCAN", "Scan failed: " + std::string(e.what()));
        throw;
    }
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> OdpODataReadInitGlobalState(duckdb::ClientContext &context,
                                                                                 duckdb::TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->CastNoConst<OdpODataReadBindData>();
    auto column_ids = input.column_ids;
    
    // Configure through the ODP wrapper, not the inner ODataReadBindData. The inner
    // instance is replaced wholesale by UpdateODataClientWithResponse on every page, and
    // only the wrapper remembers the column selection well enough to re-apply it to the
    // replacement. Configuring the inner object directly left active_column_ids_ empty, so
    // the re-application in FetchAndLoadNextPage was a no-op and page 2 onwards ran in
    // all-columns mode - which, as the comment there notes, can write out of bounds into a
    // projected chunk. See GitHub #58.
    // Every execution gets a private clone: the row buffer, the pagination cursor, the
    // audit id and the delta-token staging slot all live on the instance, so sharing one
    // made the second EXECUTE of a bound plan return nothing (GitHub #146).
    auto scan_state = bind_data.CloneForScan();

    scan_state->ActivateColumns(column_ids);

    // Filters are deliberately not pushed down for ODP; see the filter_pushdown comment at
    // the function registration below.
    scan_state->GetODataBindData().UpdateUrlFromPredicatePushdown();

    // Do NOT call PrefetchFirstPage() here. The ODP first fetch must be orchestrator-owned:
    // it is the only path that applies the required Prefer: odata.track-changes /
    // odata.maxpagesize=N headers and buffers the response (HandleInitialLoad ->
    // ExecuteInitialLoad -> UpdateODataClientWithResponse, during the scan phase). A bare GET
    // here carries none of those headers, so SAP ODP ignores max_page_size and returns the whole
    // entity set in one response, exceeding the 30 s HTTP read timeout. See GitHub #47.

    return duckdb::make_uniq<OdpODataReadGlobalState>(std::move(scan_state));
}

double OdpODataReadProgress(duckdb::ClientContext &context, const duckdb::FunctionData *func_data,
                           const duckdb::GlobalTableFunctionState *global_state) {
    // Progress lives on the per-execution clone, so it has to be read from there rather
    // than from the bind data (GitHub #146).
    auto &bind_data = ResolveOdpScanState(func_data, global_state);
    return bind_data.GetODataBindData().GetProgressFraction();
}

// ============================================================================
// Function Registration
// ============================================================================

duckdb::TableFunctionSet CreateOdpODataReadFunction() {
    ERPL_TRACE_DEBUG("ODP_FUNCTION_REGISTRATION", "=== REGISTERING ODP_ODATA_READ FUNCTION ===");
    
    duckdb::TableFunctionSet function_set("odp_odata_read");
    
    // Main function: odp_odata_read(entity_set_url)
    duckdb::TableFunction odp_read_function(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},  // entity_set_url parameter
        OdpODataReadScan, 
        OdpODataReadBind,
        OdpODataReadInitGlobalState
    );
    
    // Projection is pushed down and genuinely honoured: the column selection is stored on
    // the wrapper and re-applied to every page replacement.
    odp_read_function.projection_pushdown = true;

    // Filter pushdown is deliberately OFF. Declaring it true tells DuckDB that this scan
    // applies the filters itself, so DuckDB drops the LogicalFilter above the scan - but
    // every ODP request is issued against the raw entity set URL through the orchestrator
    // (which owns the Prefer: odata.track-changes headers), so the filters were never
    // applied anywhere and WHERE clauses silently returned unfiltered rows.
    //
    // Turning this back on requires routing the pushdown-derived URL through the
    // orchestrator and re-applying it after every UpdateODataClientWithResponse, and it can
    // only be validated against a real SAP ODP system. Until then, correct-and-slower beats
    // fast-and-wrong. See GitHub #61.
    odp_read_function.filter_pushdown = false;
    odp_read_function.table_scan_progress = OdpODataReadProgress;
    
    // Add named parameters for ODP-specific functionality
    duckdb_compat::AddNamedParameter(odp_read_function, "secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(odp_read_function, "force_full_load", duckdb::LogicalType(duckdb::LogicalTypeId::BOOLEAN));
    duckdb_compat::AddNamedParameter(odp_read_function, "import_delta_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    // UBIGINT to match odata_read; the shared validator range-checks it down to 32 bits
    // and reports an out-of-range value as an argument error rather than silently wrapping.
    duckdb_compat::AddNamedParameter(odp_read_function, "max_page_size", duckdb::LogicalType(duckdb::LogicalTypeId::UBIGINT));
    
    function_set.AddFunction(odp_read_function);
    
    ERPL_TRACE_INFO("ODP_FUNCTION_REGISTRATION", "ODP_ODATA_READ function registered successfully");
    return function_set;
}

} // namespace erpl_web
