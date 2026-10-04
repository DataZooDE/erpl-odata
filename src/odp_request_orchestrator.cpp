#include "odp_request_orchestrator.hpp"
#include "odata_url_helpers.hpp"
#include "odp_trace_redaction.hpp"
#include "tracing.hpp"
#include <iomanip>
#include "yyjson.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <locale>
#include <regex>
#include <sstream>
#include <thread>

namespace erpl_web {

namespace {

/// HTTP status returned by SAP while an extraction package is still being prepared.
constexpr int HTTP_STATUS_ACCEPTED = 202;

/// Days since the Unix epoch for a proleptic Gregorian y/m/d, after Howard Hinnant's
/// `days_from_civil`. Used instead of `timegm` (absent on MSVC) or `_mkgmtime` (absent
/// everywhere else) so that HTTP-date handling is identical on every supported platform.
int64_t DaysFromCivil(int64_t year, uint32_t month, uint32_t day)
{
    year -= (month <= 2) ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t year_of_era = static_cast<uint32_t>(year - era * 400);
    const uint32_t day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const uint32_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

/// Convert a `std::tm` already expressed in UTC to a Unix timestamp, without consulting
/// the process timezone (which is what `std::mktime` would do).
int64_t TimestampFromUtcTm(const std::tm& time_fields)
{
    const int64_t days = DaysFromCivil(static_cast<int64_t>(time_fields.tm_year) + 1900,
                                       static_cast<uint32_t>(time_fields.tm_mon) + 1,
                                       static_cast<uint32_t>(time_fields.tm_mday));
    return days * 86400 + time_fields.tm_hour * 3600 + time_fields.tm_min * 60 + time_fields.tm_sec;
}

std::string TrimWhitespace(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

/// Try each of the three date formats RFC 7231 requires a recipient to accept.
bool ParseHttpDate(const std::string& value, int64_t& out_timestamp)
{
    static const std::array<const char*, 3> FORMATS = {
        "%a, %d %b %Y %H:%M:%S",  // IMF-fixdate  - "Sun, 06 Nov 1994 08:49:37 GMT"
        "%A, %d-%b-%y %H:%M:%S",  // RFC 850      - "Sunday, 06-Nov-94 08:49:37 GMT"
        "%a %b %d %H:%M:%S %Y"    // asctime      - "Sun Nov  6 08:49:37 1994"
    };

    for (const auto* format : FORMATS) {
        std::tm time_fields = {};
        std::istringstream stream(value);
        stream.imbue(std::locale::classic());
        stream >> std::get_time(&time_fields, format);
        if (stream.fail()) {
            continue;
        }
        // RFC 850 two-digit years: std::get_time maps %y to 1969..2068, which is what we want.
        out_timestamp = TimestampFromUtcTm(time_fields);
        return true;
    }
    return false;
}

} // namespace

OdpRequestOrchestrator::OdpRequestOrchestrator(std::shared_ptr<HttpAuthParams> auth_params,
                                             uint32_t default_page_size)
    : http_factory_(std::make_unique<OdpHttpRequestFactory>(auth_params))
    , auth_params_(auth_params)
    , default_page_size_(default_page_size)
    , sleep_function_([](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); })
{
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
        "Initializing request orchestrator with default page size: %u", default_page_size_));
    
    // Create HTTP client with OData-friendly settings
    HttpParams http_params;
    http_params.url_encode = false; // OData URLs should not be encoded
    http_client_ = std::make_shared<HttpClient>(http_params);
    
    // Set default page size on factory
    http_factory_->SetDefaultPageSize(default_page_size_);
}

OdpRequestOrchestrator::OdpRequestResult OdpRequestOrchestrator::ExecuteInitialLoad(
    const std::string& url, std::optional<uint32_t> max_page_size) {
    
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", "Executing initial load for URL: " + url);
    service_origin_url_ = url;
    
    // Create initial load request with change tracking
    HttpRequest request = http_factory_->CreateInitialLoadRequest(url, max_page_size);
    
    // Return the first page only. The caller (OdpODataReadBindData) is responsible for
    // following __next links page by page and accumulating rows incrementally.
    return ExecuteRequest(request, OdpOperation::INITIAL_LOAD);
}

