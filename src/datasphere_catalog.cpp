#include "datasphere_catalog.hpp"
#include <tuple>
#include <set>
#include "odata_url_helpers.hpp"
#include "datasphere_client.hpp"
#include "odata_client.hpp"
#include "odata_content.hpp"
#include "odata_edm.hpp"
#include "datazoo/oauth2/oauth2_flow_v2.hpp"
#include "datasphere_secret.hpp"
#include "graph_output_utils.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "telemetry.hpp"
#include <algorithm>
#include <unordered_set>
#include "erpl_odata_banner.hpp"
#include "scan_row_cursor.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

// Centralized OAuth2 configuration holder (values will be populated from DuckDB secret)

duckdb_yyjson::yyjson_val *RequireValidListingPage(const HttpResponse *response, const std::string &what,
                                                   int skip,
                                                   std::shared_ptr<duckdb_yyjson::yyjson_doc> &doc_out) {
    if (response == nullptr) {
        throw duckdb::IOException(
            "Datasphere listing failed: no response for %s (page starting at %d). The listing is "
            "incomplete.", what.c_str(), skip);
    }
    if (response->Code() != 200) {
        throw duckdb::IOException(
            "Datasphere listing failed: HTTP %d for %s (page starting at %d). The listing is "
            "incomplete.", response->Code(), what.c_str(), skip);
    }

    const std::string &content = response->Content();
    doc_out = std::shared_ptr<duckdb_yyjson::yyjson_doc>(
        duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0), duckdb_yyjson::yyjson_doc_free);
    if (!doc_out) {
        throw duckdb::IOException(
            "Datasphere listing returned a page that is not valid JSON for %s (page starting at %d). "
            "The listing is incomplete.", what.c_str(), skip);
    }

    auto *root = duckdb_yyjson::yyjson_doc_get_root(doc_out.get());
    if (!duckdb_yyjson::yyjson_is_arr(root)) {
        throw duckdb::IOException(
            "Datasphere listing returned a page whose root is not an array for %s (page starting at "
            "%d). The listing is incomplete.", what.c_str(), skip);
    }
    return root;
}

OAuth2Config GetDatasphereOAuth2Config() {
    // Do not hardcode environment parameters; they will be supplied via DuckDB secret
    OAuth2Config config;
    return config;
}

// Centralized function to get or refresh OAuth2 token
std::string GetOrRefreshDatasphereToken(duckdb::ClientContext &context, OAuth2Config &config,
                                        const std::string &secret_name) {
    std::string access_token;

    // The secret is named by the caller. This used to be hardcoded to "datasphere" while
    // two binds parsed a `secret` named parameter that was never registered on any of these
    // functions - so the parameter could not be passed at all (the binder rejected it), and
    // the parsing code was unreachable. Every catalog function was locked to one secret,
    // while the read path threaded the name correctly: the same guard-at-one-seam shape.
    // See GitHub #243.
    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    std::unique_ptr<duckdb::SecretEntry> secret_entry;
    try {
        secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
    } catch (...) {
        secret_entry = nullptr;
    }

    if (!secret_entry) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' not found. Please create it using CREATE SECRET " + secret_name + " (type 'datasphere', provider 'oauth2', client_id => '...', client_secret => '...', tenant_name => '...', data_center => '...', scope => 'default', redirect_uri => 'http://localhost:65000');");
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret*>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' is not a KeyValueSecret");
    }

    // Always populate config fields from secret for downstream URL construction/debug
    auto tenant_val = kv_secret->secret_map.find("tenant_name");
    auto data_center_val = kv_secret->secret_map.find("data_center");
    auto scope_val = kv_secret->secret_map.find("scope");
    auto redirect_uri_val = kv_secret->secret_map.find("redirect_uri");
    if (tenant_val != kv_secret->secret_map.end()) config.tenant_name = tenant_val->second.ToString();
    if (data_center_val != kv_secret->secret_map.end()) config.data_center = data_center_val->second.ToString();
    if (scope_val != kv_secret->secret_map.end()) config.scope = scope_val->second.ToString();
    if (redirect_uri_val != kv_secret->secret_map.end()) config.redirect_uri = redirect_uri_val->second.ToString();

    if (DatasphereTokenManager::IsTokenValid(kv_secret)) {
        access_token = DatasphereTokenManager::GetToken(context, kv_secret);
    } else {
        DatasphereTokenManager::RefreshTokens(context, kv_secret);
        access_token = DatasphereTokenManager::GetToken(context, kv_secret);
    }

    return access_token;
}

namespace {

class JsonDocHandle {
public:
    explicit JsonDocHandle(const std::string &json_content)
        : doc(duckdb_yyjson::yyjson_read(json_content.c_str(), json_content.length(), 0)) {
    }

    ~JsonDocHandle() {
        if (doc) {
            duckdb_yyjson::yyjson_doc_free(doc);
        }
    }

    JsonDocHandle(const JsonDocHandle &) = delete;
    JsonDocHandle &operator=(const JsonDocHandle &) = delete;

    explicit operator bool() const {
        return doc != nullptr;
    }

    duckdb_yyjson::yyjson_val *Root() const {
        return doc ? duckdb_yyjson::yyjson_doc_get_root(doc) : nullptr;
    }

private:
    duckdb_yyjson::yyjson_doc *doc;
};

struct AnalyticalSchemaField {
    std::string name;
    std::string type;
    std::string edm_type;
};

struct RelationalSchemaColumn {
    std::string name;
    std::string technical_name;
    std::string type;
    std::string length;
};

duckdb::LogicalType VarcharType() {
    return duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
}

duckdb::LogicalType AnalyticalSchemaFieldType() {
    return duckdb::LogicalType::STRUCT({
        {"name", VarcharType()},
        {"type", VarcharType()},
        {"edm_type", VarcharType()}
    });
}

duckdb::LogicalType RelationalSchemaColumnType() {
    return duckdb::LogicalType::STRUCT({
        {"name", VarcharType()},
        {"technical_name", VarcharType()},
        {"type", VarcharType()},
        {"length", VarcharType()}
    });
}

duckdb::LogicalType AnalyticalSchemaType() {
    return duckdb::LogicalType::STRUCT({
        {"measures", duckdb::LogicalType::LIST(AnalyticalSchemaFieldType())},
        {"dimensions", duckdb::LogicalType::LIST(AnalyticalSchemaFieldType())},
        {"variables", duckdb::LogicalType::LIST(AnalyticalSchemaFieldType())}
    });
}

duckdb::Value MakeAnalyticalSchemaFieldValue(const AnalyticalSchemaField &field) {
    duckdb::child_list_t<duckdb::Value> children;
    children.emplace_back("name", duckdb::Value(field.name));
    children.emplace_back("type", duckdb::Value(field.type));
    children.emplace_back("edm_type", duckdb::Value(field.edm_type));
    return duckdb::Value::STRUCT(children);
}

duckdb::Value MakeRelationalSchemaColumnValue(const RelationalSchemaColumn &column) {
    duckdb::child_list_t<duckdb::Value> children;
    children.emplace_back("name", duckdb::Value(column.name));
    children.emplace_back("technical_name", duckdb::Value(column.technical_name));
    children.emplace_back("type", duckdb::Value(column.type));
    children.emplace_back("length", duckdb::Value(column.length));
    return duckdb::Value::STRUCT(children);
}

duckdb::Value MakeAnalyticalSchemaValue(const duckdb::vector<duckdb::Value> &measures,
                                        const duckdb::vector<duckdb::Value> &dimensions,
                                        const duckdb::vector<duckdb::Value> &variables) {
    duckdb::child_list_t<duckdb::Value> children;
    children.emplace_back("measures", duckdb::Value::LIST(AnalyticalSchemaFieldType(), measures));
    children.emplace_back("dimensions", duckdb::Value::LIST(AnalyticalSchemaFieldType(), dimensions));
    children.emplace_back("variables", duckdb::Value::LIST(AnalyticalSchemaFieldType(), variables));
    return duckdb::Value::STRUCT(children);
}

duckdb::Value MakeAnalyticalSchemaValue(const std::vector<AnalyticalSchemaField> &measures,
                                        const std::vector<AnalyticalSchemaField> &dimensions,
                                        const std::vector<AnalyticalSchemaField> &variables) {
    duckdb::vector<duckdb::Value> measure_values;
    duckdb::vector<duckdb::Value> dimension_values;
    duckdb::vector<duckdb::Value> variable_values;
    measure_values.reserve(measures.size());
    dimension_values.reserve(dimensions.size());
    variable_values.reserve(variables.size());

    for (const auto &measure : measures) {
        measure_values.push_back(MakeAnalyticalSchemaFieldValue(measure));
    }
    for (const auto &dimension : dimensions) {
        dimension_values.push_back(MakeAnalyticalSchemaFieldValue(dimension));
    }
    for (const auto &variable : variables) {
        variable_values.push_back(MakeAnalyticalSchemaFieldValue(variable));
    }

    return MakeAnalyticalSchemaValue(measure_values, dimension_values, variable_values);
}

duckdb::Value MakeEmptyAnalyticalSchemaValue() {
    duckdb::vector<duckdb::Value> empty;
    return MakeAnalyticalSchemaValue(empty, empty, empty);
}

duckdb::Value MakeRelationalSchemaValue(const duckdb::vector<duckdb::Value> &columns) {
    duckdb::child_list_t<duckdb::Value> children;
    children.emplace_back("columns", duckdb::Value::LIST(RelationalSchemaColumnType(), columns));
    return duckdb::Value::STRUCT(children);
}

duckdb::Value MakeEmptyRelationalSchemaValue() {
    return MakeRelationalSchemaValue(duckdb::vector<duckdb::Value> {});
}

std::string JsonStringOrDefault(duckdb_yyjson::yyjson_val *object, const char *key, const std::string &fallback) {
    auto value = duckdb_yyjson::yyjson_obj_get(object, key);
    if (value && duckdb_yyjson::yyjson_is_str(value)) {
        return duckdb_yyjson::yyjson_get_str(value);
    }
    return fallback;
}

std::string JsonIntStringOrDefault(duckdb_yyjson::yyjson_val *object, const char *key, const std::string &fallback) {
    auto value = duckdb_yyjson::yyjson_obj_get(object, key);
    if (value && duckdb_yyjson::yyjson_is_int(value)) {
        return std::to_string(duckdb_yyjson::yyjson_get_int(value));
    }
    return fallback;
}

duckdb_yyjson::yyjson_val *FirstObjectMemberValue(duckdb_yyjson::yyjson_val *object) {
    if (!object || !duckdb_yyjson::yyjson_is_obj(object)) {
        return nullptr;
    }

    duckdb_yyjson::yyjson_obj_iter iter;
    duckdb_yyjson::yyjson_obj_iter_init(object, &iter);
    auto *key = duckdb_yyjson::yyjson_obj_iter_next(&iter);
    return key ? duckdb_yyjson::yyjson_obj_iter_get_val(key) : nullptr;
}

duckdb_yyjson::yyjson_val *FirstDefinitionElements(duckdb_yyjson::yyjson_val *root) {
    auto *definitions = duckdb_yyjson::yyjson_obj_get(root, "definitions");
    auto *definition = FirstObjectMemberValue(definitions);
    if (!definition || !duckdb_yyjson::yyjson_is_obj(definition)) {
        return nullptr;
    }

    auto *elements = duckdb_yyjson::yyjson_obj_get(definition, "elements");
    return elements && duckdb_yyjson::yyjson_is_obj(elements) ? elements : nullptr;
}

bool LooksLikeDwaasMeasure(const std::string &field_name) {
    return field_name.find("count") != std::string::npos ||
           field_name.find("Count") != std::string::npos ||
           field_name.find("revenue") != std::string::npos ||
           field_name.find("amount") != std::string::npos ||
           field_name.find("sum") != std::string::npos;
}

} // namespace

