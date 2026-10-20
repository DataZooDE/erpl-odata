#include "odata_test_server.hpp"
#include <chrono>

// Keep the OpenSSL flavour of the vendored httplib so that this translation unit
// lands in the same duckdb_httplib_openssl namespace as the extension's HTTP
// client. Mixing the two flavours in one binary would be an ODR violation.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace erpl_web {
namespace test_support {

namespace {

constexpr const char *LOOPBACK_HOST = "127.0.0.1";

// Bound how long a lingering keep-alive connection can delay shutdown.
constexpr time_t KEEP_ALIVE_TIMEOUT_SECONDS = 1;
constexpr time_t READ_TIMEOUT_SECONDS = 2;
constexpr time_t IDLE_INTERVAL_MICROSECONDS = 100000;

std::string ToLowerAscii(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto c : value) {
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Decodes a query-string value tolerantly: percent escapes, plus '+' read as a
// space.
//
// Which of the two spellings arrives depends on the request. Requests sent with
// url_encode = false -- every OData path -- now reach the socket byte for byte
// as the extension composed them, spaces already written as %20 by
// ODataUrlCodec::encodeFilterExpression (GitHub #126). Requests sent with the
// default url_encode = true still go through httplib's
// application/x-www-form-urlencoded round trip in ClientImpl::write_request,
// where parse_query_text() decodes with plus_as_space = true and
// params_to_query_str() re-encodes with space_as_plus = true, so a space shows
// up as '+'. Accepting both keeps this read-back usable for either kind of
// request; the cost is that a value containing a genuine literal '+' reads back
// as a space.
//
// Callers that want the untouched wire bytes use QueryParam(), which is raw.
// Assertions about the encoding itself must use the raw form.
std::string FormDecode(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            result += ' ';
            continue;
        }
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = HexValue(value[i + 1]);
            const int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        result += value[i];
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> SplitQuery(const std::string &query)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    std::istringstream stream(query);
    std::string chunk;
    while (std::getline(stream, chunk, '&')) {
        if (chunk.empty()) {
            continue;
        }
        const auto eq = chunk.find('=');
        if (eq == std::string::npos) {
            pairs.emplace_back(chunk, std::string());
        } else {
            pairs.emplace_back(chunk.substr(0, eq), chunk.substr(eq + 1));
        }
    }
    return pairs;
}

// A '$'-prefixed OData system query option may appear literally ("$select") or
// percent-encoded ("%24select"); accept a bare name too so callers can be sloppy.
bool QueryKeyMatches(const std::string &actual_key, const std::string &wanted)
{
    if (actual_key == wanted) {
        return true;
    }
    const std::string bare = (!wanted.empty() && wanted[0] == '$') ? wanted.substr(1) : wanted;
    if (actual_key == bare) {
        return true;
    }
    return ToLowerAscii(actual_key) == ToLowerAscii("%24" + bare);
}

std::string JoinJsonObjects(const std::vector<std::string> &json_objects)
{
    std::string joined;
    for (std::size_t i = 0; i < json_objects.size(); ++i) {
        if (i > 0) {
            joined += ",";
        }
        joined += json_objects[i];
    }
    return joined;
}

std::string JsonEscape(const std::string &value)
{
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto c : value) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

}  // namespace

// ----------------------------------------------------------------------
// RecordedRequest

bool RecordedRequest::HasQueryParam(const std::string &name) const
{
    for (const auto &kv : SplitQuery(query)) {
        if (QueryKeyMatches(kv.first, name)) {
            return true;
        }
    }
    return false;
}

std::string RecordedRequest::QueryParam(const std::string &name) const
{
    for (const auto &kv : SplitQuery(query)) {
        if (QueryKeyMatches(kv.first, name)) {
            return kv.second;
        }
    }
    return std::string();
}

std::string RecordedRequest::DecodedQueryParam(const std::string &name) const
{
    return FormDecode(QueryParam(name));
}

std::string RecordedRequest::Header(const std::string &name) const
{
    const auto it = headers.find(ToLowerAscii(name));
    return it == headers.end() ? std::string() : it->second;
}

bool RecordedRequest::HasHeader(const std::string &name) const
{
    return headers.find(ToLowerAscii(name)) != headers.end();
}

// ----------------------------------------------------------------------
// CannedResponse

CannedResponse CannedResponse::Json(std::string body, int status)
{
    CannedResponse response;
    response.status = status;
    response.content_type = "application/json";
    response.body = std::move(body);
    return response;
}

CannedResponse CannedResponse::Xml(std::string body, int status)
{
    CannedResponse response;
    response.status = status;
    response.content_type = "application/xml";
    response.body = std::move(body);
    return response;
}

CannedResponse CannedResponse::Error(int status, std::string body)
{
    CannedResponse response;
    response.status = status;
    response.content_type = "application/json";
    response.body = body.empty()
                        ? std::string(R"({"error":{"code":"test","message":"canned error"}})")
                        : std::move(body);
    return response;
}

CannedResponse &CannedResponse::WithHeader(const std::string &name, const std::string &value)
{
    headers[name] = value;
    return *this;
}

CannedResponse &CannedResponse::TruncatedAfter(std::size_t bytes)
{
    truncate_after_bytes = bytes;
    return *this;
}

CannedResponse &CannedResponse::DelayedBy(int milliseconds)
{
    delay_ms = milliseconds;
    return *this;
}

// ----------------------------------------------------------------------
// ODataTestServer

struct ODataTestServer::Impl {
    struct Route {
        RequestMatcher matcher;
        std::vector<CannedResponse> responses;
        std::size_t served_count = 0;
    };