OdpRequestOrchestrator::OdpRequestResult OdpRequestOrchestrator::ExecuteDeltaFetch(
    const std::string& url, const std::string& delta_token, std::optional<uint32_t> max_page_size) {
    
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
        "Executing delta fetch for URL: %s, Token: %s", url, delta_token.substr(0, 20) + "..."));
    
    // Normalize token (strip surrounding quotes) and build delta URL
    std::string normalized_token = delta_token;
    if (!normalized_token.empty() && (normalized_token.front() == '\'' || normalized_token.front() == '"') &&
        normalized_token.back() == normalized_token.front()) {
        normalized_token = normalized_token.substr(1, normalized_token.size() - 2);
    }
    if (normalized_token != delta_token) {
        ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
            "Normalized delta token from '%s' to '%s",
            delta_token.substr(0, 64), normalized_token.substr(0, 64)));
    }
    service_origin_url_ = url;
    std::string delta_url = BuildDeltaUrl(url, normalized_token);
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", "Constructed delta URL: " + delta_url);
    
    // Create delta fetch request
    HttpRequest request = http_factory_->CreateDeltaFetchRequest(delta_url, max_page_size);
    
    return ExecuteRequest(request, OdpOperation::DELTA_FETCH);
}

OdpRequestOrchestrator::OdpRequestResult OdpRequestOrchestrator::ExecuteNextPage(const std::string& next_url,
                                                                                  bool change_tracking_confirmed) {
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", "Executing next page request for URL: " + next_url);

    // Same check the 202 Location follow below makes, for the same reason: this link comes
    // from the service and goes into a request line written verbatim. The guard was added
    // to that sibling and not to this one - which is how the comment down there came to
    // claim coverage this function did not have.
    if (!IsWireSafeUrl(next_url)) {
        throw duckdb::IOException(
            "ODP service returned a next link containing characters that cannot be sent in a "
            "request.");
    }

    // Create simple GET request for next page (no special ODP headers needed)
    HttpRequest request(HttpMethod::GET, next_url);

    // The next link comes from the server. Attaching the caller's credentials to
    // whatever host it names would hand the bearer token to that host, so follow
    // the rule HttpClient already applies to redirects: same origin keeps the
    // credentials, anything else does not (#101).
    // Fail CLOSED on an unset origin. Written as `empty() || IsSameOrigin(...)` this gate
    // attached credentials to whatever host the server named whenever the origin had not
    // been recorded yet - the wrong polarity for a credential decision. Today's call
    // ordering happens to set it first, so this was latent rather than exploitable, but
    // "we don't know the origin" must mean "send nothing", not "send everything"
    // (GitHub #187).
    const bool same_origin = !service_origin_url_.empty() && IsSameOrigin(service_origin_url_, next_url);
    if (!same_origin) {
        ERPL_TRACE_WARN("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
            "Server-supplied next link points at a different origin than the service (%s -> %s); "
            "requesting it without credentials",
            service_origin_url_, next_url));
    }

    // Apply authentication if available
    if (auth_params_ && same_origin) {
        request.AuthHeadersFromParams(*auth_params_);
    }
    
    return ExecuteRequest(request, OdpOperation::NEXT_PAGE, change_tracking_confirmed);
}

bool OdpRequestOrchestrator::ValidatePreferenceApplied(const HttpResponse& response) {
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Validating preference-applied header");
    
    // Look for preference-applied header
    auto it = response.headers.find("preference-applied");
    if (it == response.headers.end()) {
        ERPL_TRACE_WARN("ODP_ORCHESTRATOR", "No preference-applied header found in response");
        return false;
    }
    
    std::string preference_header = it->second;
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Found preference-applied header: " + preference_header);
    
    // Check if it contains odata.track-changes
    bool contains_track_changes = preference_header.find("odata.track-changes") != std::string::npos;
    
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
        "Preference validation result: %s", contains_track_changes ? "VALID" : "INVALID"));
    
    return contains_track_changes;
}