// DatasphereShowBindData implementation - now using standard OData pipeline
DatasphereShowBindData::DatasphereShowBindData(std::shared_ptr<ODataEntitySetClient> odata_client)
    : ODataReadBindData(odata_client)
    , resource_type("spaces") // Default to spaces
    , space_id("")
{
}

std::vector<std::string> DatasphereShowBindData::GetResultNames(bool all_columns) {
    if (resource_type == "assets") {
        // Return asset-specific column names based on AssetEntityV1 schema
        return {"name", "space_name", "label", "supports_analytical_queries"};
    } else {
        // Return space-specific column names (default)
    return {"name", "label", "description", "created_at", "modified_at"};
    }
}

std::vector<duckdb::LogicalType> DatasphereShowBindData::GetResultTypes(bool all_columns) {
    if (resource_type == "assets") {
        // Return asset-specific column types based on AssetEntityV1 schema
            return {
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // name
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // space_name
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),  // label
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)   // supports_analytical_queries
    };
    } else {
        // Return space-specific column types (default)
        return {
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)
    };
    }
}

// DatasphereDescribeBindData implementation
DatasphereDescribeBindData::DatasphereDescribeBindData(std::shared_ptr<ODataServiceClient> catalog_client, 
                                                       const std::string& resource_type,
                                                       const std::string& resource_id)
    : catalog_client(catalog_client)
    , resource_type(resource_type)
    , resource_id(resource_id)
{
}

// NOTE ON REACHABILITY: this function is currently DEAD CODE. Its only caller gates on
// `resource_data[0].size() < 15`, and both paths that build an asset row produce exactly
// 15 columns (the DWAAS path pushes 15; the catalog fallback resize(15)s). Rows shorter
// than that are 2-element error rows, which hit the `size() < 8` return below.
//
// The credential gate in here is therefore defence in depth, not a live fix: the #205
// exposure on this path is LATENT. It is written rather than skipped because the guard
// must already be in place if reachability is ever restored - at which point the wrong
// column index below would also start mattering. Restoring reachability is a behavioural
// change (it would start issuing metadata requests that never happen today) and is
// deliberately not part of the #205 fix.
std::vector<duckdb::Value> DatasphereDescribeBindData::FetchAssetExtendedMetadata(duckdb::ClientContext &context, 
                                                                                   const OAuth2Config &config,
                                                                                   const std::shared_ptr<HttpAuthParams> &auth_params) {
    std::vector<duckdb::Value> extended_data;
    
    try {
        // The origin every service-supplied metadata URL is measured against. It comes from
        // the CONFIGURED tenant, not from the response: tenant_name / data_center are also
        // parsed out of the returned URLs further down, and an origin derived from the URL
        // under test would make the credential gate vacuous (GitHub #205).
        const std::string catalog_service_origin =
            (!config.tenant_name.empty() && !config.data_center.empty())
                ? DatasphereUrlBuilder::BuildCatalogUrl(config.tenant_name, config.data_center)
                : std::string();

        // Get the basic asset data to extract URLs
        if (resource_data.empty() || resource_data[0].size() < 8) {
            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Cannot fetch extended metadata without basic asset data");
            return extended_data;
        }
        
        auto &asset_row = resource_data[0];
        std::string relational_metadata_url = asset_row[3].ToString(); // assetRelationalMetadataUrl
        // Column 5, not 6: 5 is assetAnalyticalMetadataUrl and 6 is assetAnalyticalDataUrl
        // (see the row construction below and the projection in the describe scan). This
        // read has always named the wrong column; it is latent only because this whole
        // function is currently unreachable - see the note at the top.
        std::string analytical_metadata_url = asset_row[5].ToString(); // assetAnalyticalMetadataUrl
        std::string supports_analytical = asset_row[7].ToString(); // supports_analytical_queries
        
        // Extract tenant and data center from the analytical metadata URL if available
        std::string tenant_name = "unknown";
        std::string data_center = "unknown";
        
        if (analytical_metadata_url != "NULL" && !analytical_metadata_url.empty()) {
            // Parse URL: https://ak-datasphere-prd.eu10.hcs.cloud.sap/api/v1/dwc/consumption/analytical/...
            size_t protocol_end = analytical_metadata_url.find("://");
            if (protocol_end != std::string::npos) {
                size_t host_start = protocol_end + 3;
                size_t first_dot = analytical_metadata_url.find('.', host_start);
                if (first_dot != std::string::npos) {
                    tenant_name = analytical_metadata_url.substr(host_start, first_dot - host_start);
                    size_t second_dot = analytical_metadata_url.find('.', first_dot + 1);
                    if (second_dot != std::string::npos) {
                        data_center = analytical_metadata_url.substr(first_dot + 1, second_dot - first_dot - 1);
                    }
                }
            }
        }
        
        // Add OData context
        extended_data.push_back(duckdb::Value(DatasphereUrlBuilder::BuildCatalogUrl(tenant_name, data_center) + "/$metadata"));
        
        // Fetch relational schema if available
        std::string relational_schema = "Not available";
        if (relational_metadata_url != "NULL" && !relational_metadata_url.empty()) {
            try {
                relational_schema = FetchMetadataSummary(relational_metadata_url, auth_params, "relational",
                                                        catalog_service_origin);
        } catch (...) {
                relational_schema = "Fetch failed";
            }
        }
        extended_data.push_back(duckdb::Value(relational_schema));
        
        // Fetch analytical schema if available
        duckdb::Value analytical_schema = duckdb::Value("Not available");
        if (analytical_metadata_url != "NULL" && !analytical_metadata_url.empty()) {
            try {
                analytical_schema = FetchDetailedAnalyticalSchema(analytical_metadata_url, auth_params,
                                                                 catalog_service_origin);
            } catch (...) {
                analytical_schema = duckdb::Value("Fetch failed");
            }
        }
        extended_data.push_back(analytical_schema);
        
        // Add derived boolean fields
        extended_data.push_back(duckdb::Value(relational_metadata_url != "NULL" && !relational_metadata_url.empty() ? "true" : "false"));
        extended_data.push_back(duckdb::Value(analytical_metadata_url != "NULL" && !analytical_metadata_url.empty() ? "true" : "false"));
        
        // Determine asset type
        std::string asset_type = "Unknown";
        if (relational_metadata_url != "NULL" && analytical_metadata_url != "NULL") {
            asset_type = "Multi-Modal";
        } else if (relational_metadata_url != "NULL") {
            asset_type = "Relational";
        } else if (analytical_metadata_url != "NULL") {
            asset_type = "Analytical";
        }
        extended_data.push_back(duckdb::Value(asset_type));
        
        // Add OData metadata ETag (placeholder for now)
        extended_data.push_back(duckdb::Value(""));
        
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Successfully fetched extended metadata for asset");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Error fetching extended metadata: " + std::string(e.what()));
        // Return fallback values
        extended_data = {
            duckdb::Value("Error"),           // odataContext
            duckdb::Value("Error"),           // relationalSchema
            duckdb::Value("Error"),           // analyticalSchema
            duckdb::Value("false"),           // hasRelationalAccess
            duckdb::Value("false"),           // hasAnalyticalAccess
            duckdb::Value("Unknown"),         // assetType
            duckdb::Value("")                 // odataMetadataEtag
        };
    }
    
    return extended_data;
}


// A URL that arrives in a response body is attacker-controlled the moment the service is
// compromised or impersonated, so the caller's bearer token may only follow it when it
// names the origin this client was opened against. Returns the credentials to use: the
// caller's for a same-origin URL, none otherwise.
//
// Fails CLOSED. Written as `origin.empty() || IsSameOrigin(...)` this would attach
// credentials to whatever host the service named whenever the origin had not been
// recorded - the wrong polarity for a credential decision (GitHub #187, #205).
std::shared_ptr<HttpAuthParams> CredentialsForServiceSuppliedUrl(
    const std::string &url, const std::string &service_origin,
    const std::shared_ptr<HttpAuthParams> &auth_params, const char *what)
{
    if (!auth_params) {
        return nullptr;
    }
    if (!IsWireSafeUrl(url)) {
        ERPL_TRACE_WARN("DATASPHERE_CATALOG",
                        "A metadata URL from the catalog response contains characters that cannot "
                        "be sent in a request; refusing to use it with credentials");
        return nullptr;
    }
    try {
        if (!service_origin.empty() && HttpUrl(url).IsSameOrigin(HttpUrl(service_origin))) {
            return auth_params;
        }
    } catch (const std::exception &e) {
        // A URL we cannot parse is not a URL we send credentials to.
        ERPL_TRACE_WARN("DATASPHERE_CATALOG", "Could not compare origins: " + std::string(e.what()));
    }
    ERPL_TRACE_WARN("DATASPHERE_CATALOG",
                    std::string(what) + " URL from the catalog response points at a different "
                    "origin than the service (" + service_origin + " -> " + url +
                    "); requesting it without credentials");
    return nullptr;
}

