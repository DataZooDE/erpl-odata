#include "odata_odp_functions.hpp"
#include "tracing.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "odata_edm.hpp"
#include "odata_client.hpp"
#include "odata_url_helpers.hpp"
#include "duckdb_argument_helper.hpp"
#include "yyjson.hpp"
#include "duckdb/common/vector_size.hpp"
#include "telemetry.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include "scan_row_cursor.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

// -------------------------------------------------------------------------------------------------
// SapODataShowBindData implementation
// -------------------------------------------------------------------------------------------------

SapODataShowBindData::SapODataShowBindData(std::shared_ptr<HttpClient> http_client, 
                                           std::shared_ptr<HttpAuthParams> auth_params,
                                           const std::string& base_url)
    : http_client(std::move(http_client))
    , auth_params(std::move(auth_params))
    , base_url(base_url)
    , data_loaded(false)
{
}

void SapODataShowBindData::LoadServiceData() {
    if (data_loaded) {
        return;
    }
    
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Loading SAP OData services from: " + base_url);
    
    try {
        // Load V2 services first
        LoadV2Services();
        
        // Load V4 services
        LoadV4Services();
        
        data_loaded = true;
        ERPL_TRACE_INFO("SAP_ODATA_SHOW", "Service discovery completed, found " + std::to_string(service_data.size()) + " services total");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("SAP_ODATA_SHOW", "Error during service discovery: " + std::string(e.what()));
        // Re-throw the exception to propagate the error to the user
        throw;
    }
}

// -------------------------------------------------------------------------------------------------
// OdpODataShowBindData implementation
// -------------------------------------------------------------------------------------------------

OdpODataShowBindData::OdpODataShowBindData(std::shared_ptr<HttpClient> http_client, 
                                           std::shared_ptr<HttpAuthParams> auth_params,
                                           const std::string& base_url)
    : http_client(std::move(http_client))
    , auth_params(std::move(auth_params))
    , base_url(base_url)
    , data_loaded(false)
{
}

void OdpODataShowBindData::LoadOdpServiceData() {
    if (data_loaded) {
        return;
    }
    
    ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Loading ODP services from: " + base_url);
    
    try {
        // Use the same approach as odata_sap_show to ensure consistency
        std::string v2_catalog_url = base_url + "/sap/opu/odata/iwfnd/catalogservice;v=2/ServiceCollection";
        
        // Create OData V2 client using the same approach as odata_sap_show
        HttpParams http_params;
        http_params.url_encode = false;  // Disable URL encoding for OData V2
        auto odata_http_client = std::make_shared<HttpClient>(http_params);
        
        // Ensure $format=json and $expand=EntitySets just like odata_sap_show
        HttpUrl v2_url(v2_catalog_url);
        ODataUrlCodec::ensureJsonFormat(v2_url);
        v2_url.Query("?$expand=EntitySets");

        // Create OData client for the catalog service
        auto odata_client = std::make_shared<ODataEntitySetClient>(odata_http_client, v2_url, auth_params);
        odata_client->SetODataVersionDirectly(ODataVersion::V2);
        
        // Get the service data using the existing OData infrastructure
        auto response = odata_client->Get();
        if (!response) {
            throw std::runtime_error("Failed to get response from SAP OData V2 catalog endpoint");
        }
        
        ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Successfully retrieved V2 catalog using OData client");
        
        // Parse V2 response to extract ODP service information
        ParseOdpServicesFromV2Response(response);
        
        data_loaded = true;
        ERPL_TRACE_INFO("ODP_ODATA_SHOW", "ODP service discovery completed, found " + std::to_string(odp_service_data.size()) + " ODP entity sets");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_ODATA_SHOW", "Error during ODP service discovery: " + std::string(e.what()));
        // Re-throw the exception to propagate the error to the user
        throw;
    }
}

// -------------------------------------------------------------------------------------------------
// Function implementations
// -------------------------------------------------------------------------------------------------

static duckdb::unique_ptr<duckdb::FunctionData> SapODataShowBind(duckdb::ClientContext &context,
                                                                 duckdb::TableFunctionBindInput &input,
                                                                 duckdb::vector<duckdb::LogicalType> &return_types,
                                                                 duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("odata_sap_show");
    auto base_url = input.inputs[0].GetValue<std::string>();

    // Get HTTP client and auth params from DuckDB secrets
    auto auth_params = HttpAuthParams::FromDuckDbSecrets(context, base_url);
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Auth params type: " + std::to_string(static_cast<int>(auth_params->AuthType())));
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Auth params: " + auth_params->ToString());
    
    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    
    auto bind_data = duckdb::make_uniq<SapODataShowBindData>(http_client, auth_params, base_url);
    
    // Define return schema: service_id, description, version, service_url
    return_types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // service_id
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // description
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // version
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)   // service_url
    };
    
    names = {"service_id", "description", "version", "service_url"};
    
    return std::move(bind_data);
}

