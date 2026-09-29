#include "business_central_functions.hpp"
#include "business_central_secret.hpp"
#include "business_central_client.hpp"
#include "odata_read_functions.hpp"
#include "odata_url_helpers.hpp"
#include "odata_edm.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include <variant>
#include "erpl_odata_banner.hpp"

namespace erpl_web {

using namespace duckdb;

// ============================================================================
// bc_show_companies - List companies in the Business Central environment
// ============================================================================

struct BcShowCompaniesBindData : public TableFunctionData {
    std::unique_ptr<ODataReadBindData> odata_bind_data;
};

// Per-execution scan state (GitHub #191). `finished` lived on the bind data and was never
// reset, so a second EXECUTE of a bound plan returned zero rows silently.
class BcShowCompaniesGlobalState : public GlobalTableFunctionState {
public:
    explicit BcShowCompaniesGlobalState(std::unique_ptr<ODataReadBindData> scan_state)
        : scan_state(std::move(scan_state)) {}

    ODataReadBindData &Scan() { return *scan_state; }
    bool finished = false;

private:
    std::unique_ptr<ODataReadBindData> scan_state;
};

static unique_ptr<GlobalTableFunctionState> BcShowCompaniesInitGlobalState(
    ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->CastNoConst<BcShowCompaniesBindData>();
    return make_uniq<BcShowCompaniesGlobalState>(bind_data.odata_bind_data->CloneForScan());
}

static unique_ptr<FunctionData> BcShowCompaniesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("BC_FUNC", "Binding bc_show_companies");

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Resolve auth
    auto auth_info = ResolveBusinessCentralAuth(context, secret_name);

    // Create OData client for companies endpoint
    auto client = BusinessCentralClientFactory::CreateCompaniesClient(
        auth_info.tenant_id,
        auth_info.environment,
        auth_info.auth_params
    );

    // Create bind data using OData infrastructure
    auto bind_data = make_uniq<BcShowCompaniesBindData>();
    bind_data->odata_bind_data = ODataReadBindData::FromEntitySetClient(client);

    // Get schema from OData
    names = bind_data->odata_bind_data->GetResultNames();
    return_types = bind_data->odata_bind_data->GetResultTypes();

    ERPL_TRACE_INFO("BC_FUNC", "bc_show_companies bound with " + std::to_string(names.size()) + " columns");
    return std::move(bind_data);
}

static void BcShowCompaniesScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<BcShowCompaniesGlobalState>();

    if (gstate.finished) {
        return;
    }

    auto rows_fetched = gstate.Scan().FetchNextResult(output);
    if (!gstate.Scan().HasMoreResults() && rows_fetched == 0) {
        gstate.finished = true;
    }
}

TableFunctionSet CreateBcShowCompaniesFunction() {
    TableFunctionSet set("bc_show_companies");

    TableFunction func({}, DATAZOO_GUARD(ERPL_ODATA_BANNER, BcShowCompaniesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, BcShowCompaniesBind), BcShowCompaniesInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;

    set.AddFunction(func);
    return set;
}

// ============================================================================
// bc_show_entities - List available entity sets in the Business Central API
// ============================================================================

struct BcShowEntitiesBindData : public TableFunctionData {
    std::unique_ptr<ODataReadBindData> odata_bind_data;
};

// Per-execution scan state (GitHub #191). `finished` lived on the bind data and was never
// reset, so a second EXECUTE of a bound plan returned zero rows silently.
class BcShowEntitiesGlobalState : public GlobalTableFunctionState {
public:
    explicit BcShowEntitiesGlobalState(std::unique_ptr<ODataReadBindData> scan_state)
        : scan_state(std::move(scan_state)) {}

    ODataReadBindData &Scan() { return *scan_state; }
    bool finished = false;

private:
    std::unique_ptr<ODataReadBindData> scan_state;
};

static unique_ptr<GlobalTableFunctionState> BcShowEntitiesInitGlobalState(
    ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->CastNoConst<BcShowEntitiesBindData>();
    return make_uniq<BcShowEntitiesGlobalState>(bind_data.odata_bind_data->CloneForScan());
}

static unique_ptr<FunctionData> BcShowEntitiesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("BC_FUNC", "Binding bc_show_entities");

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Resolve auth
    auto auth_info = ResolveBusinessCentralAuth(context, secret_name);

    // Create catalog client
    auto client = BusinessCentralClientFactory::CreateCatalogClient(
        auth_info.tenant_id,
        auth_info.environment,
        auth_info.auth_params
    );

    // Create bind data from service client (lists entity sets)
    auto bind_data = make_uniq<BcShowEntitiesBindData>();
    bind_data->odata_bind_data = ODataReadBindData::FromServiceClient(client);

    // Get schema
    names = bind_data->odata_bind_data->GetResultNames();
    return_types = bind_data->odata_bind_data->GetResultTypes();

    ERPL_TRACE_INFO("BC_FUNC", "bc_show_entities bound");
    return std::move(bind_data);
}