std::string OdpRequestOrchestrator::ExtractDeltaToken(const ODataEntitySetResponse& response) {
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Extracting delta token from response");

    // One implementation for every spelling, including the one this code used to miss:
    // SAP ODP hands the token back on "__next" as "...&!deltatoken=...", not on "__delta".
    // Reading only "__delta" left the subscription in initial-load mode, so the next read
    // re-extracted the entire dataset. See GitHub #102.
    const auto delta_link = ODataDeltaLink::ExtractDeltaLink(response.RawContent());
    const auto delta_token = ODataDeltaLink::ExtractToken(delta_link);

    if (!delta_token.empty()) {
        ERPL_TRACE_INFO("ODP_ORCHESTRATOR", "Extracted delta token: " + delta_token.substr(0, 20) + "...");
    } else {
        ERPL_TRACE_WARN("ODP_ORCHESTRATOR", "No delta token found in response");
    }

    return delta_token;
}

std::string OdpRequestOrchestrator::FetchDeltaTokenFromDeltaLinks(const HttpUrl& entity_set_url)
{
    // .../<service>/FactsOfX  ->  .../<service>/DeltaLinksOfFactsOfX
    auto path = entity_set_url.Path();
    const auto last_slash = path.rfind('/');
    if (last_slash == std::string::npos || last_slash + 1 >= path.size()) {
        return std::string();
    }
    const auto entity_set_name = path.substr(last_slash + 1);
    if (entity_set_name.rfind("DeltaLinksOf", 0) == 0) {
        return std::string();  // never recurse into the delta-links set itself
    }

    HttpUrl delta_links_url(entity_set_url);
    delta_links_url.Path(path.substr(0, last_slash + 1) + "DeltaLinksOf" + entity_set_name);
    delta_links_url.Query("?$format=json");

    try {
        HttpRequest request(HttpMethod::GET, delta_links_url);
        request.headers["Accept"] = "application/json";
        if (auth_params_ != nullptr && delta_links_url.IsSameOrigin(entity_set_url)) {
            request.AuthHeadersFromParams(*auth_params_);
        }

        auto response = http_client_->SendRequest(request);
        if (!response || response->Code() < 200 || response->Code() >= 300) {
            ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR",
                             "DeltaLinksOf lookup did not succeed; staying in initial-load mode");
            return std::string();
        }

        const auto token = ODataDeltaLink::ExtractTokenFromDeltaLinksPayload(response->Content());
        if (token.empty()) {
            ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "DeltaLinksOf carried no token");
        } else {
            ERPL_TRACE_INFO("ODP_ORCHESTRATOR",
                            "Recovered delta token from DeltaLinksOf: " + token.substr(0, 20) + "...");
        }
        return token;
    } catch (const std::exception& e) {
        // Never fail the extraction over this: the rows have already been delivered, and a
        // missing token only costs a full re-read next time.
        ERPL_TRACE_WARN("ODP_ORCHESTRATOR",
                        std::string("DeltaLinksOf lookup failed: ") + e.what());
        return std::string();
    }
}

std::string OdpRequestOrchestrator::BuildDeltaUrl(const std::string& base_url, const std::string& delta_token) {
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
        "Building delta URL from base: %s, token: %s", base_url, delta_token.substr(0, 20) + "..."));
    
    // For OData v2, the delta link uses a query param syntax: ?$format=json&!deltatoken=TOKEN
    std::string delta_url = base_url;
    bool has_query = (delta_url.find('?') != std::string::npos);
    
    // Ensure $format=json present first
    if (delta_url.find("$format=json") == std::string::npos) {
        delta_url += has_query ? '&' : '?';
        delta_url += "$format=json";
        has_query = true;
    }
    
    // Append &!deltatoken=TOKEN. The token is server-supplied, so it is
    // percent-encoded rather than pasted in raw: an unescaped '&' or '#' in it
    // would otherwise change the query the service sees (#105).
    delta_url += has_query ? '&' : '?';
    delta_url += "!deltatoken=";
    delta_url += EncodeDeltaToken(delta_token);
    
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Built delta URL: " + delta_url);
    return delta_url;
}

void OdpRequestOrchestrator::SetDefaultPageSize(uint32_t page_size) {
    default_page_size_ = page_size;
    http_factory_->SetDefaultPageSize(page_size);
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", "Updated default page size to: " + std::to_string(page_size));
}

uint32_t OdpRequestOrchestrator::GetDefaultPageSize() const {
    return default_page_size_;
}

