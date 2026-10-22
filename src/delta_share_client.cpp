#include "delta_share_client.hpp"
#include "odata_url_helpers.hpp"
#include <set>
#include "tracing.hpp"
#include "yyjson.hpp"
#include <fstream>
#include <sstream>
#include <chrono>
#include "duckdb/common/file_system.hpp"

using namespace duckdb_yyjson;

namespace erpl_web {

// =====================================================================
// DeltaShareProfile Implementation
// =====================================================================

DeltaShareProfile DeltaShareProfile::FromFile(ClientContext& context, const string& profile_path) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Loading profile from: " + profile_path);

    try {
        // Get filesystem instance from DuckDB
        auto &fs = FileSystem::GetFileSystem(context);

        // Detect if this is a remote file
        bool is_remote = FileSystem::IsRemoteFile(profile_path);
        if (is_remote) {
            ERPL_TRACE_DEBUG("DELTA_SHARE", "Detected remote profile URL");
        }

        // Set file flags - use direct I/O for remote files
        // Note: Use FileOpenFlags with idx_t constants (avoided duplicate symbol issues seen with DuckDB 1.4.3)
        FileOpenFlags flags(FileOpenFlags::FILE_FLAGS_READ);
        if (is_remote) {
            flags |= FileOpenFlags(FileOpenFlags::FILE_FLAGS_DIRECT_IO);
        }

        // Open file (works for local, HTTP, S3, etc. via DuckDB's FileSystem API)
        auto handle = fs.OpenFile(profile_path, flags);

        // Get file size
        auto file_size = fs.GetFileSize(*handle);
        if (file_size <= 0) {
            throw std::runtime_error("Invalid profile file size for: " + profile_path);
        }

        // Allocate buffer and read contents
        std::vector<char> buffer(static_cast<size_t>(file_size) + 1);
        handle->Read(buffer.data(), static_cast<idx_t>(file_size));
        buffer[file_size] = '\0';  // Null terminate

        // Parse JSON from buffer
        string json_content(buffer.data(), static_cast<size_t>(file_size));
        return FromJson(json_content);

    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("DELTA_SHARE", "Failed to load profile from " + profile_path + ": " + string(e.what()));
        throw;
    }
}

DeltaShareProfile DeltaShareProfile::FromLocalFile(const string& profile_path) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Loading profile from local file: " + profile_path);

    std::ifstream file(profile_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open Delta Sharing profile file: " + profile_path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    string json_content = buffer.str();

    return FromJson(json_content);
}

DeltaShareProfile DeltaShareProfile::FromJson(const string& json_content) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing Delta Sharing profile from JSON");

    auto doc = std::shared_ptr<yyjson_doc>(
        yyjson_read(json_content.c_str(), json_content.size(), 0),
        yyjson_doc_free
    );

    if (!doc) {
        throw std::runtime_error("Failed to parse Delta Sharing profile JSON");
    }

    auto root = yyjson_doc_get_root(doc.get());
    if (!root || !yyjson_is_obj(root)) {
        throw std::runtime_error("Invalid Delta Sharing profile: root must be JSON object");
    }

    DeltaShareProfile profile;

    // Parse version
    auto version_val = yyjson_obj_get(root, "shareCredentialsVersion");
    if (version_val && yyjson_is_uint(version_val)) {
        profile.share_credentials_version = (int32_t)yyjson_get_uint(version_val);
    } else {
        profile.share_credentials_version = 1;
    }

    // Parse endpoint
    auto endpoint_val = yyjson_obj_get(root, "endpoint");
    if (!endpoint_val || !yyjson_is_str(endpoint_val)) {
        throw std::runtime_error("Invalid Delta Sharing profile: missing 'endpoint' field");
    }
    profile.endpoint = yyjson_get_str(endpoint_val);

    // Parse bearer token
    auto token_val = yyjson_obj_get(root, "bearerToken");
    if (!token_val || !yyjson_is_str(token_val)) {
        throw std::runtime_error("Invalid Delta Sharing profile: missing 'bearerToken' field");
    }
    profile.bearer_token = yyjson_get_str(token_val);

    // Parse optional expiration time
    auto expiry_val = yyjson_obj_get(root, "expirationTime");
    if (expiry_val && yyjson_is_str(expiry_val)) {
        profile.expiration_time = yyjson_get_str(expiry_val);
    }

    ERPL_TRACE_INFO("DELTA_SHARE", "Successfully loaded Delta Sharing profile: endpoint=" + profile.endpoint);
    return profile;
}