// A failed catalog request is an ordinary, user-actionable error (HTTP status, connection
// failure), not a defect in the extension, so it must not surface as INTERNAL (GitHub #261).
// Typed DuckDB exceptions keep their class; anything else is a transport failure.
[[noreturn]] static void ThrowDiscoveryError(const std::string& prefix, const std::exception& error) {
    const auto* duckdb_error = dynamic_cast<const duckdb::Exception*>(&error);
    if (duckdb_error == nullptr) {
        throw duckdb::IOException(prefix + error.what());
    }

    const duckdb::ErrorData error_data(*duckdb_error);
    if (error_data.Type() == duckdb::ExceptionType::INTERNAL) {
        throw duckdb::IOException(prefix + error_data.RawMessage());
    }
    throw duckdb::Exception(error_data.Type(), prefix + error_data.RawMessage());
}

static void SapODataShowScan(duckdb::ClientContext &context, 
                             duckdb::TableFunctionInput &data_p, 
                             duckdb::DataChunk &output) {
    auto &bind_data = data_p.bind_data->CastNoConst<SapODataShowBindData>();
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();

    try {
        // Bind-time snapshot by design: the service catalogue is fetched once and kept on
        // the bind data, so a re-executed prepared statement replays it instead of
        // re-discovering. The row cursor is per-execution, so every execution returns the
        // full list. See the re-execution contract in graph_json_scan.hpp.
        if (!bind_data.data_loaded) {
            bind_data.LoadServiceData();
        }
        
        // Return the discovered services
        ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Scan: service_data.size() = " + std::to_string(bind_data.service_data.size()) + ", next_index = " + std::to_string(state.current_index));
        if (bind_data.service_data.empty()) {
            ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Scan: service_data is empty, returning 0 rows");
            output.SetCardinality(0);
            return;
        }
    
    // Get the next batch of services (align with STANDARD_VECTOR_SIZE semantics)
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t start_idx = state.current_index;
    idx_t end_idx = std::min(start_idx + target, static_cast<idx_t>(bind_data.service_data.size()));
    idx_t count = end_idx - start_idx;
    
    if (count == 0) {
        output.SetCardinality(0);
        return;
    }
    
    // Fill the output chunk with service data
    for (idx_t i = 0; i < count; i++) {
        idx_t row_idx = start_idx + i;
        const auto& service_row = bind_data.service_data[row_idx];
        
        // Set each column value according to new schema
        output.data[0].SetValue(i, service_row[0]); // service_id
        output.data[1].SetValue(i, service_row[1]); // description
        output.data[2].SetValue(i, service_row[3]); // version
        output.data[3].SetValue(i, service_row[2]); // service_url
    }
    
    output.SetCardinality(count);
    state.current_index = end_idx;
    
    } catch (const std::exception& e) {
        ThrowDiscoveryError("SAP OData service discovery failed: ", e);
    }
}

static duckdb::unique_ptr<duckdb::FunctionData> OdpODataShowBind(duckdb::ClientContext &context,
                                                                 duckdb::TableFunctionBindInput &input,
                                                                 duckdb::vector<duckdb::LogicalType> &return_types,
                                                                 duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("odp_odata_show");
    auto base_url = input.inputs[0].GetValue<std::string>();

    // Get HTTP client and auth params from DuckDB secrets
    auto auth_params = HttpAuthParams::FromDuckDbSecrets(context, base_url);
    ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Auth params type: " + std::to_string(static_cast<int>(auth_params->AuthType())));
    ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Auth params: " + auth_params->ToString());
    
    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    
    auto bind_data = duckdb::make_uniq<OdpODataShowBindData>(http_client, auth_params, base_url);
    
    // Define return schema for ODP entity sets (one row per entity set)
    return_types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // service_id
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // service_description
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // entity_set_id
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // entity_set_description
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // entity_set_url
        duckdb::LogicalType(duckdb::LogicalTypeId::BOOLEAN)   // change_tracking
    };
    
    names = {"service_id", "service_description", "entity_set_id", "entity_set_description", "entity_set_url", "change_tracking"};
    
    return std::move(bind_data);
}

