#include "duckdb/function/table_function.hpp"

#include "datazoo/oauth2/http_client.hpp"
#include "odata_edm.hpp"
#include "odata_expand_parser.hpp"
#include "odata_read_functions.hpp"
#include "odata_url_helpers.hpp"
#include "yyjson.hpp"

#include <unordered_set>

#include "tracing.hpp"
#include "telemetry.hpp"
#include "erpl_odata_banner.hpp"

// ODataReadBindData construction and the schema it settles during bind:
// factories for the entity-set / service-root / probe-result entry points, the
// JSON column-name helpers those factories use, EDMX metadata resolution and
// schema-order reconciliation, and the configuration accessors (input
// parameters, expand clause, extracted column names) that the SAP, Datasphere,
// Business Central, Dataverse and catalog modules bind against.
//
// Declarations live in src/include/odata_read_functions.hpp.

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

static bool IsODataMetadataField(const std::string &field_name) {
  // OData V2 spells control information with a leading double underscore.
  if (field_name == "__metadata" || field_name == "__deferred") {
    return true;
  }

  // OData V4 advertises a bound action or function as a property whose name is
  // "#namespace.name" - SAP Gateway emits one per bound operation, with an object
  // value. A structural property name is a SimpleIdentifier and can never begin
  // with '#', so these are control information too, not columns.
  //
  // Left unfiltered they became phantom VARCHAR columns that could never hold their
  // object value: a real SAP travel service produced 7 such columns and 654 failed
  // conversions in a single read. See GitHub #147.
  if (!field_name.empty() && field_name.front() == '#') {
    return true;
  }

  // OData V4 (JSON Format, "Control Information") puts control information and
  // annotations in names containing '@' - "@odata.etag", "@odata.id",
  // "@odata.type", "@odata.editLink", and property-scoped forms such as
  // "Price@odata.type". A structural property name is a SimpleIdentifier and can
  // never contain '@', so this is an exact test rather than a heuristic.
  //
  // Treating "@odata.etag" as a column meant it was pushed into $select, which
  // OData rejects outright: "Syntax error: character '@' is not valid at
  // position 0 in '@odata'". Northwind V4 returns an etag on every entity, so
  // the whole service was unreadable. See GitHub #108.
  return field_name.find('@') != std::string::npos;
}

static bool IsODataV2DeferredNavigationProperty(duckdb_yyjson::yyjson_val *val) {
  // In OData V2 JSON, navigation properties appear as {"__deferred": {"uri": "..."}}
  // Skip these — they are not queryable columns.
  if (!val || !duckdb_yyjson::yyjson_is_obj(val)) {
    return false;
  }
  return duckdb_yyjson::yyjson_obj_get(val, "__deferred") != nullptr;
}

static bool AppendColumnNamesFromObject(
    JsonValue *row_obj, std::vector<std::string> &column_names) {
  if (!row_obj || !duckdb_yyjson::yyjson_is_obj(row_obj)) {
    return false;
  }

  const auto initial_size = column_names.size();
  duckdb_yyjson::yyjson_obj_iter iter;
  duckdb_yyjson::yyjson_obj_iter_init(row_obj, &iter);

  duckdb_yyjson::yyjson_val *key;
  while ((key = duckdb_yyjson::yyjson_obj_iter_next(&iter))) {
    if (!duckdb_yyjson::yyjson_is_str(key)) {
      continue;
    }
    std::string column_name = duckdb_yyjson::yyjson_get_str(key);
    if (IsODataMetadataField(column_name)) {
      continue;
    }
    auto *val = duckdb_yyjson::yyjson_obj_iter_get_val(key);
    if (IsODataV2DeferredNavigationProperty(val)) {
      continue;  // Skip V2 navigation properties (__deferred links)
    }
    column_names.push_back(std::move(column_name));
  }

  return column_names.size() > initial_size;
}

static JsonValue *FirstObjectInArray(JsonValue *array_value) {
  if (!array_value || !duckdb_yyjson::yyjson_is_arr(array_value)) {
    return nullptr;
  }

  auto first_row = duckdb_yyjson::yyjson_arr_get_first(array_value);
  return first_row && duckdb_yyjson::yyjson_is_obj(first_row) ? first_row
                                                              : nullptr;
}

static JsonValue *FirstODataV4Row(JsonValue *root) {
  return FirstObjectInArray(duckdb_yyjson::yyjson_obj_get(root, "value"));
}