std::string DatasphereDescribeBindData::FetchMetadataSummary(const std::string &metadata_url, 
                                                           const std::shared_ptr<HttpAuthParams> &auth_params,
                                                           const std::string &metadata_type,
                                                           const std::string &service_origin) {
    try {
        // Create HTTP client for metadata fetch
        auto http_client = std::make_shared<HttpClient>();

        const auto credentials = CredentialsForServiceSuppliedUrl(metadata_url, service_origin,
                                                                  auth_params, "Relational metadata");

        // Fetch metadata (we'll just get a summary for now to avoid parsing complex XML/JSON)
        auto metadata_client = std::make_shared<ODataServiceClient>(
            http_client,
            HttpUrl(metadata_url),
            credentials
        );
        
        auto metadata_response = metadata_client->Get();
        if (metadata_response) {
            // For now, return a simple summary - in a full implementation, we'd parse the metadata
            // and extract entity sets, properties, dimensions, measures, etc.
            if (metadata_type == "relational") {
                return "Relational metadata available (tables, columns, relationships)";
            } else if (metadata_type == "analytical") {
                return "Analytical metadata available (dimensions, measures, hierarchies)";
                } else {
                return "Metadata available";
            }
        } else {
            return "Metadata fetch failed";
        }
    } catch (const std::exception& e) {
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to fetch metadata from " + metadata_url + ": " + std::string(e.what()));
        return "Metadata fetch error";
    }
}

duckdb::Value DatasphereDescribeBindData::FetchDetailedAnalyticalSchema(const std::string &metadata_url, 
                                                                       const std::shared_ptr<HttpAuthParams> &auth_params,
                                                                       const std::string &service_origin) {
    try {
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Fetching detailed analytical schema from: " + metadata_url);
        
        // Create HTTP client for metadata fetch
        auto http_client = std::make_shared<HttpClient>();

        const auto credentials = CredentialsForServiceSuppliedUrl(metadata_url, service_origin,
                                                                  auth_params, "Analytical metadata");

        // Fetch the actual metadata document
        auto metadata_client = std::make_shared<ODataServiceClient>(
            http_client, 
            HttpUrl(metadata_url),
            credentials
        );
        
        auto metadata_response = metadata_client->Get();
        if (!metadata_response) {
            return "Metadata fetch failed";
        }
        
        // Let's try to get the raw content directly from the HTTP response
        // We'll use a direct HTTP client to bypass the OData parsing layer
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Attempting to fetch raw metadata content from: " + metadata_url);
        
        // The metadata_url from the asset response might be pointing to the data endpoint
        // We need to construct the proper metadata URL by appending $metadata
        std::string metadata_endpoint_url = metadata_url;
        if (metadata_endpoint_url.find("$metadata") == std::string::npos) {
            // Remove trailing slash if present
            if (!metadata_endpoint_url.empty() && metadata_endpoint_url.back() == '/') {
                metadata_endpoint_url.pop_back();
            }
            metadata_endpoint_url += "/$metadata";
        }
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Constructed metadata endpoint URL: " + metadata_endpoint_url);
        
        // Same client the rest of the OData stack uses; a default-constructed HttpClient
        // re-encodes the URL and mangles OData query options (GitHub #102).
        auto metadata_http_client = CreateODataHttpClient();
        
        // Create a direct HTTP request to the metadata URL
        // The $metadata URL is derived from the same service-supplied URL, so it is gated
        // against the same origin rather than trusted because it was rewritten here.
        const auto endpoint_credentials = CredentialsForServiceSuppliedUrl(
            metadata_endpoint_url, service_origin, auth_params, "Analytical $metadata");
        HttpRequest metadata_request(HttpMethod::GET, HttpUrl(metadata_endpoint_url));
        if (endpoint_credentials) {
            metadata_request.AuthHeadersFromParams(*endpoint_credentials);
        }
        
        // Execute the request
        auto raw_response = metadata_http_client->SendRequest(metadata_request);
        if (!raw_response) {
            return "Failed to get raw metadata response";
        }
        
        // Extract the raw content string
        std::string raw_content = raw_response->Content();
        if (raw_content.empty()) {
            return "Empty raw metadata response";
        }
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Raw metadata content received, size: " + std::to_string(raw_content.size()) + " bytes");
        
        // Log a preview of the content for debugging
        std::string content_preview = raw_content.substr(0, 2000);
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Content preview: " + content_preview);
        
        // Now parse the XML content to extract schema information
        return ParseAnalyticalMetadata(raw_content);
        
            } catch (const std::exception& e) {
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to fetch detailed analytical schema from " + metadata_url + ": " + std::string(e.what()));
        return "Detailed schema fetch error: " + std::string(e.what());
    }
}

duckdb::Value DatasphereDescribeBindData::ParseAnalyticalMetadata(const std::string& xml_content) {
    try {
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Parsing analytical metadata XML content");
        
        // Parse XML to extract actual property names and types
        std::vector<AnalyticalSchemaField> measure_info;
        std::vector<AnalyticalSchemaField> dimension_info;
        std::vector<AnalyticalSchemaField> variable_info;
        
        // Extract measures - look for Property elements with various measure indicators
        size_t pos = 0;
        while ((pos = xml_content.find("<Property", pos)) != std::string::npos) {
            // Look for aggregation-related annotations in the next few lines
            size_t end_pos = xml_content.find(">", pos);
            if (end_pos != std::string::npos) {
                std::string property_section = xml_content.substr(pos, end_pos - pos + 200);
                
                // Extract property name and type
                size_t name_start = property_section.find("Name=\"");
                size_t name_end = property_section.find("\"", name_start + 6);
                std::string property_name = "";
                if (name_start != std::string::npos && name_end != std::string::npos) {
                    property_name = property_section.substr(name_start + 6, name_end - name_start - 6);
                }
                
                // Extract property type
                size_t type_start = property_section.find("Type=\"");
                size_t type_end = property_section.find("\"", type_start + 6);
                std::string property_type = "";
                if (type_start != std::string::npos && type_end != std::string::npos) {
                    property_type = property_section.substr(type_start + 6, type_end - type_start - 6);
                }
                
                // Check for various measure indicators
                bool is_measure = false;
                if (property_section.find("Aggregation") != std::string::npos ||
                    property_section.find("Measure") != std::string::npos ||
                    property_section.find("aggregation") != std::string::npos ||
                    property_section.find("Analytics.Measure") != std::string::npos ||
                    property_section.find("Analytics.MeasureAttribute") != std::string::npos ||
                    property_section.find("Org.OData.Aggregation.V1") != std::string::npos) {
                    is_measure = true;
                }
                
                // Also check for specific property names that we know are measures
                if (property_name.find("Count") != std::string::npos ||
                    property_name.find("revenue") != std::string::npos ||
                    property_name.find("Revenue") != std::string::npos) {
                    is_measure = true;
                }
                
                if (is_measure) {
                    // Store measure name, converted type, and original EDM type
                    std::string duckdb_type = ConvertEdmTypeToDuckDbType(property_type);
                    measure_info.push_back({property_name, duckdb_type, property_type});
                    ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Found measure property: " + property_name + " -> " + duckdb_type + " (EDM: " + property_type + ")");
            } else {
                    // Store dimension name, converted type, and original EDM type
                    std::string duckdb_type = ConvertEdmTypeToDuckDbType(property_type);
                    dimension_info.push_back({property_name, duckdb_type, property_type});
                }
            }
            pos = end_pos + 1;
        }
        
        // Count variables (look for Parameter elements)
        pos = 0;
        while ((pos = xml_content.find("<Parameter", pos)) != std::string::npos) {
            size_t end_pos = xml_content.find(">", pos);
            if (end_pos != std::string::npos) {
                std::string parameter_section = xml_content.substr(pos, end_pos - pos + 200);
                
                // Extract parameter name and type
                size_t name_start = parameter_section.find("Name=\"");
                size_t name_end = parameter_section.find("\"", name_start + 6);
                std::string parameter_name = "";
                if (name_start != std::string::npos && name_end != std::string::npos) {
                    parameter_name = parameter_section.substr(name_start + 6, name_end - name_start - 6);
                }
                
                size_t type_start = parameter_section.find("Type=\"");
                size_t type_end = parameter_section.find("\"", type_start + 6);
                std::string parameter_type = "";
                if (type_start != std::string::npos && type_end != std::string::npos) {
                    parameter_type = parameter_section.substr(type_start + 6, type_end - type_start - 6);
                }
                
                std::string duckdb_type = ConvertEdmTypeToDuckDbType(parameter_type);
                variable_info.push_back({parameter_name, duckdb_type, parameter_type});
            }
            pos = end_pos + 1;
        }
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Parsed schema - Measures: " + std::to_string(measure_info.size()) + 
                        ", Dimensions: " + std::to_string(dimension_info.size()) + 
                        ", Variables: " + std::to_string(variable_info.size()));
        
        return MakeAnalyticalSchemaValue(measure_info, dimension_info, variable_info);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to parse analytical metadata: " + std::string(e.what()));
        return MakeEmptyAnalyticalSchemaValue();
    }
}

std::string DatasphereDescribeBindData::ConvertEdmTypeToDuckDbType(const std::string& edm_type) {
    // Use the centralized conversion method from erpl_odata_edm.hpp
    return DuckTypeConverter::ConvertEdmTypeStringToDuckDbTypeString(edm_type);
}