bool DeltaShareProfile::IsExpired() const {
    if (!expiration_time.has_value()) {
        return false;
    }

    try {
        std::tm tm = {};
        std::istringstream ss(expiration_time.value());
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

        if (ss.fail()) {
            ERPL_TRACE_WARN("DELTA_SHARE", "Failed to parse expiration time: " + expiration_time.value());
            return false;
        }

        auto expiry = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        auto now = std::chrono::system_clock::now();

        return now > expiry;
    } catch (const std::exception& e) {
        ERPL_TRACE_WARN("DELTA_SHARE", "Error checking token expiration: " + string(e.what()));
        return false;
    }
}

string DeltaShareProfile::ToDebugString() const {
    return "DeltaShareProfile(endpoint=" + endpoint + ", version=" + std::to_string(share_credentials_version) + ")";
}

// =====================================================================
// DeltaShareClient Implementation
// =====================================================================

DeltaShareClient::DeltaShareClient(ClientContext& context, const DeltaShareProfile& profile)
    : profile_(profile) {

    ValidateProfile();

    // Create HTTP client with 30-second default timeout
    http_client_ = duckdb::make_shared_ptr<TimeoutHttpClient>(
        std::chrono::milliseconds(30000)
    );

    ERPL_TRACE_INFO("DELTA_SHARE", "Initialized Delta Sharing client: " + profile_.ToDebugString());
}

void DeltaShareClient::ValidateProfile() const {
    if (profile_.endpoint.empty()) {
        throw std::runtime_error("Delta Sharing profile: endpoint cannot be empty");
    }
    if (profile_.bearer_token.empty()) {
        throw std::runtime_error("Delta Sharing profile: bearer token cannot be empty");
    }
    // The bearer token is attached to every request built from this endpoint, so the
    // endpoint gets at least the scrutiny the data-file URLs get. The profile is not
    // necessarily user-authored - DeltaShareProfile::FromFile accepts a remote path - so
    // an endpoint of "http://attacker.example/v1" would have sent the token in cleartext
    // to a host of someone else's choosing.
    //
    // Absolute http(s) FIRST, exactly as the data-file check does and for the same reason:
    // RequireSecureOrLoopbackUrl deliberately RETURNS for input carrying no scheme, because
    // its other callers accept a bare name and resolve it themselves. Calling it alone let
    // "attacker.example/v1" straight through - the same omission this repo had already
    // fixed for file URLs, repeated here.
    if (!LooksLikeAbsoluteHttpUrl(profile_.endpoint)) {
        throw duckdb::InvalidInputException(
            "The Delta Sharing profile 'endpoint' must be an absolute http(s) URL "
            "(https anywhere, or plain http only for a loopback address); got '%s'.",
            profile_.endpoint.c_str());
    }
    RequireSecureOrLoopbackUrl(profile_.endpoint, "The Delta Sharing profile 'endpoint'");
    if (profile_.IsExpired()) {
        throw std::runtime_error("Delta Sharing profile: bearer token has expired");
    }
}

string DeltaShareClient::BuildUrl(const string& path) const {
    string url = profile_.endpoint;
    if (url.back() == '/') {
        url.pop_back();
    }
    return url + path;
}