static JsonValue *FirstODataV2Row(JsonValue *root) {
  auto data_obj = duckdb_yyjson::yyjson_obj_get(root, "d");
  if (!data_obj || !duckdb_yyjson::yyjson_is_obj(data_obj)) {
    return nullptr;
  }
  return FirstObjectInArray(duckdb_yyjson::yyjson_obj_get(data_obj, "results"));
}

static bool AppendFirstODataRowColumnNames(
    JsonValue *root, std::vector<std::string> &column_names) {
  if (AppendColumnNamesFromObject(FirstODataV4Row(root), column_names)) {
    return true;
  }
  if (AppendColumnNamesFromObject(FirstODataV2Row(root), column_names)) {
    return true;
  }

  // Some callers already pass the first row object rather than the response
  // envelope. Accept that shape to keep extraction local and metadata-free.
  return AppendColumnNamesFromObject(root, column_names);
}

} // namespace

void ODataReadBindData::SetNestedExpandPaths(
    const std::vector<std::string> &nested_paths) {
  if (data_extractor) {
    data_extractor->SetNestedExpandPaths(nested_paths);
  }
}

// ============================================================================
// Helper Functions
// ============================================================================

// Helper function to extract column names from a JSON object
std::vector<std::string>
ExtractColumnNamesFromJson(const std::string &json_content) {
  std::vector<std::string> column_names;

  JsonDocHandle doc(duckdb_yyjson::yyjson_read(json_content.c_str(),
                                               json_content.size(), 0));
  if (!doc) {
    return column_names;
  }

  auto root = doc.Root();
  if (!root) {
    return column_names;
  }

  if (!AppendFirstODataRowColumnNames(root, column_names)) {
    ERPL_TRACE_WARN("ODATA_READ_BIND",
                    "Could not find a row object for column-name extraction");
  }

  return column_names;
}

// ============================================================================
// ODataReadBindData Implementation
// ============================================================================

ODataReadBindData::ODataReadBindData(
    std::shared_ptr<ODataEntitySetClient> odata_client)
    : odata_client(odata_client) {
    InitializeComponents();
}

ODataReadBindData::ODataReadBindData(
    std::shared_ptr<ODataEntitySetClient> odata_client,
    bool defer_initialization)
    : odata_client(odata_client) {
  // Don't call InitializeComponents() - let the factory method do it with the
  // correct mode
}

void ODataReadBindData::InitializeComponents(bool service_root_mode) {
    predicate_pushdown_helper = nullptr;

  // The failure log is created first and owned here: page responses and the
  // row buffer are replaced during pagination, the log must not be.
  conversion_failure_log = std::make_shared<ConversionFailureLog>();

  // Only create entity-set specific components if not in service root mode
  if (!service_root_mode) {
    data_extractor = std::make_shared<ODataDataExtractor>(odata_client);
    data_extractor->SetConversionFailureLog(conversion_failure_log);
    type_resolver = std::make_shared<ODataTypeResolver>(odata_client);
  } else {
    // In service root mode, these components are not needed and would trigger
    // metadata fetching
    data_extractor = nullptr;
    type_resolver = nullptr;
    ERPL_TRACE_INFO("ODATA_READ_BIND",
                    "Skipping data_extractor and type_resolver creation in "
                    "service root mode");
  }

    progress_tracker = std::make_shared<ODataProgressTracker>();
    row_buffer = std::make_shared<ODataRowBuffer>();
}

ODataReadBindData::~ODataReadBindData() {
  // Safety net: a query with a LIMIT may be torn down before the scan reaches
  // its terminal call, and the user should still learn about bad values.
  try {
    ReportConversionFailures();
  } catch (...) {
    // Never let a destructor throw.
  }
}

void ODataReadBindData::SetStrictTyping(bool strict) {
  strict_typing_ = strict;
  if (!conversion_failure_log) {
    conversion_failure_log = std::make_shared<ConversionFailureLog>();
  }
  conversion_failure_log->SetStrictTyping(strict);
  ERPL_TRACE_INFO("ODATA_READ_BIND",
                  std::string("strict_typing set to ") +
                      (strict ? "true" : "false"));
}

std::shared_ptr<ConversionFailureLog>
ODataReadBindData::GetConversionFailureLog() const {
  return conversion_failure_log;
}