    duckdb_httplib_openssl::Server server;
    std::thread listener;
    int port = 0;
    mutable std::mutex mutex;
    std::vector<RecordedRequest> requests;
    std::vector<Route> routes;
};

namespace {

RecordedRequest Translate(const duckdb_httplib_openssl::Request &request)
{
    RecordedRequest recorded;
    recorded.method = request.method;
    recorded.path = request.path;
    recorded.target = request.target;
    recorded.body = request.body;

    const auto question_mark = request.target.find('?');
    if (question_mark != std::string::npos) {
        recorded.query = request.target.substr(question_mark + 1);
    }

    for (const auto &header : request.headers) {
        recorded.headers[ToLowerAscii(header.first)] = header.second;
    }
    return recorded;
}

}  // namespace

ODataTestServer::ODataTestServer() : impl(std::make_unique<Impl>())
{
    auto *state = impl.get();

    state->server.set_keep_alive_timeout(KEEP_ALIVE_TIMEOUT_SECONDS);
    state->server.set_read_timeout(READ_TIMEOUT_SECONDS, 0);
    state->server.set_idle_interval(0, IDLE_INTERVAL_MICROSECONDS);

    auto respond = [state](const duckdb_httplib_openssl::Request &request,
                           duckdb_httplib_openssl::Response &response) {
            const auto recorded = Translate(request);

            CannedResponse canned;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->requests.push_back(recorded);

                Impl::Route *hit = nullptr;
                for (auto &route : state->routes) {
                    if (route.matcher && route.matcher(recorded)) {
                        hit = &route;
                        break;
                    }
                }

                if (hit != nullptr && !hit->responses.empty()) {
                    const std::size_t index =
                        std::min(hit->served_count, hit->responses.size() - 1);
                    canned = hit->responses[index];
                    hit->served_count++;
                } else {
                    canned = CannedResponse::Error(
                        404,
                        std::string(R"({"error":{"code":"no_canned_response","message":")") +
                            JsonEscape(recorded.target) + R"("}})");
                }
            }

            // Held before anything is written, so the client is waiting on the response
            // rather than on the connection. The request has already been recorded, which
            // matters: a timeout test wants to prove the request went OUT and the client
            // gave up waiting, not that it was never sent.
            if (canned.delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(canned.delay_ms));
            }

            response.status = canned.status;
            if (canned.truncate_after_bytes > 0 &&
                canned.truncate_after_bytes < canned.body.size()) {
                // Announce the whole body, deliver a prefix, then fail the provider so
                // httplib closes the connection. The client sees a short read against a
                // declared Content-Length - a service dying mid-response.
                const auto body = canned.body;
                const auto cut = canned.truncate_after_bytes;
                response.set_content_provider(
                    body.size(), canned.content_type.c_str(),
                    [body, cut](std::size_t offset, std::size_t length,
                                duckdb_httplib_openssl::DataSink &sink) -> bool {
                        if (offset >= cut) {
                            return false;  // give up mid-body
                        }
                        const auto take = std::min(length, cut - offset);
                        sink.write(body.data() + offset, take);
                        return true;
                    });
            } else {
                response.set_content(canned.body, canned.content_type.c_str());
            }
            for (const auto &header : canned.headers) {
                if (ToLowerAscii(header.first) == "content-type") {
                    continue;
                }
                response.set_header(header.first, header.second);
            }
        };

    // httplib calls pre-routing handlers before it reads a POST body. Route POSTs
    // through a normal handler so recorded request bodies include COPY payloads.
    state->server.set_pre_routing_handler(
        [respond](const duckdb_httplib_openssl::Request &request,
                  duckdb_httplib_openssl::Response &response) {
            if (request.method == "POST") {
                return duckdb_httplib_openssl::Server::HandlerResponse::Unhandled;
            }
            respond(request, response);
            return duckdb_httplib_openssl::Server::HandlerResponse::Handled;
        });
    state->server.Post(R"(.*)", respond);

    state->port = state->server.bind_to_any_port(LOOPBACK_HOST);
    if (state->port <= 0) {
        throw std::runtime_error("ODataTestServer: failed to bind an ephemeral port on 127.0.0.1");
    }

    state->listener = std::thread([state]() { state->server.listen_after_bind(); });
    state->server.wait_until_ready();
}