std::optional<duckdb::Value> DatasphereDescribeBindData::ParseDwaasAnalyticalSchema(const std::string &json_content) {
    try {
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Entering ParseDwaasAnalyticalSchema with content length: " + std::to_string(json_content.length()));
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Parsing DWAAS analytical schema from JSON content");
        
        JsonDocHandle doc(json_content);
        if (!doc) {
            ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Failed to parse JSON content");
            return std::nullopt;
        }
        
        auto *root = doc.Root();
        if (!root) {
            return std::nullopt;
        }
        
        duckdb::vector<duckdb::Value> measures;
        duckdb::vector<duckdb::Value> dimensions;
        duckdb::vector<duckdb::Value> variables;
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Looking for 'definitions' in JSON root");
        auto *elements_obj = FirstDefinitionElements(root);
        if (elements_obj) {
            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Found 'elements' object, starting to iterate through fields");
            duckdb_yyjson::yyjson_obj_iter elements_iter;
            duckdb_yyjson::yyjson_obj_iter_init(elements_obj, &elements_iter);
            duckdb_yyjson::yyjson_val *field_key;

            while ((field_key = duckdb_yyjson::yyjson_obj_iter_next(&elements_iter))) {
                auto *field_val = duckdb_yyjson::yyjson_obj_iter_get_val(field_key);
                if (!duckdb_yyjson::yyjson_is_obj(field_val)) {
                    continue;
                }

                std::string field_name = duckdb_yyjson::yyjson_get_str(field_key);
                std::string field_text = JsonStringOrDefault(field_val, "@EndUserText.label", field_name);
                std::string edm_type = "Edm.String";

                if (LooksLikeDwaasMeasure(field_name)) {
                    measures.push_back(MakeAnalyticalSchemaFieldValue({field_text, "FactSourceMeasure", edm_type}));
                } else {
                    dimensions.push_back(MakeAnalyticalSchemaFieldValue({field_text, "FactSourceAttribute", edm_type}));
                }
            }
        }
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Successfully parsed analytical schema: " + std::to_string(measures.size()) + " measures, " + std::to_string(dimensions.size()) + " dimensions, " + std::to_string(variables.size()) + " variables");
        
        return MakeAnalyticalSchemaValue(measures, dimensions, variables);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Failed to parse DWAAS analytical schema: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<duckdb::Value> DatasphereDescribeBindData::ParseDwaasRelationalSchema(const std::string &json_content) {
    try {
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Entering ParseDwaasRelationalSchema with content length: " + std::to_string(json_content.length()));
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Parsing DWAAS relational schema from JSON content");
        
        JsonDocHandle doc(json_content);
        if (!doc) {
            ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Failed to parse JSON content");
            return std::nullopt;
        }
        
        auto *root = doc.Root();
        if (!root) {
            return std::nullopt;
        }
        
        duckdb::vector<duckdb::Value> columns;
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Looking for 'definitions' in JSON root");
        auto *elements_obj = FirstDefinitionElements(root);
        if (elements_obj) {
            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Found 'elements' object, starting to iterate through columns");
            duckdb_yyjson::yyjson_obj_iter elements_iter;
            duckdb_yyjson::yyjson_obj_iter_init(elements_obj, &elements_iter);
            duckdb_yyjson::yyjson_val *column_key;

            while ((column_key = duckdb_yyjson::yyjson_obj_iter_next(&elements_iter))) {
                auto *column_val = duckdb_yyjson::yyjson_obj_iter_get_val(column_key);
                if (!duckdb_yyjson::yyjson_is_obj(column_val)) {
                    continue;
                }

                std::string column_name = duckdb_yyjson::yyjson_get_str(column_key);
                RelationalSchemaColumn column {
                    JsonStringOrDefault(column_val, "@EndUserText.label", column_name),
                    column_name,
                    JsonStringOrDefault(column_val, "type", "Unknown"),
                    JsonIntStringOrDefault(column_val, "length", "")
                };
                columns.push_back(MakeRelationalSchemaColumnValue(column));
            }
        }
        
        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Successfully parsed relational schema: " + std::to_string(columns.size()) + " columns");
        
        return MakeRelationalSchemaValue(columns);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Failed to parse DWAAS relational schema: " + std::string(e.what()));
        return std::nullopt;
    }
}

void erpl_web::DatasphereDescribeBindData::LoadResourceDetails(duckdb::ClientContext &context) {
    try {
        // Get OAuth2 token using centralized function
        erpl_web::OAuth2Config config = erpl_web::GetDatasphereOAuth2Config();
        std::string access_token = erpl_web::GetOrRefreshDatasphereToken(context, config, secret_name);
        
        // Create HTTP client
        auto http_client = std::make_shared<erpl_web::HttpClient>();
        
        // Create auth params with OAuth2 token
        auto auth_params = std::make_shared<erpl_web::HttpAuthParams>();
        auth_params->bearer_token = access_token;
    
        // Construct resource URL based on type
        std::string resource_url;
        if (resource_type == "space") {
            resource_url = DatasphereUrlBuilder::BuildSpaceFilteredUrl(config.tenant_name, config.data_center, resource_id);
                } else if (resource_type == "asset") {
            // For assets, we need to determine the object type and use the appropriate DWAAS core API endpoint
            // Since the catalog API might not have all assets, let's try the DWAAS core API first based on common object types
            
            std::string dwass_url;
            std::string object_type = "unknown";
            bool dwass_success = false;
            
            // Try common object types in order of likelihood
            std::vector<std::pair<std::string, std::string>> object_types = {
                {"localtables", "LocalTable"},
                {"remotetables", "RemoteTable"}, 
                {"views", "View"},
                {"analyticmodels", "Analytic Model (Cube)"},
                {"factmodels", "Analytic Model (Cube)"},
                {"ermodels", "ERModel"}
            };
            
            for (const auto& [endpoint, type_name] : object_types) {
                dwass_url = DatasphereUrlBuilder::BuildDwaasCoreObjectUrl(config.tenant_name, config.data_center, space_id, endpoint, resource_id);
                object_type = endpoint;
                
                ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Trying " + endpoint + " endpoint: " + dwass_url);
                
                // Try to get detailed content from DWAAS core API
                HttpRequest dwass_req(HttpMethod::GET, HttpUrl(dwass_url));
                dwass_req.AuthHeadersFromParams(*auth_params);
                dwass_req.headers["Accept"] = "application/vnd.sap.datasphere.object.content+json";
                
                auto dwass_resp = http_client->SendRequest(dwass_req);
                if (dwass_resp && dwass_resp->Code() == 200) {
                    ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Successfully got detailed content from DWAAS core API: " + endpoint);
                    
                    // Store the response content for later parsing
                    dwass_response_content = dwass_resp->Content();
                    dwass_endpoint_type = endpoint;
                    
                    dwass_success = true;
                    break;
                } else {
                    ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "DWAAS core API failed for " + endpoint + ": HTTP " + std::to_string(dwass_resp ? dwass_resp->Code() : 0));
                }
            }
            
            if (dwass_success) {
                // We found the asset in DWAAS core API, create basic asset info with all 15 fields
                std::vector<duckdb::Value> asset_row;
                asset_row.push_back(duckdb::Value(resource_id)); // name (0)
                asset_row.push_back(duckdb::Value(space_id)); // space_name (1)
                asset_row.push_back(duckdb::Value(resource_id)); // label (2) - use name as label for now
                asset_row.push_back(duckdb::Value("")); // assetRelationalMetadataUrl (3)
                asset_row.push_back(duckdb::Value("")); // assetRelationalDataUrl (4)
                asset_row.push_back(duckdb::Value("")); // assetAnalyticalMetadataUrl (5)
                asset_row.push_back(duckdb::Value("")); // assetAnalyticalDataUrl (6)
                asset_row.push_back(duckdb::Value("false")); // supports_analytical_queries (7)
                
                // Extended fields (8-14) - initialize with proper types
                asset_row.push_back(duckdb::Value("")); // odataContext (8)
                
                asset_row.push_back(MakeEmptyRelationalSchemaValue()); // relationalSchema (9)
                asset_row.push_back(MakeEmptyAnalyticalSchemaValue()); // analyticalSchema (10)
                
                asset_row.push_back(duckdb::Value("false")); // has_relational_access (11)
                asset_row.push_back(duckdb::Value("false")); // has_analytical_access (12)
                
                // Determine asset type based on the endpoint that succeeded
                std::string asset_type = "Unknown";
                if (object_type == "analyticmodels" || object_type == "factmodels") {
                    asset_type = "Analytical";
                    asset_row[7] = duckdb::Value("true"); // supports_analytical_queries
                    asset_row[12] = duckdb::Value("true"); // has_analytical_access
                } else if (object_type == "localtables" || object_type == "remotetables" || object_type == "views") {
                    asset_type = "Relational";
                    asset_row[11] = duckdb::Value("true"); // has_relational_access
                }
                asset_row.push_back(duckdb::Value(asset_type)); // asset_type (13)
                
                asset_row.push_back(duckdb::Value("")); // odataMetadataEtag (14)
                
                resource_data = {asset_row};
                ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Created basic asset info from DWAAS core API with all 15 fields");
                
                // Parse the DWAAS core API response to populate the appropriate schema field based on asset type
                try {
                    // Get the DWAAS response content that we stored earlier
                    if (!dwass_response_content.empty()) {
                        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Parsing DWAAS response content to extract schema");
                        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "DWAAS response content length: " + std::to_string(dwass_response_content.length()));
                        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "DWAAS response content (first 2000 chars): " + dwass_response_content.substr(0, 2000));
                        
                        if (object_type == "analyticmodels" || object_type == "factmodels") {
                            // For analytical models, parse and populate analyticalSchema (field 10)
                            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Analytical model detected, parsing analytical schema");
                            auto parsed_schema = ParseDwaasAnalyticalSchema(dwass_response_content);
                            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "ParseDwaasAnalyticalSchema returned, checking result");
                            
                            if (parsed_schema) {
                                asset_row[10] = *parsed_schema;
                                resource_data[0][10] = *parsed_schema;
                                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Successfully parsed and populated analyticalSchema");
                            } else {
                                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to parse analytical schema from DWAAS response");
                            }
                        } else if (object_type == "localtables" || object_type == "remotetables" || object_type == "views") {
                            // For relational objects, parse and populate relationalSchema (field 9)
                            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Relational object detected, parsing relational schema");
                            auto parsed_schema = ParseDwaasRelationalSchema(dwass_response_content);
                            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "ParseDwaasRelationalSchema returned, checking result");
                            
                            if (parsed_schema) {
                                asset_row[9] = *parsed_schema;
                                resource_data[0][9] = *parsed_schema;
                                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Successfully parsed and populated relationalSchema");
                            } else {
                                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to parse relational schema from DWAAS response");
                            }
                        } else {
                            // For unknown types, try analytical schema as fallback
                            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Unknown object type, trying analytical schema as fallback");
                            auto parsed_schema = ParseDwaasAnalyticalSchema(dwass_response_content);
                            ERPL_TRACE_INFO("DATASPHERE_CATALOG", "ParseDwaasAnalyticalSchema returned, checking result");
                            
                            if (parsed_schema) {
                                asset_row[10] = *parsed_schema;
                                resource_data[0][10] = *parsed_schema;
                                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Successfully parsed and populated analyticalSchema as fallback");
                            } else {
                                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to parse schema from DWAAS response");
                            }
                        }
                    } else {
                        ERPL_TRACE_INFO("DATASPHERE_CATALOG", "No DWAAS response content available for parsing");
                    }
                } catch (const std::exception& e) {
                    ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to parse DWAAS response: " + std::string(e.what()));
                }
                
            } else {
                // DWAAS core API failed, try catalog API as fallback
                ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "DWAAS core API failed for all endpoints, trying catalog API");
                
                std::string catalog_url = DatasphereUrlBuilder::BuildCatalogAssetFilteredUrl(config.tenant_name, config.data_center, space_id, resource_id);
                
                auto catalog_client = std::make_shared<ODataEntitySetClient>(
        http_client, 
                    HttpUrl(catalog_url),
        auth_params
    );
    
                auto catalog_response = catalog_client->Get();
                if (catalog_response) {
                    // Parse the catalog response to get basic asset info
                    std::vector<std::string> catalog_names = {"name", "space_name", "label", "asset_relational_metadata_url", 
                                                            "asset_relational_data_url", "asset_analytical_metadata_url", 
                                                            "asset_analytical_data_url", "supports_analytical_queries"};
                    std::vector<duckdb::LogicalType> catalog_types = {
                        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)
                    };
                    
                    try {
                        auto catalog_data = catalog_response->ToRows(catalog_names, catalog_types);
                        if (!catalog_data.empty() && catalog_data[0].size() >= 8) {
                            // Ensure we have all 15 fields with proper types
                            if (catalog_data[0].size() < 15) {
                                catalog_data[0].resize(15);
                                // Fill extended fields with fallback values
                                for (size_t i = 8; i < 15; ++i) {
                                    if (i == 10) { // analyticalSchema field - needs to be a STRUCT
                                        catalog_data[0][i] = MakeEmptyAnalyticalSchemaValue();
                                    } else {
                                        catalog_data[0][i] = duckdb::Value("Not available");
                                    }
                                }
                            }
                            resource_data = catalog_data;
                            ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Successfully got asset info from catalog API with all 15 fields");
                        } else {
                            ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Catalog API returned empty or invalid data");
                            // Failure is not a row. This used to become {"error", "..."},
                            // which then got resize(15)'d and emitted as an asset whose
                            // name was literally "error" - a wrong answer dressed as data.
                            // See GitHub #244.
                            throw duckdb::IOException(
                                "Datasphere catalog returned no usable data for %s '%s'%s.",
                                resource_type, resource_id,
                                space_id.empty() ? std::string() : (" in space '" + space_id + "'"));
                        }
                    } catch (const std::exception& e) {
                        ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Failed to parse catalog response: " + std::string(e.what()));
                        throw duckdb::IOException(
                            "Could not parse the Datasphere catalog response for %s '%s': %s",
                            resource_type, resource_id, e.what());
                    }
                } else {
                    ERPL_TRACE_ERROR("DATASPHERE_CATALOG", "Failed to get catalog response");
                    throw duckdb::IOException(
                        "No response from the Datasphere catalog for %s '%s'.", resource_type,
                        resource_id);
                }
            }
        }
        
        // If nothing was found, say so. Returning an "error" row here made a missing asset
        // indistinguishable from an asset named "error", and the caller had no way to tell
        // the two apart (GitHub #244).
        if (resource_data.empty()) {
            throw duckdb::InvalidInputException(
                "No Datasphere %s found with ID '%s'%s.", resource_type, resource_id,
                space_id.empty() ? std::string() : (" in space '" + space_id + "'"));
        }
        
        // For assets, populate extended metadata fields with actual data or fallbacks
        // Only fetch extended metadata if we don't already have complete data from DWAAS core API
        if (resource_type == "asset" && !resource_data.empty() && resource_data[0].size() < 15) {
            try {
                ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Fetching extended metadata for asset");
                auto extended_data = FetchAssetExtendedMetadata(context, config, auth_params);
                
                // Ensure we have enough columns for all 15 fields.
                //
                // resize() fills with a default-constructed Value - an untyped NULL - and
                // column 10 is declared as a STRUCT (AnalyticalSchemaType). The catch branch
                // below already filled it with MakeEmptyAnalyticalSchemaValue(); this branch
                // did not, so the two disagreed about the same column. Same seam, guarded
                // once. See GitHub #244.
                if (resource_data[0].size() < 15) {
                    ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Expanding resource data from " + std::to_string(resource_data[0].size()) + " to 15 columns");
                    const auto previous_size = resource_data[0].size();
                    resource_data[0].resize(15);
                    for (size_t i = previous_size; i < 15; ++i) {
                        resource_data[0][i] = (i == 10) ? MakeEmptyAnalyticalSchemaValue()
                                                        : duckdb::Value("Not available");
                    }
                }
                
                // Replace the extended fields with actual data
                for (size_t i = 0; i < extended_data.size() && (i + 8) < resource_data[0].size(); ++i) {
                    resource_data[0][i + 8] = extended_data[i];
                }
                
                ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Updated extended asset data, now have " + std::to_string(resource_data[0].size()) + " columns");
            } catch (const std::exception& e) {
                ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Failed to fetch extended metadata: " + std::string(e.what()));
                // Ensure we have 15 columns even if extended metadata fails
                if (resource_data[0].size() < 15) {
                    resource_data[0].resize(15);
                    // Fill extended fields with fallback values
                    for (size_t i = 8; i < 15; ++i) {
                        if (i == 10) { // analyticalSchema field - needs to be a STRUCT
                            resource_data[0][i] = MakeEmptyAnalyticalSchemaValue();
                        } else {
                            resource_data[0][i] = duckdb::Value("Not available");
                        }
                    }
                }
            }
        }
        
    } catch (const std::exception& e) {
        // Propagate. This catch-all is the root of GitHub #244: it turned EVERY failure -
        // a missing asset, an unreachable tenant, a parse error, and latterly the explicit
        // throws added above - into the row {"error", "<message>"}. That row was then
        // resize(15)'d and emitted, so `SELECT name FROM datasphere_describe_asset(...)`
        // answered with the literal string "error" as the asset's name.
        //
        // A caller could not tell a missing asset from an asset called "error", nor a
        // network outage from an empty tenant. Nothing in the result said anything had gone
        // wrong, which is what makes this a wrong answer rather than an error.
        ERPL_TRACE_ERROR("DATASPHERE_CATALOG",
                         "Failed to load " + resource_type + " details for '" + resource_id +
                             "': " + e.what());
        throw;
    }
}