void ODataReadBindData::ReportConversionFailures(const std::string &source) {
  if (conversion_failures_reported_ || !conversion_failure_log) {
    return;
  }
  conversion_failures_reported_ = true;
  conversion_failure_log->ReportAndDrain(source);
}

// Helper methods for URL detection
bool ODataReadBindData::IsDatasphereUrl(const std::string &entity_set_url) {
    return entity_set_url.find("datasphere") != std::string::npos || 
           entity_set_url.find("hcs.cloud.sap") != std::string::npos;
}

bool ODataReadBindData::IsODataV2Url(const std::string &entity_set_url) {
    return entity_set_url.find("/V2/") != std::string::npos;
}

bool ODataReadBindData::ShouldUseDirectHttp(const std::string &entity_set_url) {
    return IsDatasphereUrl(entity_set_url) || IsODataV2Url(entity_set_url);
}

duckdb::unique_ptr<ODataReadBindData> ODataReadBindData::FromEntitySetRoot(
    const std::string &entity_set_url,
    std::shared_ptr<HttpAuthParams> auth_params) {
  // Create HTTP client with URL encoding disabled for OData (V2 & V4)
  HttpParams http_params;
  http_params.url_encode = false;
  auto http_client = std::make_shared<HttpClient>(http_params);

  // Ensure $format=json is present on the entity set URL; encode $filter via
  // ODataUrlCodec
  HttpUrl es_url(entity_set_url);
  ODataUrlCodec::ensureJsonFormat(es_url);
  auto odata_client =
      std::make_shared<ODataEntitySetClient>(http_client, es_url, auth_params);
    
    // Determine OData version and approach
    bool is_odata_v2_url = IsODataV2Url(entity_set_url);
    bool use_direct_http = ShouldUseDirectHttp(entity_set_url);
    
    // Set OData version directly for OData v2 to skip metadata fetching
    if (is_odata_v2_url) {
        odata_client->SetODataVersionDirectly(ODataVersion::V2);
    ERPL_TRACE_DEBUG(
        "ODATA_READ_BIND",
        "Set OData version to V2 directly to skip metadata fetching");
    }
    
    std::vector<std::string> extracted_column_names;
    
    if (use_direct_http) {
        // Make direct HTTP request to get first page for column extraction
        if (IsDatasphereUrl(entity_set_url)) {
      ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                       "Detected Datasphere URL, making direct HTTP request to "
                       "get first data page for @odata.context");
        } else {
      ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                       "Detected OData v2 URL, making direct HTTP request to "
                       "extract column names");
        }
        
        try {
      HttpRequest req(HttpMethod::GET, HttpUrl(es_url));
            req.AuthHeadersFromParams(*auth_params);
            req.headers["Accept"] = "application/json";
            auto resp = http_client->SendRequest(req);
            
            if (resp && resp->Code() == 200) {
        ERPL_TRACE_DEBUG(
            "ODATA_READ_BIND",
            "Successfully fetched first data page via direct HTTP");
                
                // Parse the response to extract @odata.context and column names
                auto content = resp->Content();
        ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Parsing JSON response of " +
                                                std::to_string(content.size()) +
                                                " bytes");
        JsonDocHandle doc(
            duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0));
                
                if (doc) {
          ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                           "Successfully parsed JSON document");
                    auto root = doc.Root();
                    ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Got JSON root object");
                    
                    // Check for @odata.context (OData v4)
          auto context_val =
              duckdb_yyjson::yyjson_obj_get(root, "@odata.context");
                    if (context_val && duckdb_yyjson::yyjson_is_str(context_val)) {
                        auto context_url = duckdb_yyjson::yyjson_get_str(context_val);
            ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Extracted @odata.context: " +
                                                    std::string(context_url));
                        
            // Store the metadata context URL in the OData client for future
            // metadata requests
                        std::string clean_context_url = context_url;
                        auto hash_pos = clean_context_url.find('#');
                        if (hash_pos != std::string::npos) {
                            clean_context_url = clean_context_url.substr(0, hash_pos);
                        }
                        
                        if (IsDatasphereUrl(entity_set_url)) {
                            odata_client->SetMetadataContextUrl(clean_context_url);
              ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                               "Stored metadata context URL in OData client: " +
                                   clean_context_url);
                            odata_client->SetEntitySetNameFromContextFragment(context_url);
                        }
                        
                        // Parse OData V4 response format
                        ParseODataV4Response(root, odata_client, extracted_column_names);
                    } else {
                        // Parse OData V2 response format
                        ParseODataV2Response(root, odata_client, extracted_column_names);
                    }
                }
            } else {
        ERPL_TRACE_WARN("ODATA_READ_BIND",
                        "Direct HTTP request failed with status: " +
                            std::to_string(resp ? resp->Code() : 0));
      }
    } catch (const std::exception &e) {
      ERPL_TRACE_WARN("ODATA_READ_BIND",
                      "Failed to fetch first data page via direct HTTP: " +
                          std::string(e.what()));
            // Continue without first page - will fall back to conventional metadata
        }
    } else {
    ERPL_TRACE_DEBUG(
        "ODATA_READ_BIND",
        "Standard OData v4 URL detected, using conventional metadata approach");
    }
    
    auto bind_data = duckdb::make_uniq<ODataReadBindData>(odata_client);
    
    // If we successfully extracted column names, store them for later use
    if (!extracted_column_names.empty()) {
        bind_data->SetExtractedColumnNames(extracted_column_names);
    }
    
    return bind_data;
}