static void BcShowEntitiesScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<BcShowEntitiesGlobalState>();

    if (gstate.finished) {
        return;
    }

    auto rows_fetched = gstate.Scan().FetchNextResult(output);
    if (!gstate.Scan().HasMoreResults() && rows_fetched == 0) {
        gstate.finished = true;
    }
}

TableFunctionSet CreateBcShowEntitiesFunction() {
    TableFunctionSet set("bc_show_entities");

    TableFunction func({}, DATAZOO_GUARD(ERPL_ODATA_BANNER, BcShowEntitiesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, BcShowEntitiesBind), BcShowEntitiesInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;

    set.AddFunction(func);
    return set;
}

// ============================================================================
// bc_describe - Describe schema of a Business Central entity
// ============================================================================

struct BcDescribeBindData : public TableFunctionData {
    std::vector<std::string> property_names;
    std::vector<std::string> property_types;
    std::vector<bool> is_nullable;
    std::vector<bool> is_key;
};

// Per-execution row cursor (GitHub #191). It lived on the bind data and was never reset,
// so a second EXECUTE of a bound plan resumed past the end and returned nothing. The
// describe payload itself is immutable and stays shared.
class BcDescribeGlobalState : public GlobalTableFunctionState {
public:
    idx_t current_row = 0;
};

static unique_ptr<GlobalTableFunctionState> BcDescribeInitGlobalState(
    ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<BcDescribeGlobalState>();
}

static unique_ptr<FunctionData> BcDescribeBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("BC_FUNC", "Binding bc_describe");

    // Get entity name
    auto entity_name = input.inputs[0].GetValue<string>();

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Optional company parameter is accepted for API symmetry; metadata lookup stays at the service root.

    // Resolve auth
    auto auth_info = ResolveBusinessCentralAuth(context, secret_name);

    // Create catalog client to get metadata
    auto client = BusinessCentralClientFactory::CreateCatalogClient(
        auth_info.tenant_id,
        auth_info.environment,
        auth_info.auth_params
    );

    // Get metadata
    auto metadata = client->GetMetadata();

    // Find the entity type
    auto bind_data = make_uniq<BcDescribeBindData>();
    bool found = false;

    for (const auto &entity_set : metadata.FindEntitySets()) {
        if (entity_set.name == entity_name) {
            // Resolve the entity type from the metadata
            try {
                auto type_variant = metadata.FindType(entity_set.entity_type_name);
                auto* entity_type = std::get_if<EntityType>(&type_variant);

                if (entity_type) {
                    // Get key property names
                    std::set<std::string> key_props;
                    for (const auto &key_ref : entity_type->key.property_refs) {
                        key_props.insert(key_ref.name);
                    }

                    // Collect properties
                    for (const auto &prop : entity_type->properties) {
                        bind_data->property_names.push_back(prop.name);
                        bind_data->property_types.push_back(prop.type_name);
                        bind_data->is_nullable.push_back(prop.nullable);
                        bind_data->is_key.push_back(key_props.count(prop.name) > 0);
                    }
                    found = true;
                }
            } catch (const std::runtime_error &e) {
                // Type not found, continue searching
            }
            break;
        }
    }

    if (!found) {
        throw InvalidInputException("Entity '" + entity_name + "' not found in Business Central API. Use bc_show_entities() to list available entities.");
    }

    // Set up return schema
    names = {"property_name", "property_type", "nullable", "is_key"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN};

    ERPL_TRACE_INFO("BC_FUNC", "bc_describe bound for entity: " + entity_name);
    return std::move(bind_data);
}