HeaderMap DeltaShareClient::BuildHeaders() const {
    HeaderMap headers;
    headers["Authorization"] = BuildAuthorizationHeader();
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json";
    return headers;
}

string DeltaShareClient::BuildAuthorizationHeader() const {
    return "Bearer " + profile_.bearer_token;
}

DeltaShareResponse DeltaShareClient::ExecuteGet(const string& endpoint, const HeaderMap& headers) {
    string url = BuildUrl(endpoint);
    ERPL_TRACE_DEBUG("DELTA_SHARE", "GET " + url);

    HttpRequest request(HttpMethod::GET, url);

    // Add default headers
    auto all_headers = BuildHeaders();
    for (auto& pair : headers) {
        all_headers[pair.first] = pair.second;
    }
    request.headers = all_headers;

    try {
        auto response = http_client_->SendRequest(request);

        if (!response) {
            throw std::runtime_error("No response received from server");
        }

        DeltaShareResponse delta_response;
        delta_response.http_status = response->Code();
        delta_response.content = response->Content();
        // Headers available from response object if needed

        ERPL_TRACE_DEBUG("DELTA_SHARE", "Response status: " + std::to_string(delta_response.http_status));

        return delta_response;
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("DELTA_SHARE", "HTTP GET failed: " + string(e.what()));
        throw;
    }
}

DeltaShareResponse DeltaShareClient::ExecutePost(const string& endpoint, const string& body, const HeaderMap& headers) {
    string url = BuildUrl(endpoint);
    ERPL_TRACE_DEBUG("DELTA_SHARE", "POST " + url);
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Request body: " + body);

    HttpRequest request(HttpMethod::POST, url, "application/json", body);

    // Add default headers
    auto all_headers = BuildHeaders();
    for (auto& pair : headers) {
        all_headers[pair.first] = pair.second;
    }
    request.headers = all_headers;

    try {
        auto response = http_client_->SendRequest(request);

        if (!response) {
            throw std::runtime_error("No response received from server");
        }

        DeltaShareResponse delta_response;
        delta_response.http_status = response->Code();
        delta_response.content = response->Content();
        // Headers available from response object if needed

        ERPL_TRACE_DEBUG("DELTA_SHARE", "Response status: " + std::to_string(delta_response.http_status));

        return delta_response;
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("DELTA_SHARE", "HTTP POST failed: " + string(e.what()));
        throw;
    }
}

void DeltaShareClient::HandleApiError(int32_t status_code, const string& error_body) {
    string error_msg = "Delta Sharing API error (HTTP " + std::to_string(status_code) + ")";

    ERPL_TRACE_ERROR("DELTA_SHARE", error_msg);
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Error response: " + error_body);

    throw std::runtime_error(error_msg + ": " + error_body);
}

vector<DeltaShareInfo> DeltaShareClient::ListShares() {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Listing shares from: " + profile_.endpoint);

    DeltaShareResponse response = ExecuteGet("/shares");

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    return ParseSharesResponse(response.content);
}

vector<DeltaSchemaInfo> DeltaShareClient::ListSchemas(const string& share) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Listing schemas for share: " + share);

    string endpoint = "/shares/" + share + "/schemas";
    DeltaShareResponse response = ExecuteGet(endpoint);

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    return ParseSchemasResponse(response.content, share);
}

vector<DeltaTableInfo> DeltaShareClient::ListTables(const string& share, const string& schema) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Listing tables for share: " + share + ", schema: " + schema);

    string endpoint = "/shares/" + share + "/schemas/" + schema + "/tables";
    DeltaShareResponse response = ExecuteGet(endpoint);

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    return ParseTablesResponse(response.content, share, schema);
}

DeltaTableMetadata DeltaShareClient::GetTableMetadata(const string& share, const string& schema, const string& table) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Fetching metadata for: " + share + "." + schema + "." + table);

    string endpoint = "/shares/" + share + "/schemas/" + schema + "/tables/" + table + "/metadata";
    DeltaShareResponse response = ExecuteGet(endpoint);

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    return ParseMetadataResponse(response.content);
}

