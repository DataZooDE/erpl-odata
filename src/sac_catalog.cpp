#include "sac_catalog.hpp"
#include "sac_secret_helper.hpp"
#include "sac_catalog_bind_helper.hpp"
#include "sac_client.hpp"
#include "sac_url_builder.hpp"
#include "sac_generic_bind_data.hpp"
#include "graph_output_utils.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "odata_content.hpp"
#include "duckdb/function/table_function.hpp"
#include "telemetry.hpp"
#include <algorithm>
#include <optional>
#include "scan_row_cursor.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

// SacCatalogService implementation

SacCatalogService::SacCatalogService(
    const std::string& tenant,
    const std::string& region,
    std::shared_ptr<HttpAuthParams> auth_params)
    : tenant_(tenant), region_(region), auth_params_(auth_params) {

    // Create OData service client for metadata queries
    auto base_url = SacUrlBuilder::BuildODataServiceRootUrl(tenant, region);
    HttpUrl url(base_url);
    auto http_client = std::make_shared<HttpClient>();
    catalog_client_ = std::make_shared<ODataServiceClient>(http_client, url, auth_params);
    catalog_client_->SetODataVersionDirectly(ODataVersion::V4);
}

SacCatalogService::~SacCatalogService() = default;

std::vector<SacModel> SacCatalogService::ListModels() const {
    // NOT IMPLEMENTED, and says so rather than answering.
    //
    // This returned an empty result, which DuckDB cannot distinguish from a genuine
    // "this tenant has nothing" - a zero-row chunk IS end-of-scan. So against a live
    // tenant full of models, sac_show_models() reported success and no rows.
    //
    // It is not implemented here because SAC's catalog wire format is not verifiable from
    // this repository: there is no tenant, fixture or captured response to build a parser
    // against, and a parser written to a guessed shape would look verified without being
    // so. Throwing is the honest answer until a real response exists. See GitHub #243.
    throw duckdb::NotImplementedException(
        "SacCatalogService::ListModels is not implemented yet. SAC catalog discovery needs the tenant's OData service "
        "document, which this build cannot parse. Use the Datasphere functions, or open an "
        "issue with a captured SAC response if you need this.");
}

std::optional<SacModel> SacCatalogService::GetModel(const std::string& model_id) const {
    // NOT IMPLEMENTED, and says so rather than answering.
    //
    // This returned an empty result, which DuckDB cannot distinguish from a genuine
    // "this tenant has nothing" - a zero-row chunk IS end-of-scan. So against a live
    // tenant full of models, sac_show_models() reported success and no rows.
    //
    // It is not implemented here because SAC's catalog wire format is not verifiable from
    // this repository: there is no tenant, fixture or captured response to build a parser
    // against, and a parser written to a guessed shape would look verified without being
    // so. Throwing is the honest answer until a real response exists. See GitHub #243.
    throw duckdb::NotImplementedException(
        "SacCatalogService::GetModel is not implemented yet. SAC catalog discovery needs the tenant's OData service "
        "document, which this build cannot parse. Use the Datasphere functions, or open an "
        "issue with a captured SAC response if you need this.");
}

std::vector<SacStory> SacCatalogService::ListStories() const {
    // Same as ListModels: an empty result here is indistinguishable from a tenant with no
    // stories, so it answered authoritatively without having asked anything. See GitHub #243.
    throw duckdb::NotImplementedException(
        "SacCatalogService::ListStories is not implemented yet. SAC catalog discovery needs the "
        "tenant's OData service document, which this build cannot parse. Open an issue with a "
        "captured SAC response if you need this.");
}

std::optional<SacStory> SacCatalogService::GetStory(const std::string& story_id) const {
    // ListStories throws NotImplementedException until the catalog wire format is known; letting
    // it propagate keeps "not implemented" distinguishable from "no such story" (GitHub #243).
    const auto stories = ListStories();
    const auto it = std::find_if(stories.begin(), stories.end(),
        [&story_id](const SacStory& story) { return story.id == story_id; });
    if (it == stories.end()) {
        return std::nullopt;
    }
    return *it;
}

// ===== DuckDB Table Functions =====

// Using template: SacGenericBindData<SacModel> for sac_list_models
using SacShowModelsBindData = SacGenericBindData<SacModel>;

// Scan function for sac_list_models
static void SacShowModelsScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p,
                              duckdb::DataChunk &output) {
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();
    auto &bind_data = data_p.bind_data->Cast<SacShowModelsBindData>();

    idx_t count = 0;
    while (state.current_index < bind_data.items.size() && count < output.GetCapacity()) {
        const auto &model = bind_data.items[state.current_index];

        SetStrCellNN(output.data[0], count, model.id.c_str());
        SetStrCellNN(output.data[1], count, model.name.c_str());
        SetStrCellNN(output.data[2], count, model.description.c_str());
        SetStrCellNN(output.data[3], count, model.type.c_str());
        SetStrCellNN(output.data[4], count, model.owner.c_str());
        SetStrCellNN(output.data[5], count, model.created_at.c_str());
        SetStrCellNN(output.data[6], count, model.last_modified_at.c_str());

        state.current_index++;
        count++;
    }

    state.finished = (state.current_index >= bind_data.items.size());
    output.SetCardinality(count);
}