ODataTestServer::~ODataTestServer()
{
    if (!impl) {
        return;
    }
    impl->server.stop();
    if (impl->listener.joinable()) {
        impl->listener.join();
    }
}

int ODataTestServer::Port() const
{
    return impl->port;
}

std::string ODataTestServer::BaseUrl() const
{
    return std::string("http://") + LOOPBACK_HOST + ":" + std::to_string(impl->port);
}

std::string ODataTestServer::Url(const std::string &path) const
{
    if (path.empty() || path[0] != '/') {
        return BaseUrl() + "/" + path;
    }
    return BaseUrl() + path;
}

void ODataTestServer::OnPath(const std::string &path, CannedResponse response)
{
    OnPathSequence(path, {std::move(response)});
}

void ODataTestServer::OnPathSequence(const std::string &path, std::vector<CannedResponse> responses)
{
    OnMatchSequence([path](const RecordedRequest &request) { return request.path == path; },
                    std::move(responses));
}

void ODataTestServer::OnMatch(RequestMatcher matcher, CannedResponse response)
{
    OnMatchSequence(std::move(matcher), {std::move(response)});
}

void ODataTestServer::OnMatchSequence(RequestMatcher matcher, std::vector<CannedResponse> responses)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    Impl::Route route;
    route.matcher = std::move(matcher);
    route.responses = std::move(responses);
    impl->routes.push_back(std::move(route));
}

void ODataTestServer::ServeMetadata(const std::string &path, const std::string &edmx_xml)
{
    OnPath(path, CannedResponse::Xml(edmx_xml));
}

void ODataTestServer::ServeMetadataFixture(const std::string &path,
                                           const std::string &fixture_file_name)
{
    ServeMetadata(path, ReadFixture(fixture_file_name));
}

std::vector<RecordedRequest> ODataTestServer::Requests() const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->requests;
}

std::vector<RecordedRequest> ODataTestServer::RequestsFor(const std::string &path) const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<RecordedRequest> matches;
    for (const auto &request : impl->requests) {
        if (request.path == path) {
            matches.push_back(request);
        }
    }
    return matches;
}

std::size_t ODataTestServer::RequestCount() const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->requests.size();
}

void ODataTestServer::ClearRequests()
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->requests.clear();
}

// ----------------------------------------------------------------------
// Fixtures

std::string FixtureDirectory()
{
    // __FILE__ is the absolute path of this file in the CMake build, so the
    // fixtures sit next to it regardless of the working directory the test
    // binary happens to be started from.
    const std::string this_file(__FILE__);
    const auto last_slash = this_file.find_last_of("/\\");
    if (last_slash == std::string::npos) {
        return std::string(".");
    }
    return this_file.substr(0, last_slash);
}

std::string ReadFixture(const std::string &file_name)
{
    const std::string full_path = FixtureDirectory() + "/" + file_name;
    std::ifstream stream(full_path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("ODataTestServer: cannot open fixture " + full_path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// ----------------------------------------------------------------------
// Payload builders

std::string MakeV4Page(const std::string &context_url,
                       const std::vector<std::string> &json_objects,
                       const std::string &next_link)
{
    std::string page = "{\"@odata.context\":\"" + JsonEscape(context_url) + "\"";
    if (!next_link.empty()) {
        page += ",\"@odata.nextLink\":\"" + JsonEscape(next_link) + "\"";
    }
    page += ",\"value\":[" + JoinJsonObjects(json_objects) + "]}";
    return page;
}

std::string MakeV2Page(const std::vector<std::string> &json_objects, const std::string &next_link)
{
    std::string page = "{\"d\":{\"results\":[" + JoinJsonObjects(json_objects) + "]";
    if (!next_link.empty()) {
        page += ",\"__next\":\"" + JsonEscape(next_link) + "\"";
    }
    page += "}}";
    return page;
}

CannedResponse MakeOdpDeltaPage(const std::vector<std::string> &json_objects,
                                const std::string &delta_link)
{
    std::string page = "{\"d\":{\"results\":[" + JoinJsonObjects(json_objects) + "]";
    if (!delta_link.empty()) {
        page += ",\"__delta\":\"" + JsonEscape(delta_link) + "\"";
    }
    page += "}}";

    auto response = CannedResponse::Json(page);
    response.WithHeader("Preference-Applied", "odata.track-changes");
    return response;
}

}  // namespace test_support
}  // namespace erpl_web
