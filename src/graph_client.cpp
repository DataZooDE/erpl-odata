#include "graph_client.hpp"
#ifdef ERPL_ODATA_TEST_SEAMS
#include <mutex>
#endif
#include "odata_url_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "tracing.hpp"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace duckdb_yyjson;

namespace erpl_web {

namespace {
static constexpr size_t MAX_GRAPH_PAGES = 10000;

static std::shared_ptr<yyjson_doc> ParseJsonDocument(const std::string &json_body,
                                                     const std::string &error_context) {
    auto doc = std::shared_ptr<yyjson_doc>(yyjson_read(json_body.c_str(), json_body.size(), 0), yyjson_doc_free);
    if (!doc) {
        throw duckdb::IOException("Failed to parse Microsoft Graph JSON response: " + error_context);
    }
    return doc;
}

static void AppendJsonValue(std::string &target, yyjson_val *value) {
    size_t json_len = 0;
    char *json_str = yyjson_val_write(value, 0, &json_len);
    if (!json_str) {
        throw duckdb::IOException("Failed to serialize Microsoft Graph JSON value");
    }
    target.append(json_str, json_len);
    free(json_str);
}

} // namespace

GraphClient::GraphClient(std::shared_ptr<HttpAuthParams> auth_params, std::string trace_component)
    : auth_params(std::move(auth_params)), http_client(CreateODataHttpClient()),
      trace_component(std::move(trace_component)) {
}

namespace {
#ifdef ERPL_ODATA_TEST_SEAMS
std::mutex graph_base_url_mutex;
std::string graph_loopback_base_url;
#endif
}

#ifdef ERPL_ODATA_TEST_SEAMS
void GraphClient::SetLoopbackBaseUrlForTest(const std::string &url) {
    if (!url.empty()) {
        const HttpUrl parsed(url);
        const auto port = parsed.Port();
        if (parsed.Scheme() != "http" || parsed.Host() != "127.0.0.1" || port.empty() ||
            parsed.Path() != "/v1.0" || !parsed.Username().empty() || !parsed.Password().empty() ||
            !parsed.Query().empty() || !parsed.Fragment().empty() || !parsed.IsWireSafeTarget() ||
            std::stoul(port) == 0 || std::stoul(port) > 65535) {
            throw std::invalid_argument("Graph test base URL must be a 127.0.0.1 HTTP endpoint with a port");
        }
    }
    std::lock_guard<std::mutex> guard(graph_base_url_mutex);
    graph_loopback_base_url = url;
}
#endif

std::string GraphClient::BaseUrl() {
#ifdef ERPL_ODATA_TEST_SEAMS
    std::lock_guard<std::mutex> guard(graph_base_url_mutex);
    return graph_loopback_base_url.empty() ? "https://graph.microsoft.com/v1.0" : graph_loopback_base_url;
#else
    return "https://graph.microsoft.com/v1.0";
#endif
}

std::string GraphClient::UrlEncode(const std::string &value, bool preserve_slashes) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved || (preserve_slashes && c == '/')) {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
        }
    }

    return escaped.str();
}

std::string GraphClient::StripLeadingSlash(const std::string &value) {
    if (!value.empty() && value[0] == '/') {
        return value.substr(1);
    }
    return value;
}

std::string GraphClient::EscapeODataStringLiteral(const std::string &value) {
    // Delegates: the rule is OData's, not Graph's, and it lives in one place now.
    return erpl_web::EscapeODataStringLiteral(value);
}

bool GraphClient::LooksLikeGuid(const std::string &value) {
    return value.size() == 36 && value[8] == '-' && value[13] == '-' &&
           value[18] == '-' && value[23] == '-';
}