void OdpRequestOrchestrator::SetRetryPolicy(const RetryPolicy& policy) {
    retry_policy_ = policy;
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
        "Updated 202 retry policy - MaxAttempts: %u, DefaultDelay: %lld ms, MaxTotalWait: %lld ms",
        retry_policy_.max_attempts,
        static_cast<long long>(retry_policy_.default_delay.count()),
        static_cast<long long>(retry_policy_.max_total_wait.count())));
}

const OdpRequestOrchestrator::RetryPolicy& OdpRequestOrchestrator::GetRetryPolicy() const {
    return retry_policy_;
}

void OdpRequestOrchestrator::SetSleepFunction(SleepFunction sleep_function) {
    sleep_function_ = std::move(sleep_function);
}

std::optional<std::chrono::milliseconds> OdpRequestOrchestrator::ParseRetryAfter(
    const std::string& header_value, std::chrono::system_clock::time_point now) {

    const std::string trimmed = TrimWhitespace(header_value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    // Form 1: delta-seconds. A bare run of digits, per RFC 7231; anything else falls through
    // to the date parser rather than being partially consumed.
    if (std::all_of(trimmed.begin(), trimmed.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        try {
            const auto seconds = std::stoll(trimmed);
            return std::chrono::milliseconds(std::chrono::seconds(seconds));
        } catch (const std::exception&) {
            // Out of range - treat as unparsable so the caller uses its default delay.
            return std::nullopt;
        }
    }

    // Form 2: HTTP-date.
    int64_t target_timestamp = 0;
    if (!ParseHttpDate(trimmed, target_timestamp)) {
        return std::nullopt;
    }

    const auto now_timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const int64_t delta_seconds = target_timestamp - static_cast<int64_t>(now_timestamp);
    if (delta_seconds <= 0) {
        // The server named an instant that has already passed - retry immediately.
        return std::chrono::milliseconds(0);
    }

    return std::chrono::milliseconds(std::chrono::seconds(delta_seconds));
}

// ============================================================================
// Private Helper Methods
// ============================================================================

const char* OdpOperationName(OdpOperation operation) {
    switch (operation) {
    case OdpOperation::INITIAL_LOAD:
        return "initial_load";
    case OdpOperation::DELTA_FETCH:
        return "delta_fetch";
    case OdpOperation::NEXT_PAGE:
        return "next_page";
    }
    return "unknown";
}

OdpRequestOrchestrator::OdpRequestResult OdpRequestOrchestrator::ExecuteRequest(
    const HttpRequest& request, OdpOperation operation, bool change_tracking_confirmed) {
    const std::string operation_type = OdpOperationName(operation);

    LogRequestDetails(request, operation);
    
    OdpRequestResult result;
    
    try {
        // Execute HTTP request, absorbing any 202 Accepted "still preparing" responses.
        auto http_response = SendRequestHandlingAccepted(request, operation);

        result.http_status_code = http_response->Code();
        result.response_size_bytes = http_response->Content().size();
        
        ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
            "HTTP request completed - Status: %d, Size: %zu bytes", 
            result.http_status_code, result.response_size_bytes));
        
        // Check for HTTP errors. The status and the SAP error code travel with
        // the exception so callers do not have to grep the message text (#94),
        // and the body is truncated before it reaches a log or an audit row.
        if (result.http_status_code >= 400) {
            const std::string error_code = ExtractErrorCode(http_response->content);
            throw OdpHttpException(result.http_status_code, error_code,
                "ODP request failed with HTTP " + std::to_string(result.http_status_code) +
                (error_code.empty() ? std::string() : " (" + error_code + ")") + ": " +
                odp_trace::TruncateBody(http_response->content));
        }
        
        // Capture the real response headers and validate the server's change-tracking promise
        // BEFORE the response is moved into the OData wrapper. Inferring `preference_applied`
        // from the presence of a delta token used to let the reader transition to DELTA_FETCH
        // over data that was never change-tracked, silently dropping every subsequent change.
        // See GitHub #97.
        result.response_headers = http_response->headers;
        if (operation == OdpOperation::INITIAL_LOAD) {
            result.preference_applied = ValidatePreferenceApplied(*http_response);
            change_tracking_confirmed = result.preference_applied;
            if (!result.preference_applied) {
                ERPL_TRACE_WARN("ODP_ORCHESTRATOR",
                    "Initial load response did not carry 'Preference-Applied: odata.track-changes'. "
                    "Change tracking was NOT established; the subscription must stay in initial-load "
                    "mode rather than issue a delta fetch that would miss changes.");
            }
        }

        // Process OData response
        result.response = ProcessHttpResponse(std::move(http_response));

        // Extract delta token if present
        result.extracted_delta_token = ExtractDeltaToken(*result.response);


        // Check for next page
        auto next_url = result.response->NextUrl();
        result.has_more_pages = next_url.has_value() && !next_url->empty();

        // The body does not always carry a delta link. When the server confirmed change
        // tracking and this was the terminal page, ask the service for the token rather
        // than silently dropping back to initial-load mode and re-extracting everything on
        // the next read (GitHub #169). Checked after has_more_pages is known, because only
        // the last page of an extraction can carry a token.
        //
        // change_tracking_confirmed, not result.preference_applied: the latter is only ever set for the
        // first page, so gating on it meant this never ran on the last page of a multi-page extraction -
        // the only page where !has_more_pages holds (GitHub #250).
        if (result.extracted_delta_token.empty() && change_tracking_confirmed &&
            !result.has_more_pages) {
            ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR",
                             "Change tracking was applied but the response carried no delta link; "
                             "asking the service for the current token");
            result.extracted_delta_token = FetchDeltaTokenFromDeltaLinks(request.url);
        }

        LogResponseDetails(result, operation);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
            "Request failed for operation %s: %s", operation_type, e.what()));
        throw;
    }
    
    return result;
}