static void OdpODataShowScan(duckdb::ClientContext &context, 
                             duckdb::TableFunctionInput &data_p, 
                             duckdb::DataChunk &output) {
    auto &bind_data = data_p.bind_data->CastNoConst<OdpODataShowBindData>();
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();

    try {
        if (!bind_data.data_loaded) {
            ERPL_TRACE_DEBUG("ODP_SCAN", "Loading ODP service data in scan function");
            bind_data.LoadOdpServiceData();
        }
        
        ERPL_TRACE_DEBUG("ODP_SCAN", "ODP service data size: " + std::to_string(bind_data.odp_service_data.size()));
        
        // Return the discovered ODP services
        if (bind_data.odp_service_data.empty()) {
            ERPL_TRACE_DEBUG("ODP_SCAN", "No ODP service data found, returning 0 rows");
            output.SetCardinality(0);
            return;
        }
    
    // Get the next batch of services (align with STANDARD_VECTOR_SIZE semantics)
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t start_idx = state.current_index;
    idx_t end_idx = std::min(start_idx + target, static_cast<idx_t>(bind_data.odp_service_data.size()));
    idx_t count = end_idx - start_idx;
    
    ERPL_TRACE_DEBUG("ODP_SCAN", "start_idx: " + std::to_string(start_idx) + ", end_idx: " + std::to_string(end_idx) + ", count: " + std::to_string(count) + ", output.size(): " + std::to_string(output.size()));
    
    if (count == 0) {
        ERPL_TRACE_DEBUG("ODP_SCAN", "Count is 0, returning 0 rows");
        output.SetCardinality(0);
        return;
    }
    
    // Fill the output chunk with ODP service data
    for (idx_t i = 0; i < count; i++) {
        idx_t row_idx = start_idx + i;
        const auto& odp_service_row = bind_data.odp_service_data[row_idx];
        
        // Set each column value for entity set data
        output.data[0].SetValue(i, odp_service_row[0]); // service_id
        output.data[1].SetValue(i, odp_service_row[1]); // service_description
        output.data[2].SetValue(i, odp_service_row[2]); // entity_set_id
        output.data[3].SetValue(i, odp_service_row[3]); // entity_set_description
        output.data[4].SetValue(i, odp_service_row[4]); // entity_set_url
        output.data[5].SetValue(i, odp_service_row[5]); // change_tracking
    }
    
    output.SetCardinality(count);
    state.current_index = end_idx;
    
    } catch (const std::exception& e) {
        ThrowDiscoveryError("ODP OData service discovery failed: ", e);
    }
}


// -------------------------------------------------------------------------------------------------
// Private parsing methods
// -------------------------------------------------------------------------------------------------

// Helper function to extract values from XML
std::string ExtractXmlValue(const std::string& xml, const std::string& start_tag, const std::string& end_tag) {
    size_t start_pos = xml.find(start_tag);
    if (start_pos == std::string::npos) return "";
    
    start_pos += start_tag.length();
    size_t end_pos = xml.find(end_tag, start_pos);
    if (end_pos == std::string::npos) return "";
    
    return xml.substr(start_pos, end_pos - start_pos);
}

// Helper function to extract string values from JSON objects
std::string ExtractJsonString(duckdb_yyjson::yyjson_val* obj, const std::string& key) {
    if (!obj || !duckdb_yyjson::yyjson_is_obj(obj)) {
        return "";
    }
    
    auto val = duckdb_yyjson::yyjson_obj_get(obj, key.c_str());
    if (val && duckdb_yyjson::yyjson_is_str(val)) {
        return std::string(duckdb_yyjson::yyjson_get_str(val));
    }
    
    return "";
}