bool ODataReadBindData::LooksLikeServiceRootUrl(const std::string &url) {
  // Strict V2 service root detection: ends with .svc or .svc/
  auto qpos = url.find('?');
  std::string path_only = (qpos == std::string::npos) ? url : url.substr(0, qpos);
  if (path_only.rfind("/$metadata") != std::string::npos) {
    return false;
  }
  auto pos = path_only.rfind(".svc");
  if (pos == std::string::npos) {
    return false;
  }
  // Ensure there are no further path segments after .svc except optional '/'
  size_t after = pos + 4; // length of ".svc"
  if (after == path_only.size()) {
    return true;
  }
  if (after + 1 == path_only.size() && path_only[after] == '/') {
    return true;
  }
  return false;
}

duckdb::unique_ptr<ODataReadBindData> ODataReadBindData::FromServiceRoot(
    const std::string &service_root_url,
    std::shared_ptr<HttpAuthParams> auth_params) {
  // Build minimal schema: name, kind, url (absolute)
  // Reuse ODataServiceClient to fetch and parse the service document, then
  // project rows locally.
  HttpParams http_params;
  http_params.url_encode = false;
  auto http_client = std::make_shared<HttpClient>(http_params);

  HttpUrl svc_url(service_root_url);
  // Ensure JSON format for V2 roots
  ODataUrlCodec::ensureJsonFormat(svc_url);

  auto svc_client =
      std::make_shared<ODataServiceClient>(http_client, svc_url, auth_params);
  // Force V2 headers/parsing for classic service roots when URL indicates V2
  try {
    std::string url_str = service_root_url;
    if (url_str.find("/V2/") != std::string::npos) {
      svc_client->SetODataVersionDirectly(ODataVersion::V2);
      ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                       "FromServiceRoot: forced OData version V2 based on URL");
    }
  } catch (...) {
  }
  // Try to detect/fetch
  auto response = svc_client->Get();

  // Build an entity-set client wrapper so we can reuse existing bind flow but
  // override names/types via extracted list We'll populate
  // extracted_column_names with our unified schema
  auto bind_data = duckdb::make_uniq<ODataReadBindData>(
      std::make_shared<ODataEntitySetClient>(http_client, svc_url,
                                             auth_params));

  // Unified schema (name, kind, url)
  std::vector<std::string> names = {"name", "kind", "url"};
  bind_data->SetExtractedColumnNames(names);

  // Convert service entries to synthetic rows and buffer them
  auto entries = response->EntitySets();
  for (auto &ref : entries) {
    ref.MergeWithBaseUrlIfRelative(svc_url);
  }

  // Build a display base URL without query parameters and with trailing slash
  std::string display_base = service_root_url;
  auto qpos_display = display_base.find('?');
  if (qpos_display != std::string::npos) {
    display_base = display_base.substr(0, qpos_display);
  }
  if (!display_base.empty() && display_base.back() != '/') {
    display_base.push_back('/');
  }

  // Bootstrap row buffer and push rows
  auto rows = std::vector<std::vector<duckdb::Value>>();
  rows.reserve(entries.size());
  for (const auto &ref : entries) {
    // For service root listing, expose absolute URL by extending the base path
    rows.push_back({duckdb::Value(ref.name), duckdb::Value("EntitySet"),
                    duckdb::Value(display_base + ref.name)});
  }

  // Initialize internal components for buffering
  bind_data->InitializeComponents();
  bind_data->EnableServiceRootMode();
  bind_data->row_buffer->AddRows(std::move(rows));
  bind_data->row_buffer->SetHasNextPage(false);
  bind_data->first_page_cached_ = true;

  return bind_data;
}

