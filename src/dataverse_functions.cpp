#include "dataverse_functions.hpp"
#include "dataverse_secret.hpp"
#include "dataverse_client.hpp"
#include "odata_read_functions.hpp"
#include "odata_edm.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include <variant>
#include "erpl_odata_banner.hpp"

namespace erpl_web {

using namespace duckdb;

// ============================================================================
// crm_show_entities - List entities in the Dataverse environment
// ============================================================================

struct CrmShowEntitiesBindData : public TableFunctionData {
    std::unique_ptr<ODataReadBindData> odata_bind_data;
};

// Per-execution scan state (GitHub #191). `finished` lived on the bind data and was never
// reset, so a second EXECUTE of a bound plan returned zero rows silently.
class CrmShowEntitiesGlobalState : public GlobalTableFunctionState {
public:
    explicit CrmShowEntitiesGlobalState(std::unique_ptr<ODataReadBindData> scan_state)
        : scan_state(std::move(scan_state)) {}

    ODataReadBindData &Scan() { return *scan_state; }
    bool finished = false;

private:
    std::unique_ptr<ODataReadBindData> scan_state;
};

static unique_ptr<GlobalTableFunctionState> CrmShowEntitiesInitGlobalState(
    ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->CastNoConst<CrmShowEntitiesBindData>();
    return make_uniq<CrmShowEntitiesGlobalState>(bind_data.odata_bind_data->CloneForScan());
}

static unique_ptr<FunctionData> CrmShowEntitiesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("CRM_FUNC", "Binding crm_show_entities");

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Resolve auth
    auto auth_info = ResolveDataverseAuth(context, secret_name);

    // Create OData client for EntityDefinitions endpoint
    auto client = DataverseClientFactory::CreateEntityDefinitionsClient(
        auth_info.environment_url,
        auth_info.auth_params
    );

    // Create bind data using OData infrastructure
    auto bind_data = make_uniq<CrmShowEntitiesBindData>();
    bind_data->odata_bind_data = ODataReadBindData::FromEntitySetClient(client);

    // Get schema from OData
    names = bind_data->odata_bind_data->GetResultNames();
    return_types = bind_data->odata_bind_data->GetResultTypes();

    ERPL_TRACE_INFO("CRM_FUNC", "crm_show_entities bound with " + std::to_string(names.size()) + " columns");
    return std::move(bind_data);
}

static void CrmShowEntitiesScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<CrmShowEntitiesGlobalState>();

    if (gstate.finished) {
        return;
    }

    auto rows_fetched = gstate.Scan().FetchNextResult(output);
    if (!gstate.Scan().HasMoreResults() && rows_fetched == 0) {
        gstate.finished = true;
    }
}