static void BcDescribeScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &bind_data = data.bind_data->CastNoConst<BcDescribeBindData>();
    auto &gstate = data.global_state->Cast<BcDescribeGlobalState>();

    idx_t count = 0;
    while (gstate.current_row < bind_data.property_names.size() && count < STANDARD_VECTOR_SIZE) {
        output.SetValue(0, count, Value(bind_data.property_names[gstate.current_row]));
        output.SetValue(1, count, Value(bind_data.property_types[gstate.current_row]));
        output.SetValue(2, count, Value(bind_data.is_nullable[gstate.current_row]));
        output.SetValue(3, count, Value(bind_data.is_key[gstate.current_row]));
        gstate.current_row++;
        count++;
    }

    output.SetCardinality(count);
}

TableFunctionSet CreateBcDescribeFunction() {
    TableFunctionSet set("bc_describe");

    TableFunction func({LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, BcDescribeScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, BcDescribeBind), BcDescribeInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;
    func.named_parameters["company"] = LogicalType::VARCHAR;

    set.AddFunction(func);
    return set;
}

// ============================================================================
// bc_read - Read data from a Business Central entity with predicate pushdown
// ============================================================================

struct BcReadBindData : public TableFunctionData {
    std::unique_ptr<ODataReadBindData> odata_bind_data;
};

// Owns the scan state for ONE execution of a bound plan (GitHub #182).
//
// `finished` used to live on the bind data and was never reset, so the second EXECUTE of
// a prepared statement returned zero rows deterministically and silently. Both it and the
// row buffer now belong to the per-execution clone.
class BcReadGlobalState : public GlobalTableFunctionState {
public:
    explicit BcReadGlobalState(std::unique_ptr<ODataReadBindData> scan_state)
        : scan_state(std::move(scan_state)) {}

    ODataReadBindData &Scan() { return *scan_state; }
    bool finished = false;

private:
    std::unique_ptr<ODataReadBindData> scan_state;
};

static unique_ptr<FunctionData> BcReadBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    ERPL_TRACE_DEBUG("BC_FUNC", "Binding bc_read");

    // Get entity name
    auto entity_name = input.inputs[0].GetValue<string>();

    // Get secret parameter
    auto secret_name = input.named_parameters.at("secret").GetValue<string>();

    // Get company parameter (name or GUID, resolved below)
    string company_param;
    if (input.named_parameters.count("company")) {
        company_param = input.named_parameters.at("company").GetValue<string>();
    }

    // Resolve auth
    auto auth_info = ResolveBusinessCentralAuth(context, secret_name);

    // Resolve company name to GUID if a human-readable name was given
    auto company_id = BusinessCentralClientFactory::ResolveCompanyId(
        company_param, auth_info.tenant_id, auth_info.environment, auth_info.auth_params);

    // Build URLs. The API base is the OData service root; BC serves $metadata
    // there, NOT under companies({id}) — which is the default derived by
    // stripping the last path segment from the entity set URL.
    auto base_url = BusinessCentralUrlBuilder::BuildApiUrl(auth_info.tenant_id, auth_info.environment);
    auto company_url = BusinessCentralUrlBuilder::BuildCompanyUrl(base_url, company_id);
    auto entity_set_url = BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, entity_name);
    auto metadata_url = base_url + "/$metadata";

    // Create the OData client directly so we can pin the metadata URL to the
    // service root before GetResultNames() triggers DetectODataVersion().
    // FromEntitySetRoot's "conventional V4 path" would compute the wrong URL
    // (companies(id)/$metadata) at bind time when current_response is still null.
    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    HttpUrl es_url(entity_set_url);
    ODataUrlCodec::ensureJsonFormat(es_url);
    auto odata_client = std::make_shared<ODataEntitySetClient>(http_client, es_url, auth_info.auth_params);
    odata_client->SetMetadataContextUrl(metadata_url);

    auto bind_data = make_uniq<BcReadBindData>();
    bind_data->odata_bind_data = ODataReadBindData::FromEntitySetClient(odata_client);

    // Handle expand parameter — must call ProcessExpandClause (not just SetExpandClause)
    // so that ConsumeExpand feeds $expand into the predicate pushdown helper URL builder
    // and the expanded data schema paths are set up for column extraction.
    if (input.named_parameters.count("expand")) {
        auto expand_clause = input.named_parameters.at("expand").GetValue<string>();
        ODataReadBindHelpers::ProcessExpandClause(bind_data->odata_bind_data.get(), expand_clause);
    }

    // Get schema from OData (includes expanded columns if expand was set)
    names = bind_data->odata_bind_data->GetResultNames();
    return_types = bind_data->odata_bind_data->GetResultTypes();

    ERPL_TRACE_INFO("BC_FUNC", "bc_read bound for entity: " + entity_name + " with " + std::to_string(names.size()) + " columns");
    return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> BcReadInitGlobalState(
    ClientContext &context,
    TableFunctionInitInput &input) {

    auto &bind_data = input.bind_data->CastNoConst<BcReadBindData>();

    // Projection, filter pushdown and the first-page prefetch all mutate scan state, so
    // they run against a private clone; the bind data stays as bind left it. Returning
    // nullptr here - with the scan reading the bind data directly - is what made a second
    // EXECUTE return nothing (GitHub #182).
    auto scan_state = bind_data.odata_bind_data->CloneForScan();

    scan_state->ActivateColumns(input.column_ids);
    scan_state->AddFilters(input.filters);
    scan_state->UpdateUrlFromPredicatePushdown();
    scan_state->PrefetchFirstPage();

    return make_uniq<BcReadGlobalState>(std::move(scan_state));
}