static void GraphCheckResponse(const std::unique_ptr<HttpResponse> &response,
                                const std::string &trace_component,
                                const std::string &method_label,
                                bool allow_no_content = false)
{
    if (!response) {
        std::string error_msg = "Microsoft Graph " + method_label + " request failed: no response";
        ERPL_TRACE_ERROR(trace_component, error_msg);
        throw duckdb::IOException(error_msg);
    }
    const int code = response->Code();
    if (code < 200 || code >= 300) {
        std::string error_msg = "Microsoft Graph " + method_label + " request failed (HTTP " +
                                std::to_string(code) + ")";
        if (!response->Content().empty()) {
            error_msg += ": " + response->Content().substr(0, 500);
        }
        ERPL_TRACE_ERROR(trace_component, error_msg);
        throw duckdb::IOException(error_msg);
    }
}


bool GraphClient::IsServerSuppliedUrlTrusted(const std::string &url, const std::string &origin) {
    if (origin.empty() || !IsWireSafeUrl(url)) {
        return false;
    }
    try {
        const HttpUrl trusted(origin);
        return trusted.IsSameOrigin(HttpUrl::MergeWithBaseUrlIfRelative(trusted, url));
    } catch (const std::exception &) {
        return false;
    }
}

std::string GraphClient::GetServerSuppliedUrl(const std::string &url, const std::string &origin) {
    ERPL_TRACE_DEBUG(trace_component, "GET (server-supplied) request to: " + url);

    // Resolve the link against the trusted origin first: a RELATIVE next link is
    // same-origin by construction, and comparing it unresolved would drop credentials on a
    // legitimate page. Mirrors OdpRequestOrchestrator::IsSameOrigin.
    //
    // A link that cannot be resolved at all is refused outright rather than requested
    // without credentials: sending an unparseable or unresolvable link to the network is
    // not a safer fallback, it is just a different unknown. The one guarantee is that no
    // path out of here attaches credentials to a URL whose origin was not established.
    // Enforced HERE, not at the Graph call sites: a URL carrying CR/LF or a space is
    // injected into the request line, which httplib writes without validating.
    //
    // Every GRAPH follower of a service-supplied link goes through this function. It is NOT
    // the only such seam in the extension. The others, each carrying its own check:
    // ODataClient::Get (next link), ODataClient::DoHttpGet (choke point),
    // DoMetadataHttpGet (@odata.context, first request and 404 retry),
    // ODataClientFactory::ProbeUrl (bind-time probe),
    // OdpRequestOrchestrator::ExecuteNextPage (__next) and its 202 Location follow, and
    // the Datasphere credential gate.
    //
    // An earlier version of this comment claimed universal coverage and was wrong twice
    // over - first while the OData seams were entirely unguarded, then while it named only
    // some of them. A claim about coverage is worth only as much as the list behind it.
    if (!IsWireSafeUrl(url)) {
        throw duckdb::IOException(
            "Microsoft Graph returned a link containing characters that cannot be sent in a "
            "request: '" + SummariseUrlForMessage(url) + "'.");
    }

    duckdb::unique_ptr<HttpUrl> resolved;
    bool same_origin = false;
    try {
        if (origin.empty()) {
            resolved = duckdb::make_uniq<HttpUrl>(url);
        } else {
            const HttpUrl trusted(origin);
            resolved = duckdb::make_uniq<HttpUrl>(HttpUrl::MergeWithBaseUrlIfRelative(trusted, url));
            same_origin = trusted.IsSameOrigin(*resolved);
        }
    } catch (const std::exception &e) {
        ERPL_TRACE_WARN(trace_component, "Could not resolve server-supplied link '" + url +
                                             "' against origin '" + origin + "': " + e.what());
        throw duckdb::IOException("Microsoft Graph returned a link that could not be resolved "
                                  "against the service origin: " + url);
    }

    HttpUrl &http_url = *resolved;
    if (!same_origin) {
        ERPL_TRACE_WARN(trace_component,
                        "Server-supplied next link points at a different origin than the service (" +
                            origin + " -> " + url + "); requesting it without credentials");
    }

    HttpRequest request(HttpMethod::GET, http_url);
    if (auth_params && same_origin) {
        request.AuthHeadersFromParams(*auth_params);
    }
    request.headers["Accept"] = "application/json";

    auto response = http_client->SendRequest(request);
    GraphCheckResponse(response, trace_component, "GET");
    return response->Content();
}