// Helper function to extract and add ODP entity sets as individual rows
void OdpODataShowBindData::ExtractAndAddOdpEntitySets(duckdb_yyjson::yyjson_val* service_entry, 
                                                      const std::string& service_id, 
                                                      const std::string& service_description,
                                                      const std::string& service_url) {
    // Look for EntitySets in the expanded data
    auto entity_sets_obj = duckdb_yyjson::yyjson_obj_get(service_entry, "EntitySets");
    if (!entity_sets_obj) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "No EntitySets found in service entry for " + service_id);
        return;
    }
    
    if (!duckdb_yyjson::yyjson_is_obj(entity_sets_obj)) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "EntitySets is not an object for " + service_id);
        return;
    }
    
    // Look for results array within EntitySets
    auto results_arr = duckdb_yyjson::yyjson_obj_get(entity_sets_obj, "results");
    if (!results_arr) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "No results array found in EntitySets for " + service_id);
        return;
    }
    
    if (!duckdb_yyjson::yyjson_is_arr(results_arr)) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "EntitySets.results is not an array for " + service_id);
        return;
    }
    
    // Iterate through each entity set
    duckdb_yyjson::yyjson_arr_iter arr_it;
    duckdb_yyjson::yyjson_arr_iter_init(results_arr, &arr_it);
    
    duckdb_yyjson::yyjson_val* entity_set;
    while ((entity_set = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
        if (duckdb_yyjson::yyjson_is_obj(entity_set)) {
            std::string entity_set_id = ExtractJsonString(entity_set, "ID");
            std::string entity_set_description = ExtractJsonString(entity_set, "Description");
            
            ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "Found entity set: " + entity_set_id);
            
            // Check if this entity set follows ODP patterns (case-insensitive)
            if (!entity_set_id.empty()) {
                std::string entity_name_upper = entity_set_id;
                std::transform(entity_name_upper.begin(), entity_name_upper.end(), entity_name_upper.begin(), ::toupper);
                
                if (entity_name_upper.find("ENTITYOF") == 0 || entity_name_upper.find("FACTSOF") == 0 || entity_name_upper.find("ATTROF") == 0) {
                    ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "Found ODP entity set: " + entity_set_id);
                    
                    // Construct the entity set URL for direct use with odata_read
                    // Use the entity_set_description (proper case) instead of entity_set_id (all caps)
                    std::string entity_set_url = service_url;
                    if (!entity_set_url.empty() && entity_set_url.back() != '/') {
                        entity_set_url += "/";
                    }
                    entity_set_url += entity_set_description;
                    
                    // Create a row for this ODP entity set
                    std::vector<duckdb::Value> odp_entity_row;
                    odp_entity_row.push_back(duckdb::Value(service_id));           // service_id
                    odp_entity_row.push_back(duckdb::Value(service_description));  // service_description
                    odp_entity_row.push_back(duckdb::Value(entity_set_id));        // entity_set_id
                    odp_entity_row.push_back(duckdb::Value(entity_set_description)); // entity_set_description
                    odp_entity_row.push_back(duckdb::Value(entity_set_url));       // entity_set_url
                    odp_entity_row.push_back(duckdb::Value(true));                 // change_tracking
                    
                    odp_service_data.push_back(odp_entity_row);
                    ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Added ODP entity set: " + entity_set_id + " from service: " + service_id + " with URL: " + entity_set_url);
                }
            }
        }
    }
}

// Helper function to extract ODP entity sets from JSON
std::string ExtractOdpEntitySetsFromJson(duckdb_yyjson::yyjson_val* service_entry) {
    std::vector<std::string> odp_entity_sets;
    
    // Look for EntitySets in the expanded data
    auto entity_sets_obj = duckdb_yyjson::yyjson_obj_get(service_entry, "EntitySets");
    if (!entity_sets_obj) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "No EntitySets found in service entry");
        return "";
    }
    
    if (!duckdb_yyjson::yyjson_is_obj(entity_sets_obj)) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "EntitySets is not an object");
        return "";
    }
    
    // Look for results array within EntitySets
    auto results_arr = duckdb_yyjson::yyjson_obj_get(entity_sets_obj, "results");
    if (!results_arr) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "No results array found in EntitySets");
        return "";
    }
    
    if (!duckdb_yyjson::yyjson_is_arr(results_arr)) {
        ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "EntitySets.results is not an array");
        return "";
    }
    
    // Iterate through entity sets
    duckdb_yyjson::yyjson_arr_iter arr_it;
    duckdb_yyjson::yyjson_arr_iter_init(results_arr, &arr_it);
    
    duckdb_yyjson::yyjson_val* entity_set;
    while ((entity_set = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
        if (duckdb_yyjson::yyjson_is_obj(entity_set)) {
            std::string entity_set_name = ExtractJsonString(entity_set, "ID");
            ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "Found entity set: " + entity_set_name);
            
            // Check if this entity set follows ODP patterns (case-insensitive)
            if (!entity_set_name.empty()) {
                std::string entity_name_upper = entity_set_name;
                std::transform(entity_name_upper.begin(), entity_name_upper.end(), entity_name_upper.begin(), ::toupper);
                
                if (entity_name_upper.find("ENTITYOF") == 0 || entity_name_upper.find("FACTSOF") == 0) {
                    ERPL_TRACE_DEBUG("ODP_ENTITY_EXTRACT", "Found ODP entity set: " + entity_set_name);
                    odp_entity_sets.push_back(entity_set_name);
                }
            }
        }
    }
    
    // Join entity sets with comma
    if (odp_entity_sets.empty()) return "";
    
    std::string result;
    for (size_t i = 0; i < odp_entity_sets.size(); ++i) {
        if (i > 0) result += ",";
        result += odp_entity_sets[i];
    }
    
    return result;
}