// Factory pattern methods
duckdb::unique_ptr<ODataReadBindData> ODataReadBindData::FromProbeResult(
    const ODataClientFactory::ProbeResult &result) {
  ERPL_TRACE_INFO(
      "ODATA_READ_BIND",
      std::string("Creating bind data from probe result - is_service_root: ") +
          (result.is_service_root ? "true" : "false"));

  if (result.is_service_root) {
    ERPL_TRACE_INFO("ODATA_READ_BIND", "Taking service root path");
    auto client = ODataClientFactory::CreateServiceClient(result);
    return FromServiceClient(client, result.initial_content);
  } else {
    ERPL_TRACE_INFO("ODATA_READ_BIND", "Taking entity set path");
    auto client = ODataClientFactory::CreateEntitySetClient(result);
    return FromEntitySetClient(client, result.initial_content);
  }
}

duckdb::unique_ptr<ODataReadBindData> ODataReadBindData::FromEntitySetClient(
    std::shared_ptr<ODataEntitySetClient> client,
    const std::string &initial_content) {
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   "Creating bind data from entity set client");

  auto bind_data = duckdb::make_uniq<ODataReadBindData>(client, true);

  // Initialize components for entity set mode (allows metadata fetching)
  bind_data->InitializeComponents(false);

  // If we have initial content, we can pre-populate the first page
  if (!initial_content.empty()) {
    // Parse the initial content to extract column names and types
    try {
      JsonDocHandle doc(duckdb_yyjson::yyjson_read(
          initial_content.c_str(), initial_content.length(), 0));
      if (doc) {
        auto root = doc.Root();
        if (root) {
          std::vector<std::string> extracted_column_names;

          // Determine OData version and parse accordingly
          if (client->GetODataVersion() == ODataVersion::V4) {
            ParseODataV4Response(root, client, extracted_column_names);
          } else {
            ParseODataV2Response(root, client, extracted_column_names);
          }

          bind_data->SetExtractedColumnNames(extracted_column_names);
        }
      }
    } catch (...) {
      // If parsing fails, we'll fall back to normal metadata-based approach
      ERPL_TRACE_DEBUG(
          "ODATA_READ_BIND",
          "Failed to parse initial content, will use metadata approach");
    }

    // Buffer the rows from initial_content so the next FetchNextResult does
    // not issue a redundant bare GET. Without this, callers that hand a
    // pre-fetched page in (e.g. the ODP streaming path) trigger
    // PrefetchFirstPage → odata_client->Get() with no query string, which
    // SAP ODP answers with the entire dataset, inflating results N×.
    try {
      auto synthetic = std::make_shared<ODataEntitySetResponse>(
          std::make_unique<HttpResponse>(HttpMethod::GET, HttpUrl(client->Url()),
                                         200, "application/json", initial_content),
          client->GetODataVersion());
      // Kept as data; CloneForScan adopts it into the private client it mints for each
      // execution, so the page's next link is reachable without two executions sharing a
      // pagination cursor (GitHub #149 and #75).
      bind_data->first_page_response_ = synthetic;
      bind_data->BufferFirstPageFromResponse(synthetic);
    } catch (const std::exception &e) {
      ERPL_TRACE_DEBUG(
          "ODATA_READ_BIND",
          std::string("Failed to buffer initial content; "
                      "PrefetchFirstPage will refetch: ") +
              e.what());
    }
  }

  return bind_data;
}