// Attaches the caller's credentials unconditionally, so the URL must be one THIS extension
// built - never one parsed out of a response body. Anything that arrives in a response
// (an @odata.nextLink, a statusMonitorResource, any other service-supplied link) goes
// through GetServerSuppliedUrl instead, which decides on origin. Three review rounds
// missed src/graph_excel_client.cpp's status-monitor poll because only @odata.nextLink was
// thought of as "server-supplied"; the rule is about where the URL came from, not what it
// is called.
//
// The ExtensionBuiltUrl parameter is what enforces that now. A service-supplied URL is
// always a std::string, and the explicit constructor refuses to convert one, so this
// function cannot be reached by accident - only by a call site that names the type and
// thereby states where its URL came from. test_graph_url_provenance.cpp asserts it.
std::string GraphClient::Get(const ExtensionBuiltUrl &built_url) {
    const std::string &url = built_url.Value();
    ERPL_TRACE_DEBUG(trace_component, "GET request to: " + url);

    HttpUrl http_url(url);
    HttpRequest request(HttpMethod::GET, http_url);
    if (auth_params) {
        request.AuthHeadersFromParams(*auth_params);
    }
    request.headers["Accept"] = "application/json";

    auto response = http_client->SendRequest(request);
    GraphCheckResponse(response, trace_component, "GET");

    ERPL_TRACE_DEBUG(trace_component, "Response received: " + std::to_string(response->Content().length()) + " bytes");
    return response->Content();
}

std::string GraphClient::Post(const std::string &url, const std::string &body) {
    return PostWithHeaders(url, body, {});
}

GraphClient::PostResult GraphClient::PostForResult(
    const std::string &url, const std::string &body,
    const std::map<std::string, std::string> &extra_headers) {
    ERPL_TRACE_DEBUG(trace_component, "POST (result) request to: " + url);

    HttpUrl http_url(url);
    HttpRequest request(HttpMethod::POST, http_url, "application/json", body);
    if (auth_params) {
        request.AuthHeadersFromParams(*auth_params);
    }
    request.headers["Accept"] = "application/json";
    for (const auto &kv : extra_headers) {
        request.headers[kv.first] = kv.second;
    }

    auto response = http_client->SendRequest(request);
    GraphCheckResponse(response, trace_component, "POST");

    PostResult result;
    result.body = response->Content();
    result.status_code = response->Code();
    const auto location = response->headers.find("Location");
    if (location != response->headers.end()) {
        result.location = location->second;
    }
    return result;
}

std::string GraphClient::PostWithHeaders(const std::string &url, const std::string &body,
                                         const std::map<std::string, std::string> &extra_headers)
{
    ERPL_TRACE_DEBUG(trace_component, "POST request to: " + url);

    HttpUrl http_url(url);
    HttpRequest request(HttpMethod::POST, http_url, "application/json", body);
    if (auth_params) {
        request.AuthHeadersFromParams(*auth_params);
    }
    request.headers["Accept"] = "application/json";
    for (const auto &kv : extra_headers) {
        request.headers[kv.first] = kv.second;
    }

    auto response = http_client->SendRequest(request);
    GraphCheckResponse(response, trace_component, "POST");

    ERPL_TRACE_DEBUG(trace_component, "POST response: " + std::to_string(response->Content().length()) + " bytes");
    return response->Content();
}

void GraphClient::Patch(const std::string &url, const std::string &body) {
    PatchWithHeaders(url, body, {});
}