static void BcReadScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<BcReadGlobalState>();

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
        gstate.Scan().ReportConversionFailures("bc_read");
    }
}

static double BcReadProgress(ClientContext &context, const FunctionData *bind_data_p,
                              const GlobalTableFunctionState *global_state) {
    // Progress lives on the per-execution clone, so reading it off the bind data would
    // report a scan that is no longer the one running (GitHub #182).
    if (global_state == nullptr) {
        return -1.0;
    }
    auto &gstate = const_cast<GlobalTableFunctionState *>(global_state)->Cast<BcReadGlobalState>();
    return gstate.Scan().GetProgressFraction();
}

TableFunctionSet CreateBcReadFunction() {
    TableFunctionSet set("bc_read");

    TableFunction func({LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, BcReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, BcReadBind), BcReadInitGlobalState);
    func.named_parameters["secret"] = LogicalType::VARCHAR;
    func.named_parameters["company"] = LogicalType::VARCHAR;
    func.named_parameters["expand"] = LogicalType::VARCHAR;

    // Enable pushdown features
    func.filter_pushdown = true;
    func.filter_prune = true;
    func.projection_pushdown = true;

    // Progress reporting
    func.table_scan_progress = BcReadProgress;

    set.AddFunction(func);
    return set;
}

} // namespace erpl_web