duckdb::unique_ptr<ODataReadBindData>
ODataReadBindData::FromServiceClient(std::shared_ptr<ODataServiceClient> client,
                                     const std::string &initial_content) {
  ERPL_TRACE_INFO("ODATA_READ_BIND", "Creating bind data from service client");

  // Create a minimal stub entity set client for compatibility with existing
  // bind data structure This client will never be used for actual HTTP requests
  // in service root mode
  HttpParams http_params;
  http_params.url_encode = false;
  auto http_client = std::make_shared<HttpClient>(http_params);

  // Use a dummy URL to avoid any metadata resolution attempts
  auto stub_client = std::make_shared<ODataEntitySetClient>(
      http_client, HttpUrl("http://localhost/stub"), nullptr);
  stub_client->SetODataVersionDirectly(client->GetODataVersion());

  auto bind_data = duckdb::make_uniq<ODataReadBindData>(stub_client, true);

  // Always enable service root mode for service clients
  bind_data->EnableServiceRootMode();

  // Set unified schema (name, kind, url) BEFORE initializing components
  // This ensures GetResultNames() never falls back to calling the client
  std::vector<std::string> names = {"name", "kind", "url"};
  bind_data->SetExtractedColumnNames(names);

  // Initialize components with service root mode (this prevents metadata
  // fetching)
  bind_data->InitializeComponents(true);
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   "FromServiceClient: service root mode enabled");

  // If we have initial content, parse it via ODataServiceResponse (managed
  // yyjson lifecycle)
  if (!initial_content.empty()) {
    try {
      auto temp_response = std::make_shared<ODataServiceResponse>(
          std::make_unique<HttpResponse>(HttpMethod::GET, HttpUrl(""), 200,
                                         "application/json", initial_content),
          client->GetODataVersion());

      auto entries = temp_response->EntitySets();

      // Convert service entries to synthetic rows and buffer them with absolute URLs
      auto rows = std::vector<std::vector<duckdb::Value>>();
      rows.reserve(entries.size());

      // Derive display base from the service client URL
      std::string display_base = client->Url();
      auto qpos_display = display_base.find('?');
      if (qpos_display != std::string::npos) {
        display_base = display_base.substr(0, qpos_display);
      }
      if (!display_base.empty() && display_base.back() != '/') {
        display_base.push_back('/');
      }

      for (const auto &ref : entries) {
        std::string kind = "EntitySet"; // could be extended later for Singletons, etc.
        rows.push_back({duckdb::Value(ref.name), duckdb::Value(kind),
                        duckdb::Value(display_base + ref.name)});
      }

      // Buffer rows (components already initialized in service root mode)
      bind_data->row_buffer->AddRows(std::move(rows));
      bind_data->row_buffer->SetHasNextPage(false);
      bind_data->first_page_cached_ = true;
    } catch (...) {
      ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                       "Failed to parse service root initial content; "
                       "continuing without prebuffer");
    }
  } else {
    // Fallback: fetch now and buffer entries
    try {
      auto response = client->Get();
      auto entries = response->EntitySets();

      auto rows = std::vector<std::vector<duckdb::Value>>();
      rows.reserve(entries.size());
      for (const auto &ref : entries) {
        std::string kind = "EntitySet";
        rows.push_back({duckdb::Value(ref.name), duckdb::Value(kind),
                        duckdb::Value(ref.name)});
      }

      bind_data->row_buffer->AddRows(std::move(rows));
      bind_data->row_buffer->SetHasNextPage(false);
      bind_data->first_page_cached_ = true;
    } catch (...) {
      ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                       "FromServiceClient fallback fetch failed");
    }
    }
    
    return bind_data;
}

// Helper methods for parsing OData responses
void ODataReadBindData::ParseODataV4Response(
    duckdb_yyjson::yyjson_val *root,
    std::shared_ptr<ODataEntitySetClient> odata_client,
    std::vector<std::string> &extracted_column_names) {
  (void)odata_client;
  ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Parsing OData v4 response format");
  if (AppendColumnNamesFromObject(FirstODataV4Row(root),
                                  extracted_column_names)) {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Extracted column names from OData v4 first row");
    return;
  }

  ERPL_TRACE_WARN("ODATA_READ_BIND",
                  "Could not extract column names from OData v4 response");
}

void ODataReadBindData::ParseODataV2Response(
    duckdb_yyjson::yyjson_val *root,
    std::shared_ptr<ODataEntitySetClient> odata_client,
    std::vector<std::string> &extracted_column_names) {
  (void)odata_client;
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   "No @odata.context found, trying OData v2 format");
  if (AppendColumnNamesFromObject(FirstODataV2Row(root),
                                  extracted_column_names)) {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Extracted column names from OData v2 first row");
    return;
  }

  ERPL_TRACE_WARN("ODATA_READ_BIND",
                  "Could not extract column names from OData v2 response");
}