void GraphClient::PatchWithHeaders(const std::string &url, const std::string &body,
                                    const std::map<std::string, std::string> &extra_headers) {
    ERPL_TRACE_DEBUG(trace_component, "PATCH request to: " + url);

    HttpUrl http_url(url);
    HttpRequest request(HttpMethod::PATCH, http_url, "application/json", body);
    if (auth_params) {
        request.AuthHeadersFromParams(*auth_params);
    }
    request.headers["Accept"] = "application/json";
    for (const auto &kv : extra_headers) {
        request.headers[kv.first] = kv.second;
    }

    auto response = http_client->SendRequest(request);
    GraphCheckResponse(response, trace_component, "PATCH");
}

void GraphClient::Delete(const std::string &url) {
    DeleteWithHeaders(url, {});
}

void GraphClient::DeleteWithHeaders(const std::string &url,
                                    const std::map<std::string, std::string> &extra_headers)
{
    ERPL_TRACE_DEBUG(trace_component, "DELETE request to: " + url);

    HttpUrl http_url(url);
    HttpRequest request(HttpMethod::_DELETE, http_url);
    if (auth_params) {
        request.AuthHeadersFromParams(*auth_params);
    }
    for (const auto &kv : extra_headers) {
        request.headers[kv.first] = kv.second;
    }

    auto response = http_client->SendRequest(request);
    GraphCheckResponse(response, trace_component, "DELETE");
}

std::optional<std::string> GraphClient::ExtractNextLink(const std::string &json_body) {
    auto doc = ParseJsonDocument(json_body, "pagination nextLink");
    auto root = yyjson_doc_get_root(doc.get());
    auto next = root ? yyjson_obj_get(root, "@odata.nextLink") : nullptr;
    if (next && yyjson_is_str(next)) {
        return std::string(yyjson_get_str(next));
    }
    return std::nullopt;
}

std::string GraphClient::GetAllPagesMerged(const ExtensionBuiltUrl &built_url) {
    const std::string &url = built_url.Value();
    std::vector<std::string> pages;
    pages.reserve(1);

    // `url` is built by this extension from the hardcoded Graph base, so it is the trusted
    // origin for this scan. Every LATER page comes from an @odata.nextLink in a response
    // body and is fetched through the guarded entry point, which attaches the bearer token
    // only for that origin (GitHub #205, same class as #183 / #101 / #187). This is the
    // eager paging path behind most Graph readers; the lazy path in GraphPagedScanState
    // makes the same decision.
    const std::string trusted_origin = url;

    std::string next_url = url;
    for (size_t page_count = 0; !next_url.empty(); page_count++) {
        if (page_count >= MAX_GRAPH_PAGES) {
            throw duckdb::IOException("Microsoft Graph pagination exceeded safety limit");
        }
        auto body = (page_count == 0) ? Get(built_url)
                                      : GetServerSuppliedUrl(next_url, trusted_origin);
        auto next_link = ExtractNextLink(body);
        pages.push_back(std::move(body));
        next_url = next_link.value_or("");
    }

    if (pages.size() <= 1) {
        return pages.empty() ? "{}" : pages[0];
    }

    std::string merged = "{\"value\":[";
    bool first_item = true;

    for (const auto &page : pages) {
        auto doc = ParseJsonDocument(page, "paged value merge");
        auto root = yyjson_doc_get_root(doc.get());
        auto value_arr = root ? yyjson_obj_get(root, "value") : nullptr;
        if (!value_arr || !yyjson_is_arr(value_arr)) {
            return pages[0];
        }

        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(value_arr, idx, max, item) {
            if (!first_item) {
                merged += ",";
            }
            AppendJsonValue(merged, item);
            first_item = false;
        }
    }

    merged += "]}";
    return merged;
}