std::vector<std::string> erpl_web::DatasphereDescribeBindData::GetColumnNames() const {
    if (resource_type == "space") {
        // SpaceEntityV1 schema: only name and label
        return {"name", "label"};
    } else if (resource_type == "asset") {
        // AssetEntityV1 schema: 8 fields + 7 extended metadata fields
        return {"name", "space_name", "label", "asset_relational_metadata_url", "asset_relational_data_url", 
                "asset_analytical_metadata_url", "asset_analytical_data_url", "supports_analytical_queries",
                "odata_context", "relational_schema", "analytical_schema", "has_relational_access", 
                "has_analytical_access", "asset_type"};
    }
    return {};
}

std::vector<duckdb::LogicalType> erpl_web::DatasphereDescribeBindData::GetColumnTypes() const {
    if (resource_type == "space") {
        // SpaceEntityV1 schema: only name and label
        return {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
    } else if (resource_type == "asset") {
        // AssetEntityV1 schema: 8 fields + 7 extended metadata fields
        // Note: analyticalSchema (column 10) is a STRUCT type for better unnesting support
        return {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                AnalyticalSchemaType(),
                duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
    }
    return {};
}



static duckdb::unique_ptr<duckdb::FunctionData> DatasphereDescribeSpaceBind(duckdb::ClientContext &context,
                                                                           duckdb::TableFunctionBindInput &input,
                                                                           duckdb::vector<duckdb::LogicalType> &return_types,
                                                                           duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("datasphere_describe_space");
    // Extract space_id from input
    auto space_id = input.inputs[0].GetValue<std::string>();
    
    // Extract optional secret from named parameters. The default is the secret name the
    // catalog functions have always used; "default" was never right here, and the value was
    // discarded anyway because the parameter was not registered (GitHub #243).
    std::string secret_name = "datasphere";
    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters["secret"].GetValue<std::string>();
    }
    
    // Return types based on actual Datasphere API schema (SpaceEntityV1)
    return_types = {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
    names = {"name", "label"};
    
    // Get OAuth2 token using centralized function
    OAuth2Config config = GetDatasphereOAuth2Config();
    std::string access_token = GetOrRefreshDatasphereToken(context, config, secret_name);
    
    // Create HTTP client and OData client for space endpoint
    auto http_client = std::make_shared<HttpClient>();
    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;
    
    // Construct space URL with filter - SAP Datasphere doesn't support individual resource access
    std::string space_url = DatasphereUrlBuilder::BuildSpaceFilteredUrl(config.tenant_name, config.data_center, space_id);
    
    ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Creating OData client for space endpoint: " + space_url);
    
    auto space_client = std::make_shared<ODataServiceClient>(
        http_client, 
        HttpUrl(space_url),
        auth_params
    );
    
    // Create bind data using the DatasphereDescribeBindData
    auto bind_data = duckdb::make_uniq<DatasphereDescribeBindData>(space_client, "space", space_id);
    // Carried onto the bind data because LoadResourceDetails fetches a token of its own.
    // Without this the field kept its "datasphere" default and `secret :=` was honoured by
    // the bind but ignored by the load - which the swallowing catch-all then hid, so the
    // test asserting the name was threaded passed without the code working (GitHub #244).
    bind_data->secret_name = secret_name;
    
    ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Bound describe space function for space: " + space_id);
    
    return std::move(bind_data);
}

static duckdb::unique_ptr<duckdb::FunctionData> DatasphereDescribeAssetBind(duckdb::ClientContext &context,
                                                                           duckdb::TableFunctionBindInput &input,
                                                                           duckdb::vector<duckdb::LogicalType> &return_types,
                                                                           duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("datasphere_describe_asset");
    // Extract space_id and asset_id from input
    auto space_id = input.inputs[0].GetValue<std::string>();
    auto asset_id = input.inputs[1].GetValue<std::string>();
    
    // Extract optional secret from named parameters. The default is the secret name the
    // catalog functions have always used; "default" was never right here, and the value was
    // discarded anyway because the parameter was not registered (GitHub #243).
    std::string secret_name = "datasphere";
    if (input.named_parameters.find("secret") != input.named_parameters.end()) {
        secret_name = input.named_parameters["secret"].GetValue<std::string>();
    }
    
    // Return types based on actual Datasphere API schema (AssetEntityV1) - all 15 fields
    // Note: analyticalSchema (column 10) is now a STRUCT type for better unnesting support
    return_types = {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), 
                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                    AnalyticalSchemaType(),
                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
                    duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
            names = {"name", "space_name", "label", "asset_relational_metadata_url", "asset_relational_data_url",
                "asset_analytical_metadata_url", "asset_analytical_data_url", "supports_analytical_queries",
                "odata_context", "relational_schema", "analytical_schema", "has_relational_access",
                "has_analytical_access", "asset_type", "odata_metadata_etag"};
    
    // Get OAuth2 token using centralized function
    OAuth2Config config = GetDatasphereOAuth2Config();
    std::string access_token = GetOrRefreshDatasphereToken(context, config, secret_name);
    
    // Create HTTP client and OData client for asset endpoint
    auto http_client = std::make_shared<HttpClient>();
    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;
    
    // Construct asset URL with filter - SAP Datasphere doesn't support individual resource access
    std::string asset_url = DatasphereUrlBuilder::BuildCatalogAssetFilteredUrl(config.tenant_name, config.data_center, space_id, asset_id);
    
    ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Creating OData client for asset endpoint: " + asset_url);
    
    auto asset_client = std::make_shared<ODataServiceClient>(
        http_client, 
        HttpUrl(asset_url),
        auth_params
    );
    
    // Create bind data using the DatasphereDescribeBindData
    auto bind_data = duckdb::make_uniq<DatasphereDescribeBindData>(asset_client, "asset", asset_id);
    bind_data->secret_name = secret_name;
    bind_data->space_id = space_id; // Store space_id for later use
    
    ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Bound describe asset function for asset: " + asset_id + " in space: " + space_id);
    
    return std::move(bind_data);
}



static void DatasphereDescribeSpaceFunction(duckdb::ClientContext &context, 
                                           duckdb::TableFunctionInput &data_p, 
                                           duckdb::DataChunk &output) {
    auto &bind_data = (DatasphereDescribeBindData &)*data_p.bind_data;
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();

    if (output.GetCapacity() == 0) {
        return;
    }
    
    // Check if we've already returned the data
    if (state.finished) {
        output.SetCardinality(0);
        return;
    }
    
    // Bind-time snapshot by design: the details are fetched on the first execution and
    // kept on the bind data, so re-executing a prepared statement replays them. The
    // emit latch above is per-execution. See the contract in graph_json_scan.hpp.
    // Load resource details if not already loaded
    if (bind_data.resource_data.empty()) {
        bind_data.LoadResourceDetails(context);
    }
    
    // Check if we have data to return
    if (bind_data.resource_data.empty()) {
        output.SetCardinality(0);
        return;
    }
    
    // Return the actual data from the OData response
    output.SetCardinality(1);
    
    // Set values based on the actual Datasphere API schema (SpaceEntityV1)
    // The LoadResourceDetails method should have populated resource_data with the correct schema
    // Note: We're now getting a filtered collection, so we need to handle the array response
    if (bind_data.resource_data.size() > 0 && bind_data.resource_data[0].size() >= 2) {
        output.SetValue(0, 0, bind_data.resource_data[0][0]); // name
        output.SetValue(1, 0, bind_data.resource_data[0][1]); // label
    } else {
        // Same as the asset path: a short row means LoadResourceDetails did not do its job,
        // and reporting "Error: No space found" as the space's NAME is a wrong answer rather
        // than an error (GitHub #244).
        throw duckdb::InternalException(
            "Datasphere space row for '%s' has %d columns, expected at least 2. "
            "LoadResourceDetails should have thrown rather than produced a short row.",
            bind_data.resource_id, static_cast<int>(bind_data.resource_data[0].size()));
    }
    
    // Mark that we've returned the data
    state.finished = true;
    
    ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Returned actual space details for: " + bind_data.resource_id);
}

static void DatasphereDescribeAssetFunction(duckdb::ClientContext &context, 
                                           duckdb::TableFunctionInput &data_p, 
                                           duckdb::DataChunk &output) {
    auto &bind_data = (DatasphereDescribeBindData &)*data_p.bind_data;
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();

    if (output.GetCapacity() == 0) {
        return;
    }
    
    // Check if we've already returned the data
    if (state.finished) {
        output.SetCardinality(0);
        return;
    }
    
    // Bind-time snapshot by design: the details are fetched on the first execution and
    // kept on the bind data, so re-executing a prepared statement replays them. The
    // emit latch above is per-execution. See the contract in graph_json_scan.hpp.
    // Load resource details if not already loaded
    if (bind_data.resource_data.empty()) {
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Loading resource details for asset: " + bind_data.resource_id);
        bind_data.LoadResourceDetails(context);
        ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Resource data loaded, size: " + std::to_string(bind_data.resource_data.size()));
        if (!bind_data.resource_data.empty()) {
            ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "First row size: " + std::to_string(bind_data.resource_data[0].size()));
        }
    }
    
    // Check if we have data to return
    if (bind_data.resource_data.empty()) {
        output.SetCardinality(0);
        return;
    }
    
    // Return the actual data from the OData response
    output.SetCardinality(1);
    
    // Set values based on the actual Datasphere API schema (AssetEntityV1) - all 15 fields
    // The LoadResourceDetails method should have populated resource_data with the correct schema
    // Note: We're now getting a filtered collection, so we need to handle the array response
    ERPL_TRACE_DEBUG("DATASPHERE_CATALOG", "Processing asset data, expecting 15 columns, got: " + std::to_string(bind_data.resource_data[0].size()));
    if (bind_data.resource_data.size() > 0 && bind_data.resource_data[0].size() >= 15) {
        // Basic asset fields (8 fields)
        output.SetValue(0, 0, bind_data.resource_data[0][0]); // name
        output.SetValue(1, 0, bind_data.resource_data[0][1]); // space_name
        output.SetValue(2, 0, bind_data.resource_data[0][2]); // label
        output.SetValue(3, 0, bind_data.resource_data[0][3]); // assetRelationalMetadataUrl
        output.SetValue(4, 0, bind_data.resource_data[0][4]); // assetRelationalDataUrl
        output.SetValue(5, 0, bind_data.resource_data[0][5]); // assetAnalyticalMetadataUrl
        output.SetValue(6, 0, bind_data.resource_data[0][6]); // assetAnalyticalDataUrl
        output.SetValue(7, 0, bind_data.resource_data[0][7]); // supportsAnalyticalQueries
        
        // Extended metadata fields (7 fields)
        output.SetValue(8, 0, bind_data.resource_data[0][8]);  // odataContext
        output.SetValue(9, 0, bind_data.resource_data[0][9]);  // relationalSchema
        output.SetValue(10, 0, bind_data.resource_data[0][10]); // analyticalSchema
        output.SetValue(11, 0, bind_data.resource_data[0][11]); // hasRelationalAccess
        output.SetValue(12, 0, bind_data.resource_data[0][12]); // hasAnalyticalAccess
        output.SetValue(13, 0, bind_data.resource_data[0][13]); // assetType
        output.SetValue(14, 0, bind_data.resource_data[0][14]); // odataMetadataEtag
    } else {
        // Unreachable by construction now - LoadResourceDetails throws rather than returning
        // a short row - but kept as a hard failure rather than a shape that writes strings
        // into typed columns.
        //
        // What stood here wrote the same VARCHAR into all 15 columns, INCLUDING column 10,
        // which is declared as a STRUCT (AnalyticalSchemaType). That is a cast error waiting
        // for a caller, and it encoded failure as data besides. See GitHub #244.
        throw duckdb::InternalException(
            "Datasphere asset row for '%s' has %d columns, expected 15. LoadResourceDetails "
            "should have thrown rather than produced a short row.",
            bind_data.resource_id, static_cast<int>(bind_data.resource_data[0].size()));
    }
    
    // Mark that we've returned the data
    state.finished = true;
    
    ERPL_TRACE_INFO("DATASPHERE_CATALOG", "Returned actual asset details for: " + bind_data.resource_id + " in space: " + bind_data.space_id);
}

// Table function creation functions
duckdb::TableFunctionSet CreateDatasphereShowSpacesFunction() {
    duckdb::TableFunctionSet function_set("datasphere_show_spaces");
    
    // Single implementation using DWAAS core API to match CLI behavior exactly
    // Signature: no args → single VARCHAR column 'name'
    auto scan_function = duckdb::TableFunction(
        {},
        // scan
        [](duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p, duckdb::DataChunk &output) {
            auto &state = data_p.global_state->Cast<ScanRowCursorState>();
            auto &bind = data_p.bind_data->Cast<DatasphereSpacesListBindData>();
            idx_t count = 0;
            while (state.current_index < bind.space_names.size() && count < output.GetCapacity()) {
                SetStrCellNN(output.data[0], count, bind.space_names[state.current_index].c_str());
                state.current_index++;
                count++;
            }
            output.SetCardinality(count);
        },
        // bind
        [](duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input, duckdb::vector<duckdb::LogicalType> &return_types, duckdb::vector<std::string> &names) -> duckdb::unique_ptr<duckdb::FunctionData> {
            PostHogTelemetry::Instance().RecordFunctionCall("datasphere_show_spaces");
            return_types = {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
            names = {"name"};

            // Auth via secret. The name is a named parameter now, registered below; these
            // binds previously had no way to accept one at all (GitHub #243).
            std::string secret_name = "datasphere";
            if (input.named_parameters.find("secret") != input.named_parameters.end()) {
                secret_name = input.named_parameters["secret"].GetValue<std::string>();
            }
            OAuth2Config cfg; // populated by helper
            auto token = GetOrRefreshDatasphereToken(context, cfg, secret_name);

            // Build DWAAS spaces URL using centralized builder
            std::string url = DatasphereUrlBuilder::BuildDwaasCoreSpacesUrl(cfg.tenant_name, cfg.data_center);

            auto http = std::make_shared<HttpClient>();
            auto auth = std::make_shared<HttpAuthParams>();
            auth->bearer_token = token;

            // Raw GET
            HttpRequest req(HttpMethod::GET, HttpUrl(url));
            req.AuthHeadersFromParams(*auth);
            auto resp = http->SendRequest(req);
            if (!resp || resp->Code() != 200) {
                throw duckdb::IOException("Failed to fetch spaces from DWAAS core API: HTTP " + std::to_string(resp ? resp->Code() : 0));
            }

            // The CLI returns a JSON array of names; parse quickly using yyjson (already a dep)
            auto doc = std::shared_ptr<duckdb_yyjson::yyjson_doc>(duckdb_yyjson::yyjson_read(resp->Content().c_str(), resp->Content().size(), 0), duckdb_yyjson::yyjson_doc_free);
            if (!doc) {
                throw duckdb::IOException("Failed to parse DWAAS spaces response JSON");
            }
            auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
            if (!duckdb_yyjson::yyjson_is_arr(root)) {
                throw duckdb::IOException("Unexpected DWAAS spaces payload format (expected JSON array)");
            }

            auto bind = duckdb::make_uniq<DatasphereSpacesListBindData>();
            duckdb_yyjson::yyjson_val *val;
            duckdb_yyjson::yyjson_arr_iter iter;
            duckdb_yyjson::yyjson_arr_iter_init(root, &iter);
            while ((val = duckdb_yyjson::yyjson_arr_iter_next(&iter))) {
                if (duckdb_yyjson::yyjson_is_str(val)) {
                    bind->space_names.emplace_back(duckdb_yyjson::yyjson_get_str(val));
                }
            }
            return std::move(bind);
        }
    );
    scan_function.init_global = ScanRowCursorState::Init;
    scan_function.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    function_set.AddFunction(scan_function);

    return function_set;
}

duckdb::TableFunctionSet CreateDatasphereShowAssetsFunction() {
    duckdb::TableFunctionSet function_set("datasphere_show_assets");
    
    // Replace with DWAAS core API listing across multiple object categories
    auto scan_function = duckdb::TableFunction(
        {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)},
        // scan
        [](duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p, duckdb::DataChunk &output) {
            auto &state = data_p.global_state->Cast<ScanRowCursorState>();
            auto &bind = data_p.bind_data->Cast<DatasphereSpaceObjectsBindData>();
            idx_t count = 0;
            while (state.current_index < bind.items.size() && count < output.GetCapacity()) {
                SetStrCellNN(output.data[0], count, bind.items[state.current_index].name.c_str());
                SetStrCellNN(output.data[1], count, bind.items[state.current_index].object_type.c_str());
                SetStrCellNN(output.data[2], count, bind.items[state.current_index].technical_name.c_str());
                state.current_index++;
                count++;
            }
            output.SetCardinality(count);
        },
        // bind
        [](duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input, duckdb::vector<duckdb::LogicalType> &return_types, duckdb::vector<std::string> &names) -> duckdb::unique_ptr<duckdb::FunctionData> {
            PostHogTelemetry::Instance().RecordFunctionCall("datasphere_show_assets");
            return_types = {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
            names = {"name", "object_type", "technical_name"};

            auto space_id = input.inputs[0].GetValue<std::string>();

            std::string secret_name = "datasphere";
            if (input.named_parameters.find("secret") != input.named_parameters.end()) {
                secret_name = input.named_parameters["secret"].GetValue<std::string>();
            }
            OAuth2Config cfg;
            auto token = GetOrRefreshDatasphereToken(context, cfg, secret_name);
            

            auto http = std::make_shared<HttpClient>();
            auto auth = std::make_shared<HttpAuthParams>();
            auth->bearer_token = token;

            auto fetch_list = [&](const std::string &path, const std::string &type, std::vector<DatasphereSpaceObjectItem> &out) {
                const int page_size = 100;
                int skip = 0;
                while (true) {
                    std::string url = DatasphereUrlBuilder::BuildDwaasCoreSpaceObjectsUrl(cfg.tenant_name, cfg.data_center, space_id, path) + "?select=technicalName&top=" + std::to_string(page_size) + "&skip=" + std::to_string(skip);
                    HttpRequest req(HttpMethod::GET, HttpUrl(url));
                    req.AuthHeadersFromParams(*auth);
                    auto resp = http->SendRequest(req);

                    // A failure is NOT the end of the list. See RequireValidListingPage:
                    // these checks used to `break`, the same exit the last page takes, so
                    // an HTTP 500 on page two returned page one and reported success.
                    std::shared_ptr<duckdb_yyjson::yyjson_doc> doc;
                    auto root = RequireValidListingPage(resp.get(), "'" + url + "'", skip, doc);
                    size_t added = 0;
                    duckdb_yyjson::yyjson_val *val; duckdb_yyjson::yyjson_arr_iter it; duckdb_yyjson::yyjson_arr_iter_init(root, &it);
                    while ((val = duckdb_yyjson::yyjson_arr_iter_next(&it))) {
                        if (duckdb_yyjson::yyjson_is_str(val)) {
                            // API returns array of strings (technical names)
                            std::string t = duckdb_yyjson::yyjson_get_str(val);
                            out.push_back({t, t, type});
                            added++;
                        } else if (duckdb_yyjson::yyjson_is_obj(val)) {
                            // API returns array of objects with technicalName
                            auto tn = duckdb_yyjson::yyjson_obj_get(val, "technicalName");
                            if (tn && duckdb_yyjson::yyjson_is_str(tn)) {
                                std::string tech_str = duckdb_yyjson::yyjson_get_str(tn);
                                out.push_back({tech_str, tech_str, type});
                                added++;
                            }
                        }
                    }
                    if (added < static_cast<size_t>(page_size)) {
                        break; // last page
                    }
                    skip += page_size;
                }
            };

            auto bind = duckdb::make_uniq<DatasphereSpaceObjectsBindData>();

            // Track seen technical names to avoid duplicates across sources
            std::unordered_set<std::string> seen_technical_names;

            // Simple mapping function for known technical name to business name mappings
            auto get_business_name = [&](const std::string &technical_name) -> std::string {
                // Known mappings for this space
                if (technical_name == "AM_RL_BQ_MART_DIM_LEAD") {
                    return "MART_DIM_LEAD";
                }
                // For other assets, use the technical name as business name
                return technical_name;
            };

            auto push_item = [&](const std::string &name, const std::string &technical, const std::string &type) {
                std::string tech_key = technical.empty() ? name : technical;
                if (!tech_key.empty() && seen_technical_names.insert(tech_key).second) {
                    std::string business_name = get_business_name(tech_key);
                    bind->items.push_back({business_name, tech_key, type, space_id});
                }
            };

            // DWAAS endpoints
            std::vector<DatasphereSpaceObjectItem> tmp;
            fetch_list("localtables", "LocalTable", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);
            tmp.clear();
            fetch_list("remotetables", "RemoteTable", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);
            tmp.clear();
            fetch_list("views", "View", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);
            tmp.clear();
            // Prefer the user-facing label "Analytic Model (Cube)"
            fetch_list("factmodels", "Analytic Model (Cube)", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);
            tmp.clear();
            fetch_list("analyticmodels", "Analytic Model (Cube)", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);
            tmp.clear();
            fetch_list("analyticalmodels", "Analytic Model (Cube)", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);
            tmp.clear();
            fetch_list("ermodels", "ERModel", tmp);
            for (auto &it : tmp) push_item(it.name, it.technical_name, it.object_type);

            // Also query the Catalog /assets endpoint and merge results
            try {
                std::string odata_url = DatasphereUrlBuilder::BuildCatalogAssetsFilteredUrl(cfg.tenant_name, cfg.data_center, space_id);
                HttpRequest req(HttpMethod::GET, HttpUrl(odata_url));
                req.AuthHeadersFromParams(*auth);
                auto resp = http->SendRequest(req);
                if (resp && resp->Code() == 200) {
                    auto doc = std::shared_ptr<duckdb_yyjson::yyjson_doc>(duckdb_yyjson::yyjson_read(resp->Content().c_str(), resp->Content().size(), 0), duckdb_yyjson::yyjson_doc_free);
                    if (doc) {
                        auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
                        // OData JSON: either an object with "value" array, or a plain array
                        duckdb_yyjson::yyjson_val *arr = nullptr;
                        if (duckdb_yyjson::yyjson_is_obj(root)) {
                            arr = duckdb_yyjson::yyjson_obj_get(root, "value");
                        } else if (duckdb_yyjson::yyjson_is_arr(root)) {
                            arr = root;
                        }
                        if (arr && duckdb_yyjson::yyjson_is_arr(arr)) {
                            duckdb_yyjson::yyjson_arr_iter it; duckdb_yyjson::yyjson_arr_iter_init(arr, &it);
                            duckdb_yyjson::yyjson_val *val;
                            while ((val = duckdb_yyjson::yyjson_arr_iter_next(&it))) {
                                if (!duckdb_yyjson::yyjson_is_obj(val)) continue;
                                auto nm = duckdb_yyjson::yyjson_obj_get(val, "name");
                                auto tn = duckdb_yyjson::yyjson_obj_get(val, "technicalName");
                                auto am = duckdb_yyjson::yyjson_obj_get(val, "assetAnalyticalMetadataUrl");
                                auto rm = duckdb_yyjson::yyjson_obj_get(val, "assetRelationalMetadataUrl");
                                std::string name_str = (nm && duckdb_yyjson::yyjson_is_str(nm)) ? duckdb_yyjson::yyjson_get_str(nm) : std::string("");
                                std::string tech_str = (tn && duckdb_yyjson::yyjson_is_str(tn)) ? duckdb_yyjson::yyjson_get_str(tn) : name_str;
                                bool has_analytical = (am && duckdb_yyjson::yyjson_is_str(am) && duckdb_yyjson::yyjson_get_len(am) > 0);
                                bool has_relational = (rm && duckdb_yyjson::yyjson_is_str(rm) && duckdb_yyjson::yyjson_get_len(rm) > 0);
                                std::string type = has_analytical ? "Analytic Model (Cube)" : (has_relational ? "RelationalAsset" : "Asset");
                                push_item(name_str, tech_str, type);
                            }
                        }
                    }
                }
            } catch (...) {
                // ignore catalog merge failures
            }
            return std::move(bind);
        }
    );
    scan_function.init_global = ScanRowCursorState::Init;
    scan_function.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    function_set.AddFunction(scan_function);

    // Function 2: Show all assets from all accessible spaces (new functionality)
    auto all_spaces_scan_function = duckdb::TableFunction(
        {},
        // scan
        [](duckdb::ClientContext &context, duckdb::TableFunctionInput &data_p, duckdb::DataChunk &output) {
            auto &state = data_p.global_state->Cast<ScanRowCursorState>();
            auto &bind = data_p.bind_data->Cast<DatasphereSpaceObjectsBindData>();
            idx_t count = 0;
            while (state.current_index < bind.items.size() && count < output.GetCapacity()) {
                SetStrCellNN(output.data[0], count, bind.items[state.current_index].name.c_str());
                SetStrCellNN(output.data[1], count, bind.items[state.current_index].object_type.c_str());
                SetStrCellNN(output.data[2], count, bind.items[state.current_index].technical_name.c_str());
                SetStrCellNN(output.data[3], count, bind.items[state.current_index].space_name.c_str());
                state.current_index++;
                count++;
            }
            output.SetCardinality(count);
        },
        // bind
        [](duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input, duckdb::vector<duckdb::LogicalType> &return_types, duckdb::vector<std::string> &names) -> duckdb::unique_ptr<duckdb::FunctionData> {
            return_types = {duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
            names = {"name", "object_type", "technical_name", "space_name"};

            std::string secret_name = "datasphere";
            if (input.named_parameters.find("secret") != input.named_parameters.end()) {
                secret_name = input.named_parameters["secret"].GetValue<std::string>();
            }
            OAuth2Config cfg;
            auto token = GetOrRefreshDatasphereToken(context, cfg, secret_name);

            auto http = std::make_shared<HttpClient>();
            auto auth = std::make_shared<HttpAuthParams>();
            auth->bearer_token = token;

            // First, get all accessible spaces
            std::vector<std::string> spaces;
            try {
                std::string spaces_url = DatasphereUrlBuilder::BuildDwaasCoreSpacesUrl(cfg.tenant_name, cfg.data_center);
                HttpRequest req(HttpMethod::GET, HttpUrl(spaces_url));
                req.AuthHeadersFromParams(*auth);
                auto resp = http->SendRequest(req);
                if (resp && resp->Code() == 200) {
                    auto doc = std::shared_ptr<duckdb_yyjson::yyjson_doc>(duckdb_yyjson::yyjson_read(resp->Content().c_str(), resp->Content().size(), 0), duckdb_yyjson::yyjson_doc_free);
                    if (doc) {
                        auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
                        if (duckdb_yyjson::yyjson_is_arr(root)) {
                            duckdb_yyjson::yyjson_val *val;
                            duckdb_yyjson::yyjson_arr_iter iter;
                            duckdb_yyjson::yyjson_arr_iter_init(root, &iter);
                            while ((val = duckdb_yyjson::yyjson_arr_iter_next(&iter))) {
                                if (duckdb_yyjson::yyjson_is_str(val)) {
                                    spaces.emplace_back(duckdb_yyjson::yyjson_get_str(val));
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception &e) {
                // An I/O or parse failure is not "this tenant has no spaces". Returning an
                // empty bind here made a network outage indistinguishable from an empty
                // tenant, and every asset in every space silently vanished from the result.
                throw duckdb::IOException(
                    "Datasphere space listing failed, so the asset list cannot be built: %s",
                    e.what());
            }

            auto bind = duckdb::make_uniq<DatasphereSpaceObjectsBindData>();

            // Keyed on (space, technical name, type), not the technical name alone.
            //
            // Keying on the name alone deduplicated ACROSS spaces: two spaces each holding
            // a CUSTOMER table yielded one row, and the other space's asset was dropped
            // without a word. Shared technical names across spaces are the normal case in
            // Datasphere, and the row itself carries the space - so the key that decides
            // whether a row is a duplicate has to carry it too. See GitHub #243.
            std::set<std::tuple<std::string, std::string, std::string>> seen_assets;

            // Simple mapping function for known technical name to business name mappings
            auto get_business_name = [&](const std::string &technical_name) -> std::string {
                // Known mappings for this space
                if (technical_name == "AM_RL_BQ_MART_DIM_LEAD") {
                    return "MART_DIM_LEAD";
                }
                // For other assets, use the technical name as business name
                return technical_name;
            };

            auto push_item = [&](const std::string &name, const std::string &technical, const std::string &type, const std::string &space) {
                std::string tech_key = technical.empty() ? name : technical;
                if (tech_key.empty()) {
                    return;
                }
                if (seen_assets.insert(std::make_tuple(space, tech_key, type)).second) {
                    std::string business_name = get_business_name(tech_key);
                    bind->items.push_back({business_name, tech_key, type, space});
                }
            };

            // For each space, fetch all asset types
            for (const auto& space_id : spaces) {
                auto fetch_list = [&](const std::string &path, const std::string &type) {
                    const int page_size = 100;
                    int skip = 0;
                    while (true) {
                        std::string url = DatasphereUrlBuilder::BuildDwaasCoreSpaceObjectsUrl(cfg.tenant_name, cfg.data_center, space_id, path) + "?select=technicalName&top=" + std::to_string(page_size) + "&skip=" + std::to_string(skip);
                        HttpRequest req(HttpMethod::GET, HttpUrl(url));
                        req.AuthHeadersFromParams(*auth);
                        auto resp = http->SendRequest(req);
                        // Same rule, through the same seam, so the two cannot drift.
                        std::shared_ptr<duckdb_yyjson::yyjson_doc> doc;
                        auto root = RequireValidListingPage(
                            resp.get(), "'" + url + "' in space '" + space_id + "'", skip, doc);
                        size_t added = 0;
                        duckdb_yyjson::yyjson_val *val; duckdb_yyjson::yyjson_arr_iter it; duckdb_yyjson::yyjson_arr_iter_init(root, &it);
                        while ((val = duckdb_yyjson::yyjson_arr_iter_next(&it))) {
                            if (duckdb_yyjson::yyjson_is_str(val)) {
                                // API returns array of strings (technical names)
                                std::string t = duckdb_yyjson::yyjson_get_str(val);
                                push_item(t, t, type, space_id);
                                added++;
                            } else if (duckdb_yyjson::yyjson_is_obj(val)) {
                                // API returns array of objects with technicalName
                                auto tn = duckdb_yyjson::yyjson_obj_get(val, "technicalName");
                                if (tn && duckdb_yyjson::yyjson_is_str(tn)) {
                                    std::string tech_str = duckdb_yyjson::yyjson_get_str(tn);
                                    push_item(tech_str, tech_str, type, space_id);
                                    added++;
                                }
                            }
                        }
                        if (added < static_cast<size_t>(page_size)) {
                            break; // last page
                        }
                        skip += page_size;
                    }
                };

                // DWAAS endpoints for this space
                fetch_list("localtables", "LocalTable");
                fetch_list("remotetables", "RemoteTable");
                fetch_list("views", "View");
                fetch_list("factmodels", "Analytic Model (Cube)");
                fetch_list("analyticmodels", "Analytic Model (Cube)");
                fetch_list("analyticalmodels", "Analytic Model (Cube)");
                fetch_list("ermodels", "ERModel");

                // Also query the Catalog /assets endpoint for this space
                try {
                    std::string odata_url = DatasphereUrlBuilder::BuildCatalogAssetsFilteredUrl(cfg.tenant_name, cfg.data_center, space_id);
                    HttpRequest req(HttpMethod::GET, HttpUrl(odata_url));
                    req.AuthHeadersFromParams(*auth);
                    auto resp = http->SendRequest(req);
                    if (resp && resp->Code() == 200) {
                        auto doc = std::shared_ptr<duckdb_yyjson::yyjson_doc>(duckdb_yyjson::yyjson_read(resp->Content().c_str(), resp->Content().size(), 0), duckdb_yyjson::yyjson_doc_free);
                        if (doc) {
                            auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
                            // OData JSON: either an object with "value" array, or a plain array
                            duckdb_yyjson::yyjson_val *arr = nullptr;
                            if (duckdb_yyjson::yyjson_is_obj(root)) {
                                arr = duckdb_yyjson::yyjson_obj_get(root, "value");
                            } else if (duckdb_yyjson::yyjson_is_arr(root)) {
                                arr = root;
                            }
                            if (arr && duckdb_yyjson::yyjson_is_arr(arr)) {
                                duckdb_yyjson::yyjson_arr_iter it; duckdb_yyjson::yyjson_arr_iter_init(arr, &it);
                                duckdb_yyjson::yyjson_val *val;
                                while ((val = duckdb_yyjson::yyjson_arr_iter_next(&it))) {
                                    if (!duckdb_yyjson::yyjson_is_obj(val)) continue;
                                    auto nm = duckdb_yyjson::yyjson_obj_get(val, "name");
                                    auto tn = duckdb_yyjson::yyjson_obj_get(val, "technicalName");
                                    auto am = duckdb_yyjson::yyjson_obj_get(val, "assetAnalyticalMetadataUrl");
                                    auto rm = duckdb_yyjson::yyjson_obj_get(val, "assetRelationalMetadataUrl");
                                    std::string name_str = (nm && duckdb_yyjson::yyjson_is_str(nm)) ? duckdb_yyjson::yyjson_get_str(nm) : std::string("");
                                    std::string tech_str = (tn && duckdb_yyjson::yyjson_is_str(tn)) ? duckdb_yyjson::yyjson_get_str(tn) : name_str;
                                    bool has_analytical = (am && duckdb_yyjson::yyjson_is_str(am) && duckdb_yyjson::yyjson_get_len(am) > 0);
                                    bool has_relational = (rm && duckdb_yyjson::yyjson_is_str(rm) && duckdb_yyjson::yyjson_get_len(rm) > 0);
                                    std::string type = has_analytical ? "Analytic Model (Cube)" : (has_relational ? "RelationalAsset" : "Asset");
                                    push_item(name_str, tech_str, type, space_id);
                                }
                            }
                        }
                    }
                } catch (...) {
                    // ignore catalog merge failures for this space
                }
            }

            return std::move(bind);
        }
    );
    all_spaces_scan_function.init_global = ScanRowCursorState::Init;
    all_spaces_scan_function.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    function_set.AddFunction(all_spaces_scan_function);

    return function_set;
}

duckdb::TableFunctionSet CreateDatasphereDescribeSpaceFunction() {
    duckdb::TableFunctionSet function_set("datasphere_describe_space");
    
    duckdb::TableFunction describe_space({duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)}, DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereDescribeSpaceFunction), DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereDescribeSpaceBind));
    describe_space.init_global = ScanRowCursorState::Init;
    // Registering the parameter is what makes it usable: the binder rejects any named
    // parameter a function does not declare, so `secret := 'x'` failed with "Invalid named
    // parameter" while two binds contained code to read it (GitHub #243).
    describe_space.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    
    function_set.AddFunction(describe_space);
    return function_set;
}

duckdb::TableFunctionSet CreateDatasphereDescribeAssetFunction() {
    duckdb::TableFunctionSet function_set("datasphere_describe_asset");
    
    duckdb::TableFunction describe_asset({duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR), duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)}, 
                                        DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereDescribeAssetFunction), DATAZOO_GUARD(ERPL_ODATA_BANNER, DatasphereDescribeAssetBind));
    describe_asset.init_global = ScanRowCursorState::Init;
    // Registering the parameter is what makes it usable: the binder rejects any named
    // parameter a function does not declare, so `secret := 'x'` failed with "Invalid named
    // parameter" while two binds contained code to read it (GitHub #243).
    describe_asset.named_parameters["secret"] = duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    
    function_set.AddFunction(describe_asset);
    return function_set;
}

} // namespace erpl_web
