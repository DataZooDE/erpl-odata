#pragma once

#include "odp_http_request_factory.hpp"
#include "odata_client.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "tracing.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <optional>

namespace erpl_web {

/**
 * @brief An ODP request that came back with an HTTP error status
 *
 * Carries the status code and the SAP error code from the response body, so a
 * caller can decide what to do without pattern-matching the message text. The
 * body itself is truncated; a full SAP error body is large and carries request
 * context that should not end up in a log or an audit row (#100, #105).
 */
class OdpHttpException : public std::runtime_error {
public:
    OdpHttpException(int http_status_code, std::string sap_error_code, const std::string& message)
        : std::runtime_error(message)
        , http_status_code_(http_status_code)
        , sap_error_code_(std::move(sap_error_code))
    {}

    int HttpStatusCode() const { return http_status_code_; }
    const std::string& SapErrorCode() const { return sap_error_code_; }

private:
    int http_status_code_;
    std::string sap_error_code_;
};

/**
 * @brief The kind of ODP request being made
 *
 * Behavioural gates (is change tracking validated here, may a delta token be staged) key on this, not
 * on a label: with string comparisons "initial_load" versus "next_page" the #169 delta-token fallback
 * could only ever fire for a single-page extraction and nothing noticed (GitHub #250).
 */
enum class OdpOperation { INITIAL_LOAD, DELTA_FETCH, NEXT_PAGE };

/// Stable lower-case name, used in logs and audit rows.
const char* OdpOperationName(OdpOperation operation);

/**
 * @brief Orchestrates ODP-specific HTTP requests and response processing
 * 
 * This class coordinates between the ODP HTTP factory, OData client, and response
 * processing to handle the complexities of ODP initial load and delta fetch operations.
 * It manages preference validation, delta token extraction, and error handling.
 */
class OdpRequestOrchestrator {
public:
    /**
     * @brief Result of an ODP request operation
     */
    struct OdpRequestResult {
        std::shared_ptr<ODataEntitySetResponse> response;
        std::string extracted_delta_token;
        std::string extracted_delta_url;
        /// Headers of the HTTP response that produced `response`. Preserved verbatim so that
        /// callers can validate server behaviour (notably `Preference-Applied`) rather than
        /// inferring it from the payload. `HeaderMap` lookup is case-insensitive.
        HeaderMap response_headers;
        /// True only when the server actually echoed `Preference-Applied: odata.track-changes`.
        /// Never inferred from the presence of a delta token - see GitHub #97.
        bool preference_applied;
        bool has_more_pages;
        int http_status_code;
        size_t response_size_bytes;

        OdpRequestResult()
            : preference_applied(false)
            , has_more_pages(false)
            , http_status_code(0)
            , response_size_bytes(0)
        {}
    };

    /**
     * @brief Bounds the re-poll loop that services HTTP 202 Accepted responses.
     *
     * SAP answers 202 while an extraction package is still being prepared. The client must
     * wait and ask again; these limits stop that from becoming an unbounded stall.
     */
    struct RetryPolicy {
        /// Maximum number of 202 responses tolerated before giving up.
        uint32_t max_attempts = 20;
        /// Wait used when a 202 carries no (or an unparsable) `Retry-After` header.
        std::chrono::milliseconds default_delay{std::chrono::seconds(5)};
        /// Upper bound on the summed wait across all re-polls of a single request.
        std::chrono::milliseconds max_total_wait{std::chrono::minutes(10)};
    };

    /// Injection seam for the wait between re-polls. Defaults to `std::this_thread::sleep_for`.
    /// Tests substitute a recording no-op so that they exercise the retry arithmetic without
    /// actually sleeping.
    using SleepFunction = std::function<void(std::chrono::milliseconds)>;

    /**
     * @brief Construct orchestrator with authentication parameters
     * @param auth_params HTTP authentication parameters
     * @param default_page_size Default page size for ODP requests
     */
    explicit OdpRequestOrchestrator(std::shared_ptr<HttpAuthParams> auth_params = nullptr,
                                   uint32_t default_page_size = 15000);

    ~OdpRequestOrchestrator() = default;

    // Non-copyable, non-movable
    OdpRequestOrchestrator(const OdpRequestOrchestrator&) = delete;
    OdpRequestOrchestrator& operator=(const OdpRequestOrchestrator&) = delete;
    OdpRequestOrchestrator(OdpRequestOrchestrator&&) = delete;
    OdpRequestOrchestrator& operator=(OdpRequestOrchestrator&&) = delete;

    /**
     * @brief Execute initial load request with change tracking
     * @param url Target entity set URL
     * @param max_page_size Optional page size override
     * @return Result containing response and extracted delta token
     */
    OdpRequestResult ExecuteInitialLoad(const std::string& url, 
                                       std::optional<uint32_t> max_page_size = std::nullopt);

    /**
     * @brief Execute delta fetch request using existing delta token
     * @param url Base entity set URL (delta token will be appended)
     * @param delta_token Delta token from previous request
     * @param max_page_size Optional page size override
     * @return Result containing response and new delta token
     */
    OdpRequestResult ExecuteDeltaFetch(const std::string& url, 
                                      const std::string& delta_token,
                                      std::optional<uint32_t> max_page_size = std::nullopt);

    /**
     * @brief Execute next page request (for pagination)
     * @param next_url Next page URL from previous response
     * @param change_tracking_confirmed True when the service confirmed Preference-Applied:
     *        odata.track-changes on this extraction's FIRST page. Only the first page carries that
     *        header, so a later page cannot know on its own; without it the DeltaLinksOf token
     *        recovery could never run for a multi-page initial load (GitHub #250).
     * @return Result containing response data
     */
    OdpRequestResult ExecuteNextPage(const std::string& next_url, bool change_tracking_confirmed = false);