std::vector<DeltaFileReference> DeltaShareClient::QueryTable(const std::string& share, const std::string& schema, const std::string& table,
                                                       const std::optional<DeltaShareQueryRequest>& query_request) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Querying table: " + share + "." + schema + "." + table);

    string endpoint = "/shares/" + share + "/schemas/" + schema + "/tables/" + table + "/query";
    string body = "{}";  // Empty query

    if (query_request.has_value()) {
        body = query_request->ToJson();
    }

    DeltaShareResponse response = ExecutePost(endpoint, body);

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    return ParseQueryResponse(response.content);
}

int64_t DeltaShareClient::GetTableVersion(const string& share, const string& schema, const string& table) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Getting table version for: " + share + "." + schema + "." + table);

    string endpoint = "/shares/" + share + "/schemas/" + schema + "/tables/" + table + "/version";
    DeltaShareResponse response = ExecuteGet(endpoint);

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    // Parse version from response
    auto doc = std::shared_ptr<yyjson_doc>(
        yyjson_read(response.content.c_str(), response.content.size(), 0),
        yyjson_doc_free
    );

    if (!doc) {
        throw std::runtime_error("Failed to parse version response");
    }

    auto root = yyjson_doc_get_root(doc.get());
    auto version_val = yyjson_obj_get(root, "version");

    if (version_val && yyjson_is_uint(version_val)) {
        return (int64_t)yyjson_get_uint(version_val);
    }

    return 0;
}

std::vector<DeltaFileReference> DeltaShareClient::GetTableChanges(const std::string& share, const std::string& schema, const std::string& table,
                                                            int64_t starting_version, std::optional<int64_t> ending_version) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Getting table changes for: " + share + "." + schema + "." + table);

    string endpoint = "/shares/" + share + "/schemas/" + schema + "/tables/" + table + "/changes";

    // Build request with starting version
    string body = "{\"startingVersion\": " + std::to_string(starting_version);
    if (ending_version.has_value()) {
        body += ", \"endingVersion\": " + std::to_string(ending_version.value());
    }
    body += "}";

    DeltaShareResponse response = ExecutePost(endpoint, body);

    if (response.http_status != 200) {
        HandleApiError(response.http_status, response.content);
    }

    return ParseQueryResponse(response.content);
}

// =====================================================================
// File Reference Parsing (Consolidates duplicate extraction logic)
// =====================================================================

DeltaFileReference DeltaShareClient::ParseFileReference(yyjson_val* file_obj) const {
    DeltaFileReference file_ref;

    // This is the FIRST code that touches the share server's output, so it must assume
    // nothing about it. yyjson_get_str returns NULL for any value that is not a string,
    // and assigning NULL to a std::string is undefined behaviour - a crash inside strlen.
    // A server answering /query with {"url":null} or {"url":123} therefore took the
    // extension down before any of the URL policing further along could run.
    auto url_val = yyjson_obj_get(file_obj, "url");
    if (!url_val || !yyjson_is_str(url_val)) {
        throw duckdb::InvalidInputException(
            "The Delta Sharing server returned a file entry whose 'url' is missing or not a "
            "string.");
    }
    file_ref.url = yyjson_get_str(url_val);

    // Safe as-is: yyjson_get_uint returns 0 for anything that is not a number.
    auto size_val = yyjson_obj_get(file_obj, "size");
    if (size_val) {
        file_ref.size = yyjson_get_uint(size_val);
    }

    auto id_val = yyjson_obj_get(file_obj, "id");
    if (id_val && yyjson_is_str(id_val)) {
        file_ref.id = yyjson_get_str(id_val);
    }

    // Extract partition values if present. The Delta Sharing protocol spells this
    // "partitionValues"; "partition_values" never matched, which is one half of why
    // partition columns came back empty (GitHub, see the scan's partition check).
    auto partition_vals = yyjson_obj_get(file_obj, "partitionValues");
    if (partition_vals && yyjson_is_obj(partition_vals)) {
        yyjson_obj_iter iter = yyjson_obj_iter_with(partition_vals);
        yyjson_val* key;
        yyjson_val* val;
        while ((key = yyjson_obj_iter_next(&iter))) {
            val = yyjson_obj_iter_get_val(key);
            if (yyjson_is_str(key) && yyjson_is_str(val)) {
                file_ref.partition_values[yyjson_get_str(key)] = yyjson_get_str(val);
            }
        }
    }

    // Extract stats if present
    auto stats_val = yyjson_obj_get(file_obj, "stats");
    if (stats_val) {
        char* stats_str = duckdb_yyjson::yyjson_val_write(stats_val, 0, nullptr);
        if (stats_str) {
            try {
                file_ref.stats = string(stats_str);
            } catch (...) {
                free(stats_str);
                throw;
            }
            free(stats_str);
        }
    }

    return file_ref;
}

