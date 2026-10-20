#pragma once

#include "datazoo/oauth2/http_client.hpp"
#include "yyjson.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace erpl_web {

// A URL THIS EXTENSION constructed, as opposed to one that came out of a service response.
//
// GraphClient::Get attaches the caller's credentials unconditionally, which is right for a
// URL we built from the hardcoded Graph base and wrong for anything a service handed us.
// Keeping those apart has been a matter of remembering, and it was forgotten eight times in
// this codebase: @odata.nextLink, statusMonitorResource, resourceLocation, @odata.context,
// the ODP __next and its 202 Location, the ODP __delta link, and the redirect follower.
//
// So the distinction is in the type system now. The constructor is explicit, so a plain
// std::string - which is what every response-derived URL is - will not convert, and
// `Get(monitor_url)` stops compiling. Writing ExtensionBuiltUrl{...} is a deliberate claim
// about where the URL came from, made at the moment it matters.
class ExtensionBuiltUrl {
public:
    explicit ExtensionBuiltUrl(std::string url) : value(std::move(url)) {}
    const std::string &Value() const { return value; }

private:
    std::string value;
};

class GraphClient {
public:
    GraphClient(std::shared_ptr<HttpAuthParams> auth_params, std::string trace_component);

    // Only for a URL this extension built - see ExtensionBuiltUrl. A service-supplied link
    // goes through GetServerSuppliedUrl, which decides on origin and sendability.
    std::string Get(const ExtensionBuiltUrl &url);

    // Same as Get(), but for a URL the SERVICE supplied (an @odata.nextLink). The bearer
    // token is attached only when the link names the same origin as `origin`; anything
    // else is requested without credentials, so a hostile or misconfigured next link
    // cannot collect a tenant access token. Mirrors the OData client (#183) and the ODP
    // orchestrator (#101, #187). Fails CLOSED: an empty origin sends no credentials.
    std::string GetServerSuppliedUrl(const std::string &url, const std::string &origin);

    // Whether a URL the SERVICE supplied may carry this client's credentials: true only
    // when it resolves, against `origin`, to that same origin. Fails closed on an empty
    // origin or a link that cannot be resolved. GetServerSuppliedUrl decides with this, and
    // it is public so a caller that would otherwise repeat a pointless unauthenticated
    // request - polling a foreign status monitor, say - can decide once up front.
    static bool IsServerSuppliedUrlTrusted(const std::string &url, const std::string &origin);
    std::string GetAllPagesMerged(const ExtensionBuiltUrl &url);

    std::string Post(const std::string &url, const std::string &body);
    std::string PostWithHeaders(const std::string &url, const std::string &body,
                                const std::map<std::string, std::string> &extra_headers);

    // A POST whose RESPONSE HEADERS the caller can read.
    //
    // Graph signals a long-running operation with 202 and the status-monitor URL in the
    // Location HEADER - not in the body. PostWithHeaders returns only the body, so a caller
    // using it can never see that a 202 happened, let alone where to poll. Anything that
    // has to follow a Location needs this instead.
    struct PostResult {
        std::string body;
        int status_code = 0;
        std::string location;  // the Location response header, empty when absent
    };
    PostResult PostForResult(const std::string &url, const std::string &body,
                             const std::map<std::string, std::string> &extra_headers);
    void Patch(const std::string &url, const std::string &body);
    void PatchWithHeaders(const std::string &url, const std::string &body,
                          const std::map<std::string, std::string> &extra_headers);
    void Delete(const std::string &url);
    void DeleteWithHeaders(const std::string &url, const std::map<std::string, std::string> &extra_headers);

    static std::string BaseUrl();
#ifdef ERPL_ODATA_TEST_SEAMS
    // C++ loopback tests only. Accepts 127.0.0.1 HTTP URLs; empty restores Graph.
    static void SetLoopbackBaseUrlForTest(const std::string &url);
#endif
    static std::string UrlEncode(const std::string &value, bool preserve_slashes = false);
    static std::string StripLeadingSlash(const std::string &value);
    static std::string EscapeODataStringLiteral(const std::string &value);
    static bool LooksLikeGuid(const std::string &value);
    static std::optional<std::string> ExtractNextLink(const std::string &json_body);

    // Returns the URL path component for a user: "me" when user is empty,
    // "users/{id}" for a GUID, or "users/{encoded}" for a UPN/email (@ -> %40).
    // Use as: BaseUrl() + "/" + ResolveUserSegment(user) + "/events"
    static std::string ResolveUserSegment(const std::string &user);

private:
    std::shared_ptr<HttpAuthParams> auth_params;
    std::shared_ptr<HttpClient> http_client;
    std::string trace_component;
};

std::string GraphJsonGetString(duckdb_yyjson::yyjson_val *obj, const char *key);
bool GraphJsonGetBool(duckdb_yyjson::yyjson_val *obj, const char *key, bool default_value = false);
std::vector<std::string> GraphJsonStringArray(duckdb_yyjson::yyjson_val *arr);
std::optional<std::string> GraphJsonGetRootString(const std::string &json_body,
                                                  const char *key,
                                                  const std::string &error_context);
std::optional<std::string> GraphJsonFindStringInArray(const std::string &json_body,
                                                       const char *array_key,
                                                       const char *match_key,
                                                       const std::string &match_value,
                                                       const char *return_key,
                                                       const std::string &error_context);
std::optional<std::string> GraphJsonFirstStringInArray(const std::string &json_body,
                                                       const char *array_key,
                                                       const char *return_key,
                                                       const std::string &error_context);

} // namespace erpl_web