std::unique_ptr<HttpResponse> OdpRequestOrchestrator::SendRequestHandlingAccepted(
    const HttpRequest& request, OdpOperation operation) {
    const std::string operation_type = OdpOperationName(operation);

    HttpRequest current_request = request;
    std::chrono::milliseconds total_waited{0};

    for (uint32_t attempt = 0; ; ++attempt) {
        auto http_response = http_client_->SendRequest(current_request);
        if (!http_response) {
            throw duckdb::IOException("ODP " + operation_type + " request returned no response for '" +
                                      current_request.url.ToString() + "'");
        }

        if (http_response->Code() != HTTP_STATUS_ACCEPTED) {
            if (attempt > 0) {
                ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
                    "ODP %s package became ready after %u re-poll(s) and %lld ms of waiting",
                    operation_type, attempt, static_cast<long long>(total_waited.count())));
            }
            return http_response;
        }

        // SAP is still preparing the extraction package.
        if (attempt >= retry_policy_.max_attempts) {
            throw duckdb::IOException(duckdb::StringUtil::Format(
                "ODP %s did not become ready: the service answered HTTP 202 Accepted %u times for "
                "'%s' (waited %lld ms in total). The extraction is still being prepared on the SAP "
                "side; re-run the query later, or raise the retry limit.",
                operation_type, attempt + 1, current_request.url.ToString(),
                static_cast<long long>(total_waited.count())));
        }

        std::chrono::milliseconds wait_for = retry_policy_.default_delay;
        auto retry_after_header = http_response->headers.find("retry-after");
        if (retry_after_header != http_response->headers.end()) {
            auto parsed = ParseRetryAfter(retry_after_header->second, std::chrono::system_clock::now());
            if (parsed.has_value()) {
                wait_for = *parsed;
            } else {
                ERPL_TRACE_WARN("ODP_ORCHESTRATOR",
                    "Could not parse Retry-After value '" + retry_after_header->second +
                    "'; falling back to the default delay");
            }
        }

        if (total_waited + wait_for > retry_policy_.max_total_wait) {
            throw duckdb::IOException(duckdb::StringUtil::Format(
                "ODP %s did not become ready within the %lld ms wait budget: the service answered "
                "HTTP 202 Accepted for '%s' and asked to be retried in a further %lld ms (already "
                "waited %lld ms). The extraction is still being prepared on the SAP side; re-run "
                "the query later, or raise the wait budget.",
                operation_type, static_cast<long long>(retry_policy_.max_total_wait.count()),
                current_request.url.ToString(),
                static_cast<long long>(wait_for.count()),
                static_cast<long long>(total_waited.count())));
        }

        // A 202 may name the resource to poll in `Location`. Honour it when the server sends one,
        // because re-issuing the original initial-load request (which carries
        // `Prefer: odata.track-changes`) risks opening a second ODQ subscription. Restrict the
        // redirect to the same origin, consistent with the rest of the ODP request path.
        auto location_header = http_response->headers.find("location");
        if (location_header != http_response->headers.end() && !location_header->second.empty()) {
            // The Location comes from the service and goes into the request line, which is
            // written verbatim - so CR/LF in it would inject a header on a credentialed
            // request. Uses IsWireSafeUrl, as ExecuteNextPage above does. (This comment
            // previously claimed every follower used it; ExecuteNextPage did not, which is
            // the sibling gap the claim was meant to rule out.)
            if (!IsWireSafeUrl(location_header->second)) {
                throw duckdb::IOException(
                    "ODP " + operation_type + " received an HTTP 202 whose Location header "
                    "contains characters that cannot be sent in a request.");
            }
            HttpUrl poll_url =
                HttpUrl::MergeWithBaseUrlIfRelative(current_request.url, location_header->second);
            if (!poll_url.IsSameOrigin(current_request.url)) {
                throw duckdb::IOException(
                    "ODP " + operation_type + " received an HTTP 202 whose Location header points to "
                    "a different origin ('" + poll_url.ToString() + "' vs '" +
                    current_request.url.ToSchemeHostAndPort() + "'); refusing to follow it.");
            }
            if (!poll_url.Equals(current_request.url)) {
                ERPL_TRACE_INFO("ODP_ORCHESTRATOR",
                    "Following 202 Location for polling: " + poll_url.ToString());
                current_request = HttpRequest(HttpMethod::GET, poll_url.ToString());
                if (auth_params_) {
                    current_request.AuthHeadersFromParams(*auth_params_);
                }
            }
        }

        ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
            "ODP %s not ready (HTTP 202, attempt %u of %u); waiting %lld ms before re-polling %s",
            operation_type, attempt + 1, retry_policy_.max_attempts + 1,
            static_cast<long long>(wait_for.count()), current_request.url.ToString()));

        if (sleep_function_ && wait_for.count() > 0) {
            sleep_function_(wait_for);
        }
        total_waited += wait_for;
    }
}