// =====================================================================
// Response Parsing (Stub implementations - to be enhanced)
// =====================================================================

DeltaTableMetadata DeltaShareClient::ParseMetadataResponse(const string& ndjson_content) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing metadata response");

    DeltaTableMetadata metadata;

    // Parse NDJSON: each line is a separate JSON object
    // First line contains protocol info (shared token, delta protocol version, etc.)
    // Subsequent lines contain metadata with schema

    size_t pos = 0;
    int line_num = 0;

    while (pos < ndjson_content.length()) {
        // Find the end of the current line
        size_t line_end = ndjson_content.find('\n', pos);
        if (line_end == string::npos) {
            line_end = ndjson_content.length();
        }

        // Extract the line
        string line = ndjson_content.substr(pos, line_end - pos);

        // Skip empty lines
        if (!line.empty() && line != "\r") {
            // Remove trailing carriage return if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing NDJSON line " + std::to_string(line_num) + ": " + line.substr(0, 50));

            // Parse the JSON line
            auto doc = std::shared_ptr<yyjson_doc>(
                yyjson_read(line.c_str(), line.length(), 0),
                yyjson_doc_free
            );

            if (doc) {
                auto root = yyjson_doc_get_root(doc.get());

                // Check if this is a protocol line
                auto protocol_val = yyjson_obj_get(root, "protocol");
                if (protocol_val) {
                    // Protocol information line - extract version info if needed
                    ERPL_TRACE_DEBUG("DELTA_SHARE", "Found protocol metadata line");
                }

                // Check if this is a metadata line (Delta Sharing uses camelCase "metaData")
                auto metadata_val = yyjson_obj_get(root, "metaData");
                if (metadata_val) {
                    // Extract schema from metadata (stored as "schemaString" in Delta Sharing Protocol)
                    auto schema_val = yyjson_obj_get(metadata_val, "schemaString");
                    if (schema_val && yyjson_is_str(schema_val)) {
                        // schemaString is already a JSON string, extract it directly
                        metadata.schema_json = string(yyjson_get_str(schema_val));
                        ERPL_TRACE_DEBUG("DELTA_SHARE", "Extracted schema: " + metadata.schema_json.substr(0, 100));
                    }

                    // Extract partition columns if present (also using camelCase)
                    auto partition_cols_val = yyjson_obj_get(metadata_val, "partitionColumns");
                    if (partition_cols_val && yyjson_is_arr(partition_cols_val)) {
                        size_t idx, max;
                        yyjson_val* col_item;
                        yyjson_arr_foreach(partition_cols_val, idx, max, col_item) {
                            if (yyjson_is_str(col_item)) {
                                metadata.partition_columns.push_back(yyjson_get_str(col_item));
                            }
                        }
                    }
                }
            } else {
                ERPL_TRACE_WARN("DELTA_SHARE", "Failed to parse NDJSON line: " + line);
            }

            line_num++;
        }

        pos = line_end + 1;
    }

    ERPL_TRACE_INFO("DELTA_SHARE", "Parsed metadata response, schema length: " + std::to_string(metadata.schema_json.length()));
    return metadata;
}