// Bind function for sac_list_models
static duckdb::unique_ptr<duckdb::FunctionData> SacShowModelsBind(
    duckdb::ClientContext &context,
    duckdb::TableFunctionBindInput &input,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("sac_show_models");

    // Extract and resolve credentials
    auto secret_name = SacCatalogBindHelper::ExtractSecretName(input);
    auto secret_data = SacCatalogBindHelper::ResolveSacCredentials(context, secret_name);
    auto catalog = SacCatalogBindHelper::CreateCatalogService(secret_data);

    // Fetch data
    auto models = catalog->ListModels();

    // Set return types and column names
    names = {"id", "name", "description", "type", "owner", "created_at", "last_modified_at"};
    return_types = SacCatalogBindHelper::CreateVarcharReturnTypes(names.size());

    // Populate bind data
    auto bind = duckdb::make_uniq<SacShowModelsBindData>();
    bind->items = models;

    return std::move(bind);
}

duckdb::TableFunctionSet CreateSacShowModelsFunction() {
    duckdb::TableFunctionSet function_set("sac_show_models");
    
    duckdb::TableFunction func(
        {},  // No parameters
        SacShowModelsScan,
        SacShowModelsBind
    );

    // Add optional named parameter for secret name
    func.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);

    func.init_global = ScanRowCursorState::Init;

    function_set.AddFunction(func);
    return function_set;
}

// ===== sac_list_stories =====

// Using template: SacGenericBindData<SacStory> for sac_list_stories
using SacShowStoriesBindData = SacGenericBindData<SacStory>;

static void SacShowStoriesScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p,
                               duckdb::DataChunk &output) {
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();
    auto &bind_data = data_p.bind_data->Cast<SacShowStoriesBindData>();

    idx_t count = 0;
    while (state.current_index < bind_data.items.size() && count < output.GetCapacity()) {
        const auto &story = bind_data.items[state.current_index];

        SetStrCellNN(output.data[0], count, story.id.c_str());
        SetStrCellNN(output.data[1], count, story.name.c_str());
        SetStrCellNN(output.data[2], count, story.description.c_str());
        SetStrCellNN(output.data[3], count, story.owner.c_str());
        SetStrCellNN(output.data[4], count, story.created_at.c_str());
        SetStrCellNN(output.data[5], count, story.last_modified_at.c_str());
        SetStrCellNN(output.data[6], count, story.status.c_str());

        state.current_index++;
        count++;
    }

    state.finished = (state.current_index >= bind_data.items.size());
    output.SetCardinality(count);
}

static duckdb::unique_ptr<duckdb::FunctionData> SacShowStoriesBind(
    duckdb::ClientContext &context,
    duckdb::TableFunctionBindInput &input,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("sac_show_stories");

    // Extract and resolve credentials
    auto secret_name = SacCatalogBindHelper::ExtractSecretName(input);
    auto secret_data = SacCatalogBindHelper::ResolveSacCredentials(context, secret_name);
    auto catalog = SacCatalogBindHelper::CreateCatalogService(secret_data);

    // Fetch data
    auto stories = catalog->ListStories();

    // Set return types and column names
    names = {"id", "name", "description", "owner", "created_at", "last_modified_at", "status"};
    return_types = SacCatalogBindHelper::CreateVarcharReturnTypes(names.size());

    // Populate bind data
    auto bind = duckdb::make_uniq<SacShowStoriesBindData>();
    bind->items = stories;

    return std::move(bind);
}

duckdb::TableFunctionSet CreateSacShowStoriesFunction() {
    duckdb::TableFunctionSet function_set("sac_show_stories");
    
    duckdb::TableFunction func(
        {},  // No parameters
        SacShowStoriesScan,
        SacShowStoriesBind
    );

    func.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);

    func.init_global = ScanRowCursorState::Init;

    function_set.AddFunction(func);
    return function_set;
}

// ===== sac_describe_model =====

// Using template: SacItemWithDetailsBindData<SacModel> for sac_describe_model
using SacGetModelInfoBindData = SacItemWithDetailsBindData<SacModel>;

static void SacGetModelInfoScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p,
                                duckdb::DataChunk &output) {
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();
    auto &bind_data = data_p.bind_data->Cast<SacGetModelInfoBindData>();

    if (!bind_data.item_found) {
        output.SetCardinality(0);
        state.finished = true;
        return;
    }

    idx_t count = 0;

    // Output one row for the model with dimension list
    if (state.current_index == 0) {
        // Build dimension list as comma-separated string
        std::string dims_str;
        for (size_t i = 0; i < bind_data.details.size(); ++i) {
            if (i > 0) dims_str += ", ";
            dims_str += bind_data.details[i];
        }

        SetStrCellNN(output.data[0], 0, bind_data.item.id.c_str());
        SetStrCellNN(output.data[1], 0, bind_data.item.name.c_str());
        SetStrCellNN(output.data[2], 0, bind_data.item.description.c_str());
        SetStrCellNN(output.data[3], 0, bind_data.item.type.c_str());
        SetStrCellNN(output.data[4], 0, dims_str.c_str());
        SetStrCellNN(output.data[5], 0, bind_data.item.created_at.c_str());

        state.current_index++;
        count = 1;
    }

    state.finished = true;
    output.SetCardinality(count);
}