TableFunctionSet CreateCrmShowEntitiesFunction() {
    TableFunctionSet set("crm_show_entities");

    TableFunction func({}, DATAZOO_GUARD(ERPL_ODATA_BANNER, CrmShowEntitiesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, CrmShowEntitiesBind), CrmShowEntitiesInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;

    set.AddFunction(func);
    return set;
}

// ============================================================================
// crm_describe - Describe schema of a Dataverse entity
// ============================================================================

struct CrmDescribeBindData : public TableFunctionData {
    std::vector<std::string> attribute_names;
    std::vector<std::string> attribute_types;
    std::vector<bool> is_nullable;
    std::vector<bool> is_primary;
};

// Per-execution row cursor (GitHub #191). It lived on the bind data and was never reset,
// so a second EXECUTE of a bound plan resumed past the end and returned nothing. The
// describe payload itself is immutable and stays shared.
class CrmDescribeGlobalState : public GlobalTableFunctionState {
public:
    idx_t current_row = 0;
};

static unique_ptr<GlobalTableFunctionState> CrmDescribeInitGlobalState(
    ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<CrmDescribeGlobalState>();
}

static unique_ptr<FunctionData> CrmDescribeBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("CRM_FUNC", "Binding crm_describe");

    // Get entity name (logical name in Dataverse)
    auto entity_name = input.inputs[0].GetValue<string>();

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Resolve auth
    auto auth_info = ResolveDataverseAuth(context, secret_name);

    // Create client for entity attributes
    auto client = DataverseClientFactory::CreateEntityAttributesClient(
        auth_info.environment_url,
        entity_name,
        auth_info.auth_params
    );

    // Create bind data for fetching attributes
    auto odata_bind = ODataReadBindData::FromEntitySetClient(client);

    // Fetch all attribute rows
    auto bind_data = make_uniq<CrmDescribeBindData>();

    // Get attribute names and types from the OData response
    vector<string> attr_names = odata_bind->GetResultNames();
    vector<LogicalType> attr_types = odata_bind->GetResultTypes();

    // Find the column indices for the fields we need
    idx_t logical_name_idx = idx_t(-1);
    idx_t attr_type_idx = idx_t(-1);
    idx_t is_primary_idx = idx_t(-1);

    for (idx_t i = 0; i < attr_names.size(); i++) {
        if (attr_names[i] == "LogicalName") logical_name_idx = i;
        else if (attr_names[i] == "AttributeTypeName" || attr_names[i] == "AttributeType") attr_type_idx = i;
        else if (attr_names[i] == "IsPrimaryId") is_primary_idx = i;
    }

    // Fetch rows and extract the relevant columns
    DataChunk chunk;
    chunk.Initialize(context, attr_types);

    while (odata_bind->HasMoreResults()) {
        auto rows_fetched = odata_bind->FetchNextResult(chunk);
        if (rows_fetched == 0) break;

        for (idx_t row = 0; row < chunk.size(); row++) {
            // Get LogicalName
            if (logical_name_idx != idx_t(-1)) {
                auto val = chunk.GetValue(logical_name_idx, row);
                bind_data->attribute_names.push_back(val.IsNull() ? "" : val.ToString());
            } else {
                bind_data->attribute_names.push_back("unknown");
            }

            // Get AttributeType
            if (attr_type_idx != idx_t(-1)) {
                auto val = chunk.GetValue(attr_type_idx, row);
                bind_data->attribute_types.push_back(val.IsNull() ? "Unknown" : val.ToString());
            } else {
                bind_data->attribute_types.push_back("Unknown");
            }

            // Dataverse attributes are generally nullable
            bind_data->is_nullable.push_back(true);

            // Get IsPrimaryId
            if (is_primary_idx != idx_t(-1)) {
                auto val = chunk.GetValue(is_primary_idx, row);
                bind_data->is_primary.push_back(!val.IsNull() && val.GetValue<bool>());
            } else {
                bind_data->is_primary.push_back(false);
            }
        }

        chunk.Reset();
    }

    // Set up return schema
    names = {"attribute_name", "attribute_type", "nullable", "is_primary"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN};

    ERPL_TRACE_INFO("CRM_FUNC", "crm_describe bound for entity: " + entity_name + " with " + std::to_string(bind_data->attribute_names.size()) + " attributes");
    return std::move(bind_data);
}

static void CrmDescribeScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &bind_data = data.bind_data->CastNoConst<CrmDescribeBindData>();
    auto &gstate = data.global_state->Cast<CrmDescribeGlobalState>();

    idx_t count = 0;
    while (gstate.current_row < bind_data.attribute_names.size() && count < STANDARD_VECTOR_SIZE) {
        output.SetValue(0, count, Value(bind_data.attribute_names[gstate.current_row]));
        output.SetValue(1, count, Value(bind_data.attribute_types[gstate.current_row]));
        output.SetValue(2, count, Value(bind_data.is_nullable[gstate.current_row]));
        output.SetValue(3, count, Value(bind_data.is_primary[gstate.current_row]));
        gstate.current_row++;
        count++;
    }

    output.SetCardinality(count);
}