vector<DeltaFileReference> DeltaShareClient::ParseQueryResponse(const string& ndjson_content) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing query response (NDJSON format)");

    vector<DeltaFileReference> files;

    // Parse NDJSON: each line is a separate JSON object
    // Format of each line in the response:
    // - Protocol line: {"protocol": {...}, "metadata": {...}}
    // - File line: {"file": {...}} or {"add": {...}}
    //   where file/add contains: {"url": "presigned_url", "size": 1024, "id": "file_id", ...}

    size_t pos = 0;
    int line_num = 0;

    while (pos < ndjson_content.length()) {
        // Find the end of the current line
        size_t line_end = ndjson_content.find('\n', pos);
        if (line_end == string::npos) {
            line_end = ndjson_content.length();
        }

        // Extract the line
        string line = ndjson_content.substr(pos, line_end - pos);

        // Skip empty lines
        if (!line.empty() && line != "\r") {
            // Remove trailing carriage return if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing NDJSON line " + std::to_string(line_num) + ": " + line.substr(0, 50));

            // Parse the JSON line
            auto doc = std::shared_ptr<yyjson_doc>(
                yyjson_read(line.c_str(), line.length(), 0),
                yyjson_doc_free
            );

            if (doc) {
                auto root = yyjson_doc_get_root(doc.get());

                // Check if this is a protocol/metadata line (skip for now)
                auto protocol_val = yyjson_obj_get(root, "protocol");
                if (protocol_val) {
                    ERPL_TRACE_DEBUG("DELTA_SHARE", "Found protocol line in query response");
                    line_num++;
                    pos = line_end + 1;
                    continue;
                }

                // Check if this is a file reference line (Delta format: {"file": {...}})
                auto file_val = yyjson_obj_get(root, "file");
                if (file_val && yyjson_is_obj(file_val)) {
                    auto file_ref = ParseFileReference(file_val);
                    files.push_back(file_ref);
                    ERPL_TRACE_DEBUG("DELTA_SHARE", "Extracted file reference: " + file_ref.url.substr(0, 60) + "...");
                }

                // Check if this is an add action line (Delta format: {"add": {...}})
                auto add_val = yyjson_obj_get(root, "add");
                if (add_val && yyjson_is_obj(add_val)) {
                    auto file_ref = ParseFileReference(add_val);
                    files.push_back(file_ref);
                    ERPL_TRACE_DEBUG("DELTA_SHARE", "Extracted add action file reference: " + file_ref.url.substr(0, 60) + "...");
                }
            } else {
                ERPL_TRACE_WARN("DELTA_SHARE", "Failed to parse NDJSON line: " + line);
            }

            line_num++;
        }

        pos = line_end + 1;
    }

    ERPL_TRACE_INFO("DELTA_SHARE", "Parsed query response, found " + std::to_string(files.size()) + " file references");
    return files;
}