// Helper function to extract ODP entity sets from XML
std::string ExtractOdpEntitySets(const std::string& entry_xml) {
    std::vector<std::string> odp_entity_sets;
    
    // Look for EntitySets section in the expanded XML
    size_t entity_sets_start = entry_xml.find("<m:properties");
    if (entity_sets_start == std::string::npos) return "";
    
    // Find the EntitySets section
    size_t entity_sets_pos = entry_xml.find("EntitySets", entity_sets_start);
    if (entity_sets_pos == std::string::npos) return "";
    
    // Extract entity set names that follow ODP patterns
    // Look for patterns like EntityOf* or FactsOf*
    std::vector<std::string> patterns = {"EntityOf", "FactsOf"};
    
    for (const auto& pattern : patterns) {
        size_t pos = 0;
        while ((pos = entry_xml.find(pattern, pos)) != std::string::npos) {
            // Find the end of the entity set name (usually ends with quote or space)
            size_t name_start = pos;
            size_t name_end = entry_xml.find_first_of("\" \t\n\r", name_start);
            if (name_end == std::string::npos) {
                pos = name_start + 1;
                continue;
            }
            
            std::string entity_set_name = entry_xml.substr(name_start, name_end - name_start);
            if (!entity_set_name.empty()) {
                odp_entity_sets.push_back(entity_set_name);
            }
            
            pos = name_start + 1;
        }
    }
    
    // Join entity sets with comma
    if (odp_entity_sets.empty()) return "";
    
    std::string result;
    for (size_t i = 0; i < odp_entity_sets.size(); ++i) {
        if (i > 0) result += ",";
        result += odp_entity_sets[i];
    }
    
    return result;
}