const std::vector<std::string> &ODataReadBindData::MetadataColumnNames() {
  if (!all_result_names.empty() || service_root_mode_ || !odata_client) {
    return all_result_names;
  }
  try {
    ERPL_TRACE_INFO("ODATA_READ_BIND",
                    "Calling odata_client->GetResultNames() for metadata");
    all_result_names = odata_client->GetResultNames();
  } catch (const std::exception &e) {
    ERPL_TRACE_WARN("ODATA_READ_BIND",
                    std::string("Metadata column names unavailable: ") + e.what());
  }
  return all_result_names;
}

const std::vector<duckdb::LogicalType> &ODataReadBindData::MetadataColumnTypes() {
  if (!all_result_types.empty() || service_root_mode_ || !odata_client) {
    return all_result_types;
  }
  try {
    ERPL_TRACE_INFO("ODATA_READ_BIND",
                    "Calling odata_client->GetResultTypes() for metadata");
    all_result_types = odata_client->GetResultTypes();
  } catch (const std::exception &e) {
    ERPL_TRACE_WARN("ODATA_READ_BIND",
                    std::string("Metadata column types unavailable: ") + e.what());
  }
  return all_result_types;
}

std::vector<std::string> ODataReadBindData::ReconcileSchemaOrder(
    const std::vector<std::string> &metadata_names,
    const std::vector<std::string> &json_names) {
  // Metadata (EDMX) order wins: the DuckDB catalog entry for an ATTACHed
  // service declares its columns from EDMX, and DuckDB indexes the scan's
  // column_ids against that declaration. Deriving the scan schema from JSON
  // key order instead put values under the wrong headers (GitHub #88).
  if (metadata_names.empty()) {
    return json_names;
  }
  if (json_names.empty()) {
    return metadata_names;
  }

  std::vector<std::string> reconciled = metadata_names;
  std::unordered_set<std::string> known(metadata_names.begin(),
                                        metadata_names.end());
  for (const auto &json_name : json_names) {
    if (known.insert(json_name).second) {
      // Present in the payload but not declared in EDMX. Keep it, but only
      // after every declared column so the metadata prefix stays aligned with
      // the catalog.
      reconciled.push_back(json_name);
    }
  }
  return reconciled;
}

void ODataReadBindData::EnsureBaseSchemaResolved() {
  if (base_schema_resolved_) {
    return;
  }

  if (service_root_mode_) {
    base_result_names = extracted_column_names.empty()
                            ? std::vector<std::string>{"name", "kind", "url"}
                            : extracted_column_names;
    base_result_types.assign(
        base_result_names.size(),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    base_schema_resolved_ = true;
    return;
  }

  const auto metadata_names = MetadataColumnNames();
  const auto metadata_types = MetadataColumnTypes();

  base_result_names = ReconcileSchemaOrder(metadata_names, extracted_column_names);

  base_result_types.clear();
  base_result_types.reserve(base_result_names.size());
  for (const auto &column_name : base_result_names) {
    auto it = std::find(metadata_names.begin(), metadata_names.end(), column_name);
    if (it != metadata_names.end()) {
      const auto metadata_index =
          static_cast<size_t>(std::distance(metadata_names.begin(), it));
      if (metadata_index < metadata_types.size()) {
        base_result_types.push_back(metadata_types[metadata_index]);
        continue;
      }
    }
    ERPL_TRACE_WARN("ODATA_READ_BIND",
                    "Column '" + column_name +
                        "' has no declared metadata type, using VARCHAR");
    base_result_types.push_back(
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
  }

  ERPL_TRACE_INFO("ODATA_READ_BIND",
                  duckdb::StringUtil::Format(
                      "Resolved base schema: %d columns (%d from metadata)",
                      (int)base_result_names.size(), (int)metadata_names.size()));

  base_schema_resolved_ = true;
}

std::vector<std::string> ODataReadBindData::GetResultNames(bool all_columns) {
  EnsureBaseSchemaResolved();

  std::vector<std::string> base_names = base_result_names;

  // Add expanded data columns if we have any, avoiding duplicates
  if (HasExpandedData()) {
    for (const auto &expand_name : data_extractor->GetExpandedDataSchema()) {
      if (std::find(base_names.begin(), base_names.end(), expand_name) ==
          base_names.end()) {
        ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                         "Adding expanded column: " + expand_name);
        base_names.push_back(expand_name);
      }
    }
  }

  if (all_columns || active_column_ids.empty()) {
    return base_names;
  }

  std::vector<std::string> active_result_names;
  for (auto &column_id : active_column_ids) {
    if (duckdb::IsRowIdColumnId(column_id)) {
      continue;
    }
    if (column_id < base_names.size()) {
      active_result_names.push_back(base_names[column_id]);
    }
  }

  return active_result_names;
}

std::vector<duckdb::LogicalType>
ODataReadBindData::GetResultTypes(bool all_columns) {
  EnsureBaseSchemaResolved();

  std::vector<duckdb::LogicalType> combined_types = base_result_types;
  const auto &base_names_local = base_result_names;

  // Add/merge expanded data types if we have any, keeping them aligned with the
  // names GetResultNames() produces.
  if (HasExpandedData()) {
    const auto &exp_schema = data_extractor->GetExpandedDataSchema();
    const auto &exp_types = data_extractor->GetExpandedDataTypes();
    for (size_t i = 0; i < exp_schema.size(); ++i) {
      const auto &exp_name = exp_schema[i];
      duckdb::LogicalType exp_type =
          (i < exp_types.size())
              ? exp_types[i]
              : duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);

      auto it =
          std::find(base_names_local.begin(), base_names_local.end(), exp_name);
      if (it != base_names_local.end()) {
        const auto idx =
            static_cast<size_t>(std::distance(base_names_local.begin(), it));
        if (idx < combined_types.size()) {
          combined_types[idx] = exp_type;
        }
      } else {
        combined_types.push_back(exp_type);
      }
    }
  }

  if (all_columns || active_column_ids.empty()) {
    return combined_types;
  }

  std::vector<duckdb::LogicalType> active_result_types;
  for (auto &column_id : active_column_ids) {
    if (duckdb::IsRowIdColumnId(column_id)) {
      continue;
    }
    if (column_id < combined_types.size()) {
      active_result_types.push_back(combined_types[column_id]);
    }
  }

  return active_result_types;
}