    /**
     * @brief Extract delta token from OData response
     * @param response OData entity set response
     * @return Extracted delta token or empty string if not found
     */
    static std::string ExtractDeltaToken(const ODataEntitySetResponse& response);
    /**
     * @brief Extract delta link URL from OData response
     */
    static std::string ExtractDeltaUrl(const ODataEntitySetResponse& response);

    /**
     * @brief Build delta URL by appending delta token to base URL
     * @param base_url Base entity set URL
     * @param delta_token Delta token to append
     * @return Complete delta URL
     */
    static std::string BuildDeltaUrl(const std::string& base_url, const std::string& delta_token);

    /**
     * @brief True when two URLs share scheme, host and effective port
     *
     * Server-supplied next/delta links are followed with the caller's bearer
     * token; a link pointing elsewhere must not carry it (#101). This mirrors
     * the cross-origin rule HttpClient already applies to redirects.
     */
    static bool IsSameOrigin(const std::string& reference_url, const std::string& candidate_url);

    /**
     * @brief Percent-encode a delta token for use in a URL query string
     */
    static std::string EncodeDeltaToken(const std::string& delta_token);

    /**
     * @brief Extract the SAP/OData error code from an error response body
     * @return The code (e.g. "SY/530"), or an empty string when absent
     */
    static std::string ExtractErrorCode(const std::string& response_content);
    /**
     * @brief Normalize delta URL (remove quoted token patterns, ensure $format=json)
     */
    static std::string NormalizeDeltaUrl(const std::string& delta_url);

    /**
     * @brief Set default page size for requests
     * @param page_size Default page size
     */
    void SetDefaultPageSize(uint32_t page_size);

    /**
     * @brief Get current default page size
     * @return Default page size
     */
    uint32_t GetDefaultPageSize() const;

    /**
     * @brief Replace the bounds applied to the HTTP 202 re-poll loop
     */
    void SetRetryPolicy(const RetryPolicy& policy);

    /**
     * @brief Get the bounds currently applied to the HTTP 202 re-poll loop
     */
    const RetryPolicy& GetRetryPolicy() const;

    /**
     * @brief Replace the function used to wait between re-polls (test seam)
     * @param sleep_function Callable invoked with the duration to wait; ignored when empty
     */
    void SetSleepFunction(SleepFunction sleep_function);

private:
    // Core components
    std::unique_ptr<OdpHttpRequestFactory> http_factory_;
    std::shared_ptr<HttpClient> http_client_;
    std::shared_ptr<HttpAuthParams> auth_params_;
    uint32_t default_page_size_;
    // Origin of the service this orchestrator was pointed at; every
    // server-supplied follow-up URL is checked against it before credentials
    // are attached.
    std::string service_origin_url_;
    RetryPolicy retry_policy_;
    SleepFunction sleep_function_;

    /// Send `request`, servicing any HTTP 202 Accepted responses by waiting for the interval the
    /// server asks for and asking again, within the bounds of `retry_policy_`.
    std::unique_ptr<HttpResponse> SendRequestHandlingAccepted(const HttpRequest& request,
                                                             OdpOperation operation);

    // Helper methods
    OdpRequestResult ExecuteRequest(const HttpRequest& request, OdpOperation operation,
                                    bool change_tracking_confirmed = false);

    // Recover the delta token from the service's DeltaLinksOf<EntitySet> entity set.
    //
    // The inline __delta link is not reliably present: SAP returns it on the first read of
    // a freshly generated service and not on later full extractions of the same one, so
    // reading only the response body leaves the subscription with no token and every later
    // read re-extracts everything. The token is always discoverable here. Returns an empty
    // string when the service exposes no such entity set (an ODP without delta support) or
    // the lookup fails - callers stay in initial-load mode, as before. See GitHub #169.
    std::string FetchDeltaTokenFromDeltaLinks(const HttpUrl& entity_set_url);
    std::shared_ptr<ODataEntitySetResponse> ProcessHttpResponse(std::unique_ptr<HttpResponse> http_response);
    void LogRequestDetails(const HttpRequest& request, OdpOperation operation) const;
    void LogResponseDetails(const OdpRequestResult& result, OdpOperation operation) const;
    
public:
    // Static utility methods (public for testing)
    static std::string ExtractDeltaTokenFromV2Response(const std::string& response_content);
    static std::string ExtractDeltaTokenFromV4Response(const std::string& response_content);
    static std::string ExtractTokenFromDeltaUrl(const std::string& delta_url);
    static std::string EnsureJsonFormat(const std::string& url);
    static bool HasJsonFormat(const std::string& url);
    static bool ValidatePreferenceApplied(const HttpResponse& response);

    /**
     * @brief Parse an HTTP `Retry-After` header value (RFC 7231 section 7.1.3)
     *
     * Accepts both permitted forms: delta-seconds (`"120"`) and an HTTP-date
     * (`"Wed, 21 Oct 2015 07:28:00 GMT"`, plus the two obsolete date formats recipients are
     * required to tolerate). A date already in the past yields a zero wait.
     *
     * @param header_value Raw header value
     * @param now Reference instant against which an HTTP-date is measured
     * @return The wait requested, or std::nullopt when the value cannot be parsed
     */
    static std::optional<std::chrono::milliseconds> ParseRetryAfter(
        const std::string& header_value,
        std::chrono::system_clock::time_point now);

    // Strip surrounding single/double quotes from a raw delta token if present.
    static std::string NormalizeDeltaToken(const std::string& raw);
};

} // namespace erpl_web