std::shared_ptr<ODataEntitySetResponse> OdpRequestOrchestrator::ProcessHttpResponse(
    std::unique_ptr<HttpResponse> http_response) {
    
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Processing HTTP response to OData response");
    
    // Determine OData version from response
    ODataVersion version = ODataVersion::V2; // ODP typically uses V2
    
    // Create OData response wrapper
    auto odata_response = std::make_shared<ODataEntitySetResponse>(std::move(http_response), version);
    
    return odata_response;
}

void OdpRequestOrchestrator::LogRequestDetails(const HttpRequest& request, OdpOperation operation) const {
    std::stringstream log_msg;
    log_msg << "Executing " << OdpOperationName(operation) << " request:" << std::endl;
    log_msg << "  Method: " << request.method.ToString() << std::endl;
    log_msg << "  URL: " << request.url.ToString() << std::endl;
    // Redacted: an Authorization header written verbatim puts the SAP password
    // or the Entra bearer token into the trace file (#100).
    log_msg << "  Headers:" << odp_trace::FormatHeaders(request.headers);
    
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", log_msg.str());
}

void OdpRequestOrchestrator::LogResponseDetails(const OdpRequestResult& result, OdpOperation operation) const {
    std::stringstream log_msg;
    log_msg << "Completed " << OdpOperationName(operation) << " request:" << std::endl;
    log_msg << "  HTTP Status: " << result.http_status_code << std::endl;
    log_msg << "  Response Size: " << result.response_size_bytes << " bytes" << std::endl;
    log_msg << "  Delta Token: " << (result.extracted_delta_token.empty() ? "NONE" : result.extracted_delta_token.substr(0, 20) + "...") << std::endl;
    log_msg << "  Has More Pages: " << (result.has_more_pages ? "YES" : "NO") << std::endl;
    log_msg << "  Preference Applied: " << (result.preference_applied ? "YES" : "NO");
    
    ERPL_TRACE_INFO("ODP_ORCHESTRATOR", log_msg.str());
}