TableFunctionSet CreateCrmDescribeFunction() {
    TableFunctionSet set("crm_describe");

    TableFunction func({LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, CrmDescribeScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, CrmDescribeBind), CrmDescribeInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;

    set.AddFunction(func);
    return set;
}

// ============================================================================
// crm_read - Read data from a Dataverse entity with predicate pushdown
// ============================================================================

struct CrmReadBindData : public TableFunctionData {
    std::unique_ptr<ODataReadBindData> odata_bind_data;
};

// Owns the scan state for ONE execution of a bound plan (GitHub #182).
//
// `finished` used to live on the bind data and was never reset, so the second EXECUTE of
// a prepared statement returned zero rows deterministically and silently. Both it and the
// row buffer now belong to the per-execution clone.
class CrmReadGlobalState : public GlobalTableFunctionState {
public:
    explicit CrmReadGlobalState(std::unique_ptr<ODataReadBindData> scan_state)
        : scan_state(std::move(scan_state)) {}

    ODataReadBindData &Scan() { return *scan_state; }
    bool finished = false;

private:
    std::unique_ptr<ODataReadBindData> scan_state;
};

static unique_ptr<FunctionData> CrmReadBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("CRM_FUNC", "Binding crm_read");

    // Get entity name (the plural logical collection name, e.g., "accounts", "contacts")
    auto entity_name = input.inputs[0].GetValue<string>();

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Resolve auth
    auto auth_info = ResolveDataverseAuth(context, secret_name);

    // Create OData client
    auto client = DataverseClientFactory::CreateEntitySetClient(
        auth_info.environment_url,
        entity_name,
        auth_info.auth_params
    );

    // Create bind data using OData infrastructure
    auto bind_data = make_uniq<CrmReadBindData>();
    bind_data->odata_bind_data = ODataReadBindData::FromEntitySetClient(client);

    // Handle expand parameter — must call ProcessExpandClause so ConsumeExpand
    // feeds $expand into the predicate pushdown helper and sets up expanded schema.
    if (input.named_parameters.count("expand")) {
        auto expand_clause = input.named_parameters.at("expand").GetValue<string>();
        ODataReadBindHelpers::ProcessExpandClause(bind_data->odata_bind_data.get(), expand_clause);
    }

    // Get schema from OData (includes expanded columns if expand was set)
    names = bind_data->odata_bind_data->GetResultNames();
    return_types = bind_data->odata_bind_data->GetResultTypes();

    ERPL_TRACE_INFO("CRM_FUNC", "crm_read bound for entity: " + entity_name + " with " + std::to_string(names.size()) + " columns");
    return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> CrmReadInitGlobalState(
    ClientContext &context,
    TableFunctionInitInput &input) {

    auto &bind_data = input.bind_data->CastNoConst<CrmReadBindData>();

    // Projection, filter pushdown and the first-page prefetch all mutate scan state, so
    // they run against a private clone; the bind data stays as bind left it. Returning
    // nullptr here - with the scan reading the bind data directly - is what made a second
    // EXECUTE return nothing (GitHub #182).
    auto scan_state = bind_data.odata_bind_data->CloneForScan();

    scan_state->ActivateColumns(input.column_ids);
    scan_state->AddFilters(input.filters);
    scan_state->UpdateUrlFromPredicatePushdown();
    scan_state->PrefetchFirstPage();

    return make_uniq<CrmReadGlobalState>(std::move(scan_state));
}

static void CrmReadScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<CrmReadGlobalState>();

    if (gstate.finished) {
        return;
    }

    auto rows_fetched = gstate.Scan().FetchNextResult(output);
    if (!gstate.Scan().HasMoreResults() && rows_fetched == 0) {
        gstate.finished = true;
        // Report at the terminal exit, as odata_read does, rather than leaving it to the
        // bind-data destructor: the destructor fires at teardown, so warnings could land
        // after the result instead of beside it. Name this function, not odata_read
        // (GitHub #196).
        gstate.Scan().ReportConversionFailures("crm_read");
    }
}

static double CrmReadProgress(ClientContext &context, const FunctionData *bind_data_p,
                              const GlobalTableFunctionState *global_state) {
    // Progress lives on the per-execution clone, so reading it off the bind data would
    // report a scan that is no longer the one running (GitHub #182).
    if (global_state == nullptr) {
        return -1.0;
    }
    auto &gstate = const_cast<GlobalTableFunctionState *>(global_state)->Cast<CrmReadGlobalState>();
    return gstate.Scan().GetProgressFraction();
}

TableFunctionSet CreateCrmReadFunction() {
    TableFunctionSet set("crm_read");

    TableFunction func({LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, CrmReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, CrmReadBind), CrmReadInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;
    func.named_parameters["expand"] = LogicalType::VARCHAR;

    // Enable pushdown features
    func.filter_pushdown = true;
    func.filter_prune = true;
    func.projection_pushdown = true;

    // Progress reporting
    func.table_scan_progress = CrmReadProgress;

    set.AddFunction(func);
    return set;
}

} // namespace erpl_web