void SapODataShowBindData::ParseODataV2CatalogFromResponse(std::shared_ptr<ODataEntitySetResponse> response) {
    // Use the same JSON parsing logic as odata_read to extract service information
    // This ensures consistency with the working OData V2 functionality
    
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Parsing OData V2 catalog JSON response");
    
    try {
        // Get the raw JSON content from the response
        auto content = response->RawContent();
        ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Response content length: " + std::to_string(content.length()));
        
        // Parse JSON using the same approach as odata_read
        auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
        if (!doc) {
            throw std::runtime_error("Failed to parse JSON response");
        }
        
        auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
        if (!root) {
            duckdb_yyjson::yyjson_doc_free(doc);
            throw std::runtime_error("Failed to get JSON root");
        }
        
        // Parse OData V2 format: {"d": {"results": [...]}}
        auto data_obj = duckdb_yyjson::yyjson_obj_get(root, "d");
        if (data_obj && duckdb_yyjson::yyjson_is_obj(data_obj)) {
            auto results_arr = duckdb_yyjson::yyjson_obj_get(data_obj, "results");
            if (results_arr && duckdb_yyjson::yyjson_is_arr(results_arr)) {
                ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Found results array in OData V2 response");
                
                // Iterate through each service entry
                duckdb_yyjson::yyjson_arr_iter arr_it;
                duckdb_yyjson::yyjson_arr_iter_init(results_arr, &arr_it);
                
                duckdb_yyjson::yyjson_val* service_entry;
                while ((service_entry = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
                    if (duckdb_yyjson::yyjson_is_obj(service_entry)) {
                        // Extract service information from JSON object
                        std::string service_id = ExtractJsonString(service_entry, "ID");
                        std::string description = ExtractJsonString(service_entry, "Description");
                        if (description.empty()) {
                            description = ExtractJsonString(service_entry, "Title");
                        }
                        std::string service_url = ExtractJsonString(service_entry, "ServiceUrl");
                        
                        if (!service_id.empty()) {
        // Create service row: [service_id, description, service_url, version]
        std::vector<duckdb::Value> service_row;
        service_row.push_back(duckdb::Value(service_id));
        service_row.push_back(duckdb::Value(description));
        service_row.push_back(duckdb::Value(service_url));
        service_row.push_back(duckdb::Value("V2"));
        
        service_data.push_back(service_row);
                            ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Added V2 service: " + service_id + " (total services: " + std::to_string(service_data.size()) + ")");
                        }
                    }
                }
            } else {
                ERPL_TRACE_WARN("SAP_ODATA_SHOW", "Could not find 'results' array in OData V2 response");
            }
        } else {
            ERPL_TRACE_WARN("SAP_ODATA_SHOW", "Could not find 'd' object in OData V2 response");
        }
        
        duckdb_yyjson::yyjson_doc_free(doc);
        ERPL_TRACE_INFO("SAP_ODATA_SHOW", "Parsed " + std::to_string(service_data.size()) + " V2 services from JSON");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("SAP_ODATA_SHOW", "Error parsing OData V2 catalog response: " + std::string(e.what()));
        throw;
    }
}

void SapODataShowBindData::LoadV2Services() {
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Loading V2 services from SAP OData catalog");
    
    // Use the exact same approach as odata_read to ensure consistency
    std::string v2_catalog_url = base_url + "/sap/opu/odata/iwfnd/catalogservice;v=2/ServiceCollection";
    
    // Create OData V2 client using the same approach as odata_read
    HttpParams http_params;
    http_params.url_encode = false;  // Disable URL encoding for OData V2
    auto odata_http_client = std::make_shared<HttpClient>(http_params);
    
    // Ensure $format=json just like odata_read
    HttpUrl v2_url(v2_catalog_url);
    ODataUrlCodec::ensureJsonFormat(v2_url);

    // Create OData client for the catalog service
    auto odata_client = std::make_shared<ODataEntitySetClient>(odata_http_client, v2_url, auth_params);
    odata_client->SetODataVersionDirectly(ODataVersion::V2);
    
    // Get the service data using the existing OData infrastructure
    auto response = odata_client->Get();
    if (!response) {
        throw std::runtime_error("Failed to get response from SAP OData V2 catalog endpoint");
    }
    
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Successfully retrieved V2 catalog using OData client");
    
    // Use the same parsing logic as odata_read to extract service information
    ParseODataV2CatalogFromResponse(response);
    
    ERPL_TRACE_INFO("SAP_ODATA_SHOW", "Loaded " + std::to_string(service_data.size()) + " V2 services");
}

void SapODataShowBindData::LoadV4Services() {
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Loading V4 services from SAP OData catalog");
    
    // Use the V4 catalog endpoint with expand for services
    std::string v4_catalog_url = base_url + "/sap/opu/odata4/iwfnd/config/default/iwfnd/catalog/0002/ServiceGroups";
    
    // Create OData V4 client using the same approach as odata_read
    HttpParams http_params;
    http_params.url_encode = false;  // Disable URL encoding for OData V4
    auto odata_http_client = std::make_shared<HttpClient>(http_params);
    
    // Build URL with expand parameter
    HttpUrl v4_url(v4_catalog_url);
    v4_url.Query("?$expand=DefaultSystem($expand=Services())");
    ODataUrlCodec::ensureJsonFormat(v4_url);

    // Create OData client for the catalog service
    auto odata_client = std::make_shared<ODataEntitySetClient>(odata_http_client, v4_url, auth_params);
    odata_client->SetODataVersionDirectly(ODataVersion::V4);
    
    // Get the service data using the existing OData infrastructure
    auto response = odata_client->Get();
    if (!response) {
        throw std::runtime_error("Failed to get response from SAP OData V4 catalog endpoint");
    }
    
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Successfully retrieved V4 catalog using OData client");
    
    // Parse V4 response to extract service information
    ParseODataV4CatalogFromResponse(response);
    
    ERPL_TRACE_INFO("SAP_ODATA_SHOW", "Loaded V4 services, total services now: " + std::to_string(service_data.size()));
}

void SapODataShowBindData::ParseODataV4CatalogFromResponse(std::shared_ptr<ODataEntitySetResponse> response) {
    ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Parsing OData V4 catalog JSON response");
    
    try {
        // Get the raw JSON content from the response
        auto content = response->RawContent();
        ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "V4 Response content length: " + std::to_string(content.length()));
        
        // Parse JSON using the same approach as odata_read
        auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
        if (!doc) {
            throw std::runtime_error("Failed to parse V4 JSON response");
        }
        
        auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
        if (!root) {
            duckdb_yyjson::yyjson_doc_free(doc);
            throw std::runtime_error("Failed to get V4 JSON root");
        }
        
        // Parse OData V4 format: {"value": [...]}
        auto value_arr = duckdb_yyjson::yyjson_obj_get(root, "value");
        if (value_arr && duckdb_yyjson::yyjson_is_arr(value_arr)) {
            ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Found value array in OData V4 response");
            
            // Iterate through each service group entry
            duckdb_yyjson::yyjson_arr_iter arr_it;
            duckdb_yyjson::yyjson_arr_iter_init(value_arr, &arr_it);
            
            duckdb_yyjson::yyjson_val* service_group;
            while ((service_group = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
                if (duckdb_yyjson::yyjson_is_obj(service_group)) {
                    // Extract DefaultSystem from service group
                    auto default_system = duckdb_yyjson::yyjson_obj_get(service_group, "DefaultSystem");
                    if (default_system && duckdb_yyjson::yyjson_is_obj(default_system)) {
                        // Extract Services array from DefaultSystem
                        auto services_arr = duckdb_yyjson::yyjson_obj_get(default_system, "Services");
                        if (services_arr && duckdb_yyjson::yyjson_is_arr(services_arr)) {
                            // Iterate through each service in the Services array
                            duckdb_yyjson::yyjson_arr_iter services_it;
                            duckdb_yyjson::yyjson_arr_iter_init(services_arr, &services_it);
                            
                            duckdb_yyjson::yyjson_val* service_entry;
                            while ((service_entry = duckdb_yyjson::yyjson_arr_iter_next(&services_it))) {
                                if (duckdb_yyjson::yyjson_is_obj(service_entry)) {
                                    // Extract service information from JSON object
                                    std::string service_id = ExtractJsonString(service_entry, "ServiceId");
                                    std::string description = ExtractJsonString(service_entry, "Description");
                                    if (description.empty()) {
                                        description = ExtractJsonString(service_entry, "ServiceAlias");
                                    }
                                    std::string service_url_relative = ExtractJsonString(service_entry, "ServiceUrl");
                                    
                                    if (!service_id.empty() && !service_url_relative.empty()) {
                                        // Build full service URL from relative path
                                        std::string full_service_url = base_url + service_url_relative;
                                        // Create service row
    std::vector<duckdb::Value> service_row;
                                        service_row.push_back(duckdb::Value(service_id));
                                        service_row.push_back(duckdb::Value(description));
                                        service_row.push_back(duckdb::Value(full_service_url));
    service_row.push_back(duckdb::Value("V4"));
    
    service_data.push_back(service_row);
                                        ERPL_TRACE_DEBUG("SAP_ODATA_SHOW", "Added V4 service: " + service_id + " (total services: " + std::to_string(service_data.size()) + ")");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            ERPL_TRACE_WARN("SAP_ODATA_SHOW", "Could not find 'value' array in OData V4 response");
        }
        
        duckdb_yyjson::yyjson_doc_free(doc);
        ERPL_TRACE_INFO("SAP_ODATA_SHOW", "Parsed V4 services from JSON");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("SAP_ODATA_SHOW", "Error parsing OData V4 catalog response: " + std::string(e.what()));
        throw;
    }
}

void OdpODataShowBindData::ParseOdpServicesFromV2Response(std::shared_ptr<ODataEntitySetResponse> response) {
    // Use the same JSON parsing logic as sap_odata_show to extract service information
    // This ensures consistency with the working OData V2 functionality
    
    ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Parsing OData V2 catalog JSON response for ODP services");
    
    try {
        // Get the raw JSON content from the response
        auto content = response->RawContent();
        ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Response content length: " + std::to_string(content.length()));
        
        // Parse JSON using the same approach as sap_odata_show
        auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
        if (!doc) {
            throw std::runtime_error("Failed to parse JSON response");
        }
        
        auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
        if (!root) {
            duckdb_yyjson::yyjson_doc_free(doc);
            throw std::runtime_error("Failed to get JSON root");
        }
        
        // Parse OData V2 format: {"d": {"results": [...]}}
        auto data_obj = duckdb_yyjson::yyjson_obj_get(root, "d");
        if (data_obj && duckdb_yyjson::yyjson_is_obj(data_obj)) {
            auto results_arr = duckdb_yyjson::yyjson_obj_get(data_obj, "results");
            if (results_arr && duckdb_yyjson::yyjson_is_arr(results_arr)) {
                ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Found results array in OData V2 response");
                
                // Iterate through each service entry
                duckdb_yyjson::yyjson_arr_iter arr_it;
                duckdb_yyjson::yyjson_arr_iter_init(results_arr, &arr_it);
                
                duckdb_yyjson::yyjson_val* service_entry;
                while ((service_entry = duckdb_yyjson::yyjson_arr_iter_next(&arr_it))) {
                    if (duckdb_yyjson::yyjson_is_obj(service_entry)) {
                        // Extract service information from JSON object
                        std::string service_id = ExtractJsonString(service_entry, "ID");
                        std::string description = ExtractJsonString(service_entry, "Description");
                        if (description.empty()) {
                            description = ExtractJsonString(service_entry, "Title");
                        }
                        std::string service_url = ExtractJsonString(service_entry, "ServiceUrl");
                        
                        // Extract and process each ODP entity set individually
                        if (!service_id.empty()) {
                            ExtractAndAddOdpEntitySets(service_entry, service_id, description, service_url);
                        }
                    }
                }
            } else {
                ERPL_TRACE_WARN("ODP_ODATA_SHOW", "Could not find 'results' array in OData V2 response");
            }
        } else {
            ERPL_TRACE_WARN("ODP_ODATA_SHOW", "Could not find 'd' object in OData V2 response");
        }
        
        duckdb_yyjson::yyjson_doc_free(doc);
        ERPL_TRACE_INFO("ODP_ODATA_SHOW", "Parsed " + std::to_string(odp_service_data.size()) + " ODP entity sets from JSON");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_ODATA_SHOW", "Error parsing OData V2 catalog response: " + std::string(e.what()));
        throw;
    }
}

void OdpODataShowBindData::ParseOdpServicesFromCatalog(const std::string& content) {
    // Parse Atom XML feed to extract ODP services
    // The content is an Atom XML feed with service entries and expanded EntitySets
    
    ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Parsing ODP services from catalog XML, content length: " + std::to_string(content.length()));
    
    // Simple XML parsing using string operations (for now)
    // In a production implementation, we'd use a proper XML parser
    
    size_t pos = 0;
    while ((pos = content.find("<entry>", pos)) != std::string::npos) {
        size_t entry_start = pos;
        size_t entry_end = content.find("</entry>", entry_start);
        if (entry_end == std::string::npos) break;
        
        std::string entry = content.substr(entry_start, entry_end - entry_start + 8);
        
        // Extract service ID
        std::string service_id = ExtractXmlValue(entry, "<d:ID>", "</d:ID>");
        if (service_id.empty()) {
            pos = entry_end + 8;
            continue;
        }
        
        // Extract description
        std::string description = ExtractXmlValue(entry, "<d:Description>", "</d:Description>");
        if (description.empty()) {
            description = ExtractXmlValue(entry, "<d:Title>", "</d:Title>");
        }
        
        // Extract service URL
        std::string service_url = ExtractXmlValue(entry, "<d:ServiceUrl>", "</d:ServiceUrl>");
        
        // Check if this is an ODP service by looking for EntitySets with ODP patterns
        std::string entity_sets = ExtractOdpEntitySets(entry);
        bool has_odp_entity_sets = !entity_sets.empty();
        
        // Only add services that have ODP entity sets
        if (has_odp_entity_sets) {
            std::vector<duckdb::Value> odp_service_row;
            odp_service_row.push_back(duckdb::Value(service_id));
            odp_service_row.push_back(duckdb::Value(description));
            odp_service_row.push_back(duckdb::Value(service_url));
            odp_service_row.push_back(duckdb::Value(entity_sets));
            odp_service_row.push_back(duckdb::Value(true)); // change_tracking (assume ODP services support it)
            
            odp_service_data.push_back(odp_service_row);
            ERPL_TRACE_DEBUG("ODP_ODATA_SHOW", "Added ODP service: " + service_id + " with entity sets: " + entity_sets);
        }
        
        pos = entry_end + 8;
    }
    
    ERPL_TRACE_INFO("ODP_ODATA_SHOW", "Parsed " + std::to_string(odp_service_data.size()) + " ODP services from XML");
}

// -------------------------------------------------------------------------------------------------
// Function registration
// -------------------------------------------------------------------------------------------------

duckdb::TableFunctionSet CreateODataSapShowFunction() {
    duckdb::TableFunctionSet function_set("odata_sap_show");
    
    duckdb::TableFunction sap_odata_show(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},  // base_url parameter
        SapODataShowScan, 
        SapODataShowBind
    );
    
    // Add named parameters for optional configuration
    sap_odata_show.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    sap_odata_show.init_global = ScanRowCursorState::Init;
    
    function_set.AddFunction(sap_odata_show);
    
    ERPL_TRACE_DEBUG("ODP_FUNCTION_REGISTRATION", "=== REGISTERING ODATA_SAP_SHOW FUNCTION ===");
    return function_set;
}

duckdb::TableFunctionSet CreateOdpODataShowFunction() {
    duckdb::TableFunctionSet function_set("odp_odata_show");
    
    duckdb::TableFunction odp_odata_show(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},  // base_url parameter
        OdpODataShowScan, 
        OdpODataShowBind
    );
    
    // Add named parameters for optional configuration
    odp_odata_show.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    
    odp_odata_show.init_global = ScanRowCursorState::Init;

    function_set.AddFunction(odp_odata_show);
    
    ERPL_TRACE_DEBUG("ODP_FUNCTION_REGISTRATION", "=== REGISTERING ODP_ODATA_SHOW FUNCTION ===");
    return function_set;
}


} // namespace erpl_web