std::string OdpRequestOrchestrator::ExtractDeltaTokenFromV2Response(const std::string& response_content) {
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Extracting delta token from OData v2 response");
    
    try {
        // Parse JSON response
        auto doc = duckdb_yyjson::yyjson_read(response_content.c_str(), response_content.length(), 0);
        if (!doc) {
            ERPL_TRACE_WARN("ODP_ORCHESTRATOR", "Failed to parse JSON response");
            return "";
        }
        
        auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
        if (!root) {
            duckdb_yyjson::yyjson_doc_free(doc);
            return "";
        }
        
        // Look for __delta property in OData v2 format
        auto d_obj = duckdb_yyjson::yyjson_obj_get(root, "d");
        if (d_obj) {
            auto delta_val = duckdb_yyjson::yyjson_obj_get(d_obj, "__delta");
            if (delta_val && duckdb_yyjson::yyjson_is_str(delta_val)) {
                std::string delta_url = duckdb_yyjson::yyjson_get_str(delta_val);
                duckdb_yyjson::yyjson_doc_free(doc);
                
                // Extract token from delta URL
                auto token = ExtractTokenFromDeltaUrl(delta_url);
                ERPL_TRACE_INFO("ODP_ORCHESTRATOR", "Found v2 delta link: " + delta_url);
                ERPL_TRACE_INFO("ODP_ORCHESTRATOR", duckdb::StringUtil::Format(
                    "Extracted token from v2 delta link: %s", token.substr(0, 64)));
                return token;
            }
        }
        
        duckdb_yyjson::yyjson_doc_free(doc);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_ORCHESTRATOR", "Error parsing v2 response: " + std::string(e.what()));
    }
    
    return "";
}

std::string OdpRequestOrchestrator::ExtractDeltaTokenFromV4Response(const std::string& response_content) {
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Extracting delta token from OData v4 response");
    
    try {
        // Parse JSON response
        auto doc = duckdb_yyjson::yyjson_read(response_content.c_str(), response_content.length(), 0);
        if (!doc) {
            ERPL_TRACE_WARN("ODP_ORCHESTRATOR", "Failed to parse JSON response");
            return "";
        }
        
        auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
        if (!root) {
            duckdb_yyjson::yyjson_doc_free(doc);
            return "";
        }
        
        // Look for @odata.deltaLink property in OData v4 format
        auto delta_val = duckdb_yyjson::yyjson_obj_get(root, "@odata.deltaLink");
        if (delta_val && duckdb_yyjson::yyjson_is_str(delta_val)) {
            std::string delta_url = duckdb_yyjson::yyjson_get_str(delta_val);
            duckdb_yyjson::yyjson_doc_free(doc);
            
            // Extract token from delta URL
            return ExtractTokenFromDeltaUrl(delta_url);
        }
        
        duckdb_yyjson::yyjson_doc_free(doc);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_ORCHESTRATOR", "Error parsing v4 response: " + std::string(e.what()));
    }
    
    return "";
}

std::string OdpRequestOrchestrator::ExtractTokenFromDeltaUrl(const std::string& delta_url) {
    ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Extracting token from delta URL: " + delta_url);
    
    // Look for !deltatoken= pattern (OData v2)
    std::regex v2_pattern(R"(!deltatoken=([^&]+))");
    std::smatch match;
    
    if (std::regex_search(delta_url, match, v2_pattern)) {
        std::string token = match[1].str();
        // Normalize: strip surrounding single/double quotes if present
        if (!token.empty() && (token.front() == '\'' || token.front() == '"') &&
            token.back() == token.front()) {
            token = token.substr(1, token.size() - 2);
        }
        ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Extracted v2 delta token: " + token.substr(0, 20) + "...");
        return token;
    }
    
    // Look for $deltatoken= pattern (OData v4)
    std::regex v4_pattern(R"(\$deltatoken=([^&]+))");
    if (std::regex_search(delta_url, match, v4_pattern)) {
        std::string token = match[1].str();
        ERPL_TRACE_DEBUG("ODP_ORCHESTRATOR", "Extracted v4 delta token: " + token.substr(0, 20) + "...");
        return token;
    }
    
    ERPL_TRACE_WARN("ODP_ORCHESTRATOR", "No delta token pattern found in URL");
    return "";
}