vector<DeltaShareInfo> DeltaShareClient::ParseSharesResponse(const string& json_content) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing shares response");

    vector<DeltaShareInfo> shares;

    auto doc = std::shared_ptr<yyjson_doc>(
        yyjson_read(json_content.c_str(), json_content.size(), 0),
        yyjson_doc_free
    );

    if (!doc) {
        ERPL_TRACE_WARN("DELTA_SHARE", "Failed to parse JSON shares response");
        return shares;
    }

    auto root = yyjson_doc_get_root(doc.get());
    auto shares_arr = yyjson_obj_get(root, "shares");

    if (shares_arr && yyjson_is_arr(shares_arr)) {
        size_t idx, max;
        yyjson_val* share_item;
        yyjson_arr_foreach(shares_arr, idx, max, share_item) {
            DeltaShareInfo share_info;

            // A share with no usable name is SKIPPED, not emitted with an empty one. The
            // schema parser in delta_share_scan.cpp already `continue`s past exactly this
            // case, so that is the house rule; surfacing an empty-named row instead would
            // let a malformed listing masquerade as a real share.
            auto name_val = yyjson_obj_get(share_item, "name");
            if (!name_val || !yyjson_is_str(name_val)) {
                ERPL_TRACE_WARN("DELTA_SHARE", "Skipping a share entry with no string 'name'");
                continue;
            }
            share_info.name = yyjson_get_str(name_val);

            auto id_val = yyjson_obj_get(share_item, "id");
            if (id_val && yyjson_is_str(id_val)) {
                share_info.id = yyjson_get_str(id_val);
            }

            shares.push_back(share_info);
        }
    }

    ERPL_TRACE_INFO("DELTA_SHARE", "Parsed " + std::to_string(shares.size()) + " shares");
    return shares;
}

vector<DeltaSchemaInfo> DeltaShareClient::ParseSchemasResponse(const string& json_content, const string& share_name) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing schemas response");

    vector<DeltaSchemaInfo> schemas;

    auto doc = std::shared_ptr<yyjson_doc>(
        yyjson_read(json_content.c_str(), json_content.size(), 0),
        yyjson_doc_free
    );

    if (!doc) {
        ERPL_TRACE_WARN("DELTA_SHARE", "Failed to parse JSON schemas response");
        return schemas;
    }

    auto root = yyjson_doc_get_root(doc.get());
    auto schemas_arr = yyjson_obj_get(root, "schemas");

    if (schemas_arr && yyjson_is_arr(schemas_arr)) {
        size_t idx, max;
        yyjson_val* schema_item;
        yyjson_arr_foreach(schemas_arr, idx, max, schema_item) {
            DeltaSchemaInfo schema_info;
            schema_info.share = share_name;

            auto name_val = yyjson_obj_get(schema_item, "name");
            if (!name_val || !yyjson_is_str(name_val)) {
                ERPL_TRACE_WARN("DELTA_SHARE", "Skipping a schema entry with no string 'name'");
                continue;
            }
            schema_info.name = yyjson_get_str(name_val);

            schemas.push_back(schema_info);
        }
    }

    ERPL_TRACE_INFO("DELTA_SHARE", "Parsed " + std::to_string(schemas.size()) + " schemas");
    return schemas;
}

vector<DeltaTableInfo> DeltaShareClient::ParseTablesResponse(const string& json_content, const string& share_name, const string& schema_name) {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Parsing tables response");

    vector<DeltaTableInfo> tables;

    auto doc = std::shared_ptr<yyjson_doc>(
        yyjson_read(json_content.c_str(), json_content.size(), 0),
        yyjson_doc_free
    );

    if (!doc) {
        ERPL_TRACE_WARN("DELTA_SHARE", "Failed to parse JSON tables response");
        return tables;
    }

    auto root = yyjson_doc_get_root(doc.get());
    auto tables_arr = yyjson_obj_get(root, "tables");

    if (tables_arr && yyjson_is_arr(tables_arr)) {
        size_t idx, max;
        yyjson_val* table_item;
        yyjson_arr_foreach(tables_arr, idx, max, table_item) {
            DeltaTableInfo table_info;
            table_info.share = share_name;
            table_info.schema = schema_name;

            auto name_val = yyjson_obj_get(table_item, "name");
            if (!name_val || !yyjson_is_str(name_val)) {
                ERPL_TRACE_WARN("DELTA_SHARE", "Skipping a table entry with no string 'name'");
                continue;
            }
            table_info.name = yyjson_get_str(name_val);

            auto id_val = yyjson_obj_get(table_item, "id");
            if (id_val && yyjson_is_str(id_val)) {
                table_info.id = yyjson_get_str(id_val);
            }

            auto desc_val = yyjson_obj_get(table_item, "description");
            if (desc_val && yyjson_is_str(desc_val)) {
                table_info.description = yyjson_get_str(desc_val);
            }

            tables.push_back(table_info);
        }
    }

    ERPL_TRACE_INFO("DELTA_SHARE", "Parsed " + std::to_string(tables.size()) + " tables");
    return tables;
}