static duckdb::unique_ptr<duckdb::FunctionData> SacGetModelInfoBind(
    duckdb::ClientContext &context,
    duckdb::TableFunctionBindInput &input,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("sac_describe_model");

    // Extract and validate parameters
    auto model_id = SacCatalogBindHelper::ExtractPositionalString(input, 0, "model_id");
    auto secret_name = SacCatalogBindHelper::ExtractSecretName(input);

    // Extract and resolve credentials
    auto secret_data = SacCatalogBindHelper::ResolveSacCredentials(context, secret_name);
    auto catalog = SacCatalogBindHelper::CreateCatalogService(secret_data);

    // Set return types and column names
    names = {"id", "name", "description", "type", "dimensions", "created_at"};
    return_types = SacCatalogBindHelper::CreateVarcharReturnTypes(names.size());

    // Fetch data
    auto model_opt = catalog->GetModel(model_id);

    // Populate bind data
    auto bind = duckdb::make_uniq<SacGetModelInfoBindData>();
    if (model_opt.has_value()) {
        bind->item = model_opt.value();
        bind->details = bind->item.dimensions;
        bind->item_found = true;
    } else {
        bind->item_found = false;
    }

    return std::move(bind);
}

duckdb::TableFunctionSet CreateSacGetModelInfoFunction() {
    duckdb::TableFunctionSet function_set("sac_describe_model");
    
    duckdb::TableFunction func(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},  // model_id
        SacGetModelInfoScan,
        SacGetModelInfoBind
    );

    func.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);

    func.init_global = ScanRowCursorState::Init;

    function_set.AddFunction(func);
    return function_set;
}

// ===== sac_describe_story =====

// Using template: SacSingleItemBindData<SacStory> for sac_describe_story
using SacGetStoryInfoBindData = SacSingleItemBindData<SacStory>;

static void SacGetStoryInfoScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p,
                                duckdb::DataChunk &output) {
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();
    auto &bind_data = data_p.bind_data->Cast<SacGetStoryInfoBindData>();

    if (!bind_data.item_found) {
        output.SetCardinality(0);
        state.finished = true;
        return;
    }

    idx_t count = 0;

    if (state.current_index == 0) {
        SetStrCellNN(output.data[0], 0, bind_data.item.id.c_str());
        SetStrCellNN(output.data[1], 0, bind_data.item.name.c_str());
        SetStrCellNN(output.data[2], 0, bind_data.item.description.c_str());
        SetStrCellNN(output.data[3], 0, bind_data.item.owner.c_str());
        SetStrCellNN(output.data[4], 0, bind_data.item.status.c_str());
        SetStrCellNN(output.data[5], 0, bind_data.item.created_at.c_str());
        SetStrCellNN(output.data[6], 0, bind_data.item.last_modified_at.c_str());

        state.current_index++;
        count = 1;
    }

    state.finished = true;
    output.SetCardinality(count);
}

static duckdb::unique_ptr<duckdb::FunctionData> SacGetStoryInfoBind(
    duckdb::ClientContext &context,
    duckdb::TableFunctionBindInput &input,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("sac_describe_story");

    // Extract and validate parameters
    auto story_id = SacCatalogBindHelper::ExtractPositionalString(input, 0, "story_id");
    auto secret_name = SacCatalogBindHelper::ExtractSecretName(input);

    // Extract and resolve credentials
    auto secret_data = SacCatalogBindHelper::ResolveSacCredentials(context, secret_name);
    auto catalog = SacCatalogBindHelper::CreateCatalogService(secret_data);

    // Set return types and column names
    names = {"id", "name", "description", "owner", "status", "created_at", "last_modified_at"};
    return_types = SacCatalogBindHelper::CreateVarcharReturnTypes(names.size());

    // Fetch data
    auto story_opt = catalog->GetStory(story_id);

    // Populate bind data
    auto bind = duckdb::make_uniq<SacGetStoryInfoBindData>();
    if (story_opt.has_value()) {
        bind->item = story_opt.value();
        bind->item_found = true;
    } else {
        bind->item_found = false;
    }

    return std::move(bind);
}

duckdb::TableFunctionSet CreateSacGetStoryInfoFunction() {
    duckdb::TableFunctionSet function_set("sac_describe_story");
    
    duckdb::TableFunction func(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},  // story_id
        SacGetStoryInfoScan,
        SacGetStoryInfoBind
    );

    func.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);

    func.init_global = ScanRowCursorState::Init;

    function_set.AddFunction(func);
    return function_set;
}

} // namespace erpl_web