std::string OdpRequestOrchestrator::ExtractDeltaUrl(const ODataEntitySetResponse& response) {
    // Same single implementation, so the URL and the token can never disagree about
    // which spellings count as a delta link. See GitHub #102.
    return ODataDeltaLink::ExtractDeltaLink(response.RawContent());
}

std::string OdpRequestOrchestrator::NormalizeDeltaUrl(const std::string& delta_url) {
    // Ensure $format=json present and unquote token if quoted in URL
    std::string url = delta_url;
    // Strip quotes around token manually if present
    size_t pos = url.find("!deltatoken=");
    if (pos != std::string::npos) {
        size_t start = pos + std::string("!deltatoken=").size();
        if (start < url.size() && (url[start] == '\'' || url[start] == '"')) {
            char q = url[start];
            size_t end = url.find(q, start + 1);
            if (end != std::string::npos) {
                url.erase(end, 1);
                url.erase(start, 1);
            }
        }
    }
    // Ensure $format=json
    if (url.find("$format=json") == std::string::npos) {
        char sep = (url.find('?') == std::string::npos) ? '?' : '&';
        url += sep;
        url += "$format=json";
    }
    return url;
}
std::string OdpRequestOrchestrator::EnsureJsonFormat(const std::string& url) {
    if (HasJsonFormat(url)) {
        return url;
    }
    
    // Add $format=json parameter
    char separator = (url.find('?') != std::string::npos) ? '&' : '?';
    return url + separator + "$format=json";
}

bool OdpRequestOrchestrator::HasJsonFormat(const std::string& url) {
    return url.find("$format=json") != std::string::npos;
}

bool OdpRequestOrchestrator::IsSameOrigin(const std::string& reference_url, const std::string& candidate_url) {
    if (reference_url.empty() || candidate_url.empty()) {
        return false;
    }
    try {
        HttpUrl reference(reference_url);
        HttpUrl candidate = HttpUrl::MergeWithBaseUrlIfRelative(reference, candidate_url);
        return reference.IsSameOrigin(candidate);
    } catch (const std::exception& e) {
        // An unparseable link is not a link we should send credentials to.
        ERPL_TRACE_WARN("ODP_ORCHESTRATOR", "Could not compare origins: " + std::string(e.what()));
        return false;
    }
}

std::string OdpRequestOrchestrator::EncodeDeltaToken(const std::string& delta_token) {
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase << std::setfill('0');
    for (const unsigned char c : delta_token) {
        const bool is_unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                   c == '-' || c == '_' || c == '.' || c == '~';
        if (is_unreserved) {
            encoded << static_cast<char>(c);
        } else {
            encoded << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return encoded.str();
}

std::string OdpRequestOrchestrator::ExtractErrorCode(const std::string& response_content) {
    if (response_content.empty()) {
        return "";
    }

    // Both OData v2 and v4 error payloads carry {"error":{"code":"..."}}.
    auto doc = duckdb_yyjson::yyjson_read(response_content.c_str(), response_content.length(), 0);
    if (!doc) {
        return "";
    }

    std::string code;
    auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
    if (root) {
        auto error_obj = duckdb_yyjson::yyjson_obj_get(root, "error");
        if (error_obj) {
            auto code_val = duckdb_yyjson::yyjson_obj_get(error_obj, "code");
            if (code_val && duckdb_yyjson::yyjson_is_str(code_val)) {
                code = duckdb_yyjson::yyjson_get_str(code_val);
            }
        }
    }
    duckdb_yyjson::yyjson_doc_free(doc);
    return code;
}

std::string OdpRequestOrchestrator::NormalizeDeltaToken(const std::string& raw) {
    if (!raw.empty() &&
        (raw.front() == '\'' || raw.front() == '"') &&
        raw.back() == raw.front()) {
        return raw.substr(1, raw.size() - 2);
    }
    return raw;
}

} // namespace erpl_web