std::string GraphClient::ResolveUserSegment(const std::string &user) {
    if (user.empty()) {
        return "me";
    }
    // GUID (8-4-4-4-12, 36 chars): safe for URL paths, no encoding needed.
    if (LooksLikeGuid(user)) {
        return "users/" + user;
    }
    // UPN (user@tenant.com) or email: percent-encode @ -> %40 so the segment
    // is valid in a URL path. Dots and hyphens are RFC 3986 unreserved.
    return "users/" + UrlEncode(user);
}

std::string GraphJsonGetString(yyjson_val *obj, const char *key) {
    auto val = obj ? yyjson_obj_get(obj, key) : nullptr;
    if (val && yyjson_is_str(val)) {
        return yyjson_get_str(val);
    }
    return "";
}

bool GraphJsonGetBool(yyjson_val *obj, const char *key, bool default_value) {
    auto val = obj ? yyjson_obj_get(obj, key) : nullptr;
    if (val && yyjson_is_bool(val)) {
        return yyjson_get_bool(val);
    }
    return default_value;
}

std::vector<std::string> GraphJsonStringArray(yyjson_val *arr) {
    std::vector<std::string> result;
    if (!arr || !yyjson_is_arr(arr)) {
        return result;
    }

    size_t idx, max;
    yyjson_val *val;
    yyjson_arr_foreach(arr, idx, max, val) {
        if (yyjson_is_str(val)) {
            result.emplace_back(yyjson_get_str(val));
        } else if (yyjson_is_null(val)) {
            result.emplace_back("");
        } else if (yyjson_is_num(val)) {
            result.emplace_back(std::to_string(yyjson_get_num(val)));
        } else if (yyjson_is_bool(val)) {
            result.emplace_back(yyjson_get_bool(val) ? "true" : "false");
        } else {
            result.emplace_back("");
        }
    }
    return result;
}

std::optional<std::string> GraphJsonGetRootString(const std::string &json_body,
                                                  const char *key,
                                                  const std::string &error_context) {
    auto doc = ParseJsonDocument(json_body, error_context);
    auto root = yyjson_doc_get_root(doc.get());
    auto val = root ? yyjson_obj_get(root, key) : nullptr;
    if (val && yyjson_is_str(val)) {
        return std::string(yyjson_get_str(val));
    }
    return std::nullopt;
}

std::optional<std::string> GraphJsonFindStringInArray(const std::string &json_body,
                                                       const char *array_key,
                                                       const char *match_key,
                                                       const std::string &match_value,
                                                       const char *return_key,
                                                       const std::string &error_context) {
    auto doc = ParseJsonDocument(json_body, error_context);
    auto root = yyjson_doc_get_root(doc.get());
    auto arr = root ? yyjson_obj_get(root, array_key) : nullptr;
    if (!arr || !yyjson_is_arr(arr)) {
        return std::nullopt;
    }

    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, idx, max, item) {
        auto match_val = yyjson_obj_get(item, match_key);
        auto result_val = yyjson_obj_get(item, return_key);
        if (match_val && yyjson_is_str(match_val) &&
            result_val && yyjson_is_str(result_val) &&
            match_value == yyjson_get_str(match_val)) {
            return std::string(yyjson_get_str(result_val));
        }
    }
    return std::nullopt;
}

std::optional<std::string> GraphJsonFirstStringInArray(const std::string &json_body,
                                                       const char *array_key,
                                                       const char *return_key,
                                                       const std::string &error_context) {
    auto doc = ParseJsonDocument(json_body, error_context);
    auto root = yyjson_doc_get_root(doc.get());
    auto arr = root ? yyjson_obj_get(root, array_key) : nullptr;
    if (!arr || !yyjson_is_arr(arr)) {
        return std::nullopt;
    }

    auto first = yyjson_arr_get_first(arr);
    auto result_val = first ? yyjson_obj_get(first, return_key) : nullptr;
    if (result_val && yyjson_is_str(result_val)) {
        return std::string(yyjson_get_str(result_val));
    }
    return std::nullopt;
}

} // namespace erpl_web