// =====================================================================
// DeltaShareQueryRequest Implementation
// =====================================================================

string DeltaShareQueryRequest::ToJson() const {
    std::string json = "{";

    // Add predicate hints if present
    if (!predicate_hints.empty()) {
        json += "\"predicate_hints\": [";
        for (size_t i = 0; i < predicate_hints.size(); ++i) {
            json += "\"" + predicate_hints[i] + "\"";
            if (i < predicate_hints.size() - 1) {
                json += ", ";
            }
        }
        json += "]";

        if (!json_predicate_hints.empty() || limit_hint.has_value() || version.has_value()) {
            json += ", ";
        }
    }

    // Add JSON predicate hints if present
    if (!json_predicate_hints.empty()) {
        json += "\"json_predicate_hints\": {";
        size_t i = 0;
        for (auto& pair : json_predicate_hints) {
            json += "\"" + pair.first + "\": \"" + pair.second + "\"";
            if (i < json_predicate_hints.size() - 1) {
                json += ", ";
            }
            ++i;
        }
        json += "}";

        if (limit_hint.has_value() || version.has_value()) {
            json += ", ";
        }
    }

    // Add limit hint if present
    if (limit_hint.has_value()) {
        json += "\"limit_hint\": " + std::to_string(limit_hint.value());

        if (version.has_value()) {
            json += ", ";
        }
    }

    // Add version if present
    if (version.has_value()) {
        json += "\"version\": " + std::to_string(version.value());
    }

    json += "}";
    return json;
}

// =====================================================================
// Utility Functions
// =====================================================================

LogicalType ConvertDeltaTypeToLogicalType(const string& delta_type) {
	// Map Delta types to DuckDB types
	if (delta_type == "string" || delta_type == "String") {
		return LogicalType::VARCHAR;
	} else if (delta_type == "integer" || delta_type == "int") {
		return LogicalType::INTEGER;
	} else if (delta_type == "long") {
		return LogicalType::BIGINT;
	} else if (delta_type == "short") {
		return LogicalType::SMALLINT;
	} else if (delta_type == "byte") {
		return LogicalType::TINYINT;
	} else if (delta_type == "double") {
		return LogicalType::DOUBLE;
	} else if (delta_type == "float") {
		return LogicalType::FLOAT;
	} else if (delta_type == "boolean") {
		return LogicalType::BOOLEAN;
	} else if (delta_type == "date") {
		return LogicalType::DATE;
	} else if (delta_type == "timestamp") {
		return LogicalType::TIMESTAMP;
	} else {
		// Unknown type - default to VARCHAR
		return LogicalType::VARCHAR;
	}
}

bool IsKnownDeltaType(const string& delta_type) {
	// The set ConvertDeltaTypeToLogicalType maps deliberately. Everything else - decimal,
	// binary, and the nested types (struct/array/map, which arrive as JSON objects rather
	// than as a type name at all) - falls through to VARCHAR there. That default used to be
	// harmless because the value was never cast; now that delta_share_scan casts each file
	// onto the bound schema, it would quietly stringify a column instead of failing, so the
	// scan asks this first (GitHub #207).
	static const std::set<string> known = {
		"string", "String", "integer", "int", "long", "short", "byte",
		"double", "float", "boolean", "date", "timestamp"};
	return known.count(delta_type) > 0;
}

} // namespace erpl_web