void ODataReadBindData::SetExtractedColumnNames(
    const std::vector<std::string> &column_names) {
    extracted_column_names = column_names;
    base_schema_resolved_ = false;
  ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Stored " +
                                          std::to_string(column_names.size()) +
                                          " extracted column names");
}

void ODataReadBindData::SetInputParameters(
    const std::map<std::string, std::string> &input_params) {
    input_parameters = input_params;
  ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Stored " +
                                          std::to_string(input_params.size()) +
                                          " input parameters");
}

const std::map<std::string, std::string> &
ODataReadBindData::GetInputParameters() const {
    return input_parameters;
}

std::shared_ptr<ODataEntitySetClient>
ODataReadBindData::GetODataClient() const {
    return odata_client;
}

void ODataReadBindData::SetExpandClause(const std::string &expand_clause) {
    this->expand_clause = expand_clause;
}

std::string ODataReadBindData::GetExpandClause() const { return expand_clause; }

void ODataReadBindData::SetExpandedDataSchema(
    const std::vector<std::string> &expand_paths) {
  if (data_extractor) {
    data_extractor->SetExpandedDataSchema(expand_paths);
    ERPL_TRACE_INFO("ODATA_READ_BIND", "Set expanded data schema with " +
                                           std::to_string(expand_paths.size()) +
                                           " navigation properties");
  } else {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Skipping SetExpandedDataSchema in service root mode");
  }
}

// duplicate definition removed; implementation is at top of file to ensure
// availability before use

bool ODataReadBindData::HasExpandedData() const {
  return data_extractor && data_extractor->HasExpandedData();
}

void ODataReadBindData::UpdateExpandedColumnType(
    const std::string &expand_path, const duckdb::LogicalType &new_type) {
  if (!HasExpandedData() || !data_extractor) {
    return;
  }

  const auto &exp_schema = data_extractor->GetExpandedDataSchema();
  for (size_t i = 0; i < exp_schema.size(); ++i) {
    if (exp_schema[i] != expand_path) {
      continue;
    }
    // GetResultTypes() merges the extractor's expanded types over the base
    // schema by column name on every call, so the extractor is the single
    // place this type needs to live.
    data_extractor->UpdateExpandedColumnType(i, new_type);
    return;
  }
}

} // namespace erpl_web
