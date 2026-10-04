#pragma once

#include "odata_read_functions.hpp"
#include "odp_subscription_state_manager.hpp"
#include "odp_request_orchestrator.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "tracing.hpp"
#include <memory>
#include <string>

namespace erpl_web {

/**
 * @brief ODP-aware bind data class that extends OData functionality with delta replication
 * 
 * This class uses composition to delegate core OData operations to ODataReadBindData
 * while adding ODP-specific functionality for subscription management and delta token handling.
 * It coordinates between the state manager and request orchestrator to provide seamless
 * ODP delta replication capabilities.
 */
// Drains empty pages until rows arrive or the budget is spent.
//
// Extracted so the FAILURE path can be tested at all. Reaching it through the reader needs
// a live ODP service with an open delta queue, so nothing exercised the branch that decides
// between "keep draining" and "abort"; with the loop inline, restoring the old break -
// which yielded zero rows and let the caller commit the delta token over rows never
// delivered - would have failed no test.
//
// `fetch_rows` returns the rows a fetch produced; `has_next_page` reports whether the
// service is still advertising one; `fetch_next_page` advances. Throws IOException when the
// budget is spent with a page still pending: zero rows is how a DuckDB table function says
// end-of-scan, so yielding here would report a partial extraction as complete.
template <class FetchRows, class HasNextPage, class FetchNextPage>
unsigned int DrainEmptyOdpPages(FetchRows &&fetch_rows, HasNextPage &&has_next_page,
                                FetchNextPage &&fetch_next_page, unsigned int max_empty_pages,
                                const std::string &failure_detail) {
    unsigned int rows_fetched = fetch_rows();
    unsigned int empty_pages = 0;
    while (rows_fetched == 0 && has_next_page()) {
        if (++empty_pages > max_empty_pages) {
            throw duckdb::IOException(
                "The ODP service returned " + std::to_string(max_empty_pages) +
                " consecutive empty pages while still advertising another page. Aborting "
                "rather than reporting a partial extraction as complete. No rows from this "
                "run have been committed and the delta token was not advanced, so the "
                "subscription still points at the last position it completed and the next "
                "run resumes from there." +
                failure_detail);
        }
        fetch_next_page();
        rows_fetched = fetch_rows();
    }
    return rows_fetched;
}

class OdpODataReadBindData : public duckdb::TableFunctionData {
public:
    /**
     * @brief Construct ODP bind data with subscription parameters
     * @param context DuckDB client context
     * @param entity_set_url Full ODP entity set URL
     * @param secret_name Secret name for authentication (default: "default")
     * @param force_full_load Force initial load even if subscription exists
     * @param import_delta_token Import existing delta token
     * @param max_page_size Optional page size override
     */
    OdpODataReadBindData(duckdb::ClientContext& context,
                        const std::string& entity_set_url,
                        const std::string& secret_name = "",
                        bool force_full_load = false,
                        const std::string& import_delta_token = "",
                        std::optional<uint32_t> max_page_size = std::nullopt);

    ~OdpODataReadBindData() = default;

    // Non-copyable, non-movable
    OdpODataReadBindData(const OdpODataReadBindData&) = delete;
    OdpODataReadBindData& operator=(const OdpODataReadBindData&) = delete;
    OdpODataReadBindData(OdpODataReadBindData&&) = delete;
    OdpODataReadBindData& operator=(OdpODataReadBindData&&) = delete;

    // ========================================================================
    // Core DuckDB Table Function Interface (delegated to ODataReadBindData)
    // ========================================================================

    /**
     * @brief Get result column names
     * @param all_columns Include all columns or just selected ones
     * @return Vector of column names
     */
    std::vector<std::string> GetResultNames(bool all_columns = false);

    /**
     * @brief Get result column types
     * @param all_columns Include all columns or just selected ones
     * @return Vector of logical types
     */
    std::vector<duckdb::LogicalType> GetResultTypes(bool all_columns = false);

    /**
     * @brief Check if more results are available
     * @return True if more data can be fetched
     */
    bool HasMoreResults();

    /**
     * @brief Fetch next batch of results
     * @param output Output data chunk to fill
     * @return Number of rows fetched
     */
    unsigned int FetchNextResult(duckdb::DataChunk &output);

    /**
     * @brief Activate specific columns for projection pushdown
     * @param column_ids Column indices to activate
     */
    void ActivateColumns(const std::vector<duckdb::column_t> &column_ids);

    /**
     * @brief Add filters for predicate pushdown
     * @param filters Table filter set
     */
    void AddFilters(const duckdb::optional_ptr<duckdb::TableFilterSet> &filters);

    /**
     * @brief Add result modifiers (ORDER BY, LIMIT, etc.)
     * @param modifiers Result modifier list
     */
    void AddResultModifiers(const std::vector<duckdb::unique_ptr<duckdb::BoundResultModifier>> &modifiers);

    /**
     * @brief Get progress fraction for query execution
     * @return Progress value between 0.0 and 1.0
     */
    double GetProgressFraction() const;

    // ========================================================================
    // ODP-Specific Interface
    // ========================================================================

    /**
     * @brief Get subscription ID
     * @return Current subscription ID
     */
    std::string GetSubscriptionId() const;

    /**
     * @brief Get current delta token
     * @return Current delta token or empty string
     */
    std::string GetCurrentDeltaToken() const;

    /**
     * @brief Check if subscription is active
     * @return True if subscription is active and functional
     */
    bool IsSubscriptionActive() const;

    /**
     * @brief Get current subscription phase
     * @return Current phase (INITIAL_LOAD, DELTA_FETCH, etc.)
     */
    OdpSubscriptionStateManager::SubscriptionPhase GetCurrentPhase() const;

    /**
     * @brief Get audit history for this subscription
     * @param days_back Number of days to look back (default: 30)
     * @return Vector of audit entries
     */
    std::vector<OdpAuditEntry> GetAuditHistory(int days_back = 30) const;
    
    /**
     * @brief Get the underlying OData bind data for delegation
     * @return Reference to the composed ODataReadBindData
     */
    ODataReadBindData& GetODataBindData();
    const ODataReadBindData& GetODataBindData() const;
    
    /**
     * @brief Process scan results for ODP-specific logic
     * 
     * Called after each successful scan to handle delta token updates,
     * audit logging, and subscription state management.
     * 
     * @param output The data chunk that was just scanned
     */
    void ProcessScanResult(const duckdb::DataChunk& output);

    /**
     * @brief Initialize all components and establish subscription
     */
    void Initialize();

    /**
     * @brief Build a private copy of this bind data for one execution of a bound plan.
     *
     * All mutable scan state - the inner ODataReadBindData's row buffer and pagination
     * cursor, the audit id, the staged delta token - lives on the instance, so sharing one
     * between executions makes the second EXECUTE of a prepared statement find the buffer
     * already drained and return nothing (GitHub #146, the #75 class).
     *
     * The clone re-runs Initialize(), which re-resolves the SAME subscription rather than
     * creating a second one: (service_url, entity_set_name) is unique in the repository and
     * CreateSubscription returns the existing id for an active row. The audit id and the
     * delta-token staging slot start empty, which is what they must be for a new execution.
     */
    duckdb::unique_ptr<OdpODataReadBindData> CloneForScan() const;

    /**
     * @brief Called when the scan is finished, however it finished.
     *
     * Commits a staged delta token. The scan can end WITHOUT a final zero-row
     * FetchNextResult - OdpODataReadScan returns early when HasMoreResults() is already
     * false - so relying on the fetch path alone means a normally drained scan never
     * advances the subscription and every read re-extracts from the old token.
     * Idempotent, so it is safe to call from both exits.
     */
    void FinalizeScan();

private:
    void WriteAuditTotals();

public:

private:
    // ========================================================================
    // Core Components (Composition Pattern)
    // ========================================================================

    // Delegate to existing OData functionality
    std::unique_ptr<ODataReadBindData> odata_bind_data_;
    
    // ODP-specific components
    std::unique_ptr<OdpSubscriptionStateManager> state_manager_;
    std::unique_ptr<OdpRequestOrchestrator> request_orchestrator_;
    
    // Configuration
    duckdb::ClientContext& context_;
    std::string entity_set_url_;
    std::string secret_name_;
    std::shared_ptr<HttpAuthParams> auth_params_;
    std::optional<uint32_t> max_page_size_;
    bool force_full_load_;
    std::string import_delta_token_;
    
    // State tracking
    bool initialized_;
    bool first_fetch_completed_;
    // Why the last initial load or delta fetch returned false. The handlers catch the
    // failure so they can transition the subscription to ERROR_STATE, which means the
    // service's own explanation would otherwise be lost by the time the caller has to
    // raise an error (GitHub #173).
    std::string last_fetch_error_;
    int64_t current_audit_id_;

    // Totals for the audit row, accumulated across the whole extraction and written once.
    // The audit UPDATE assigns rather than adds, so writing per chunk left the row holding
    // the LAST chunk's count instead of the total - useless for the one thing the audit
    // table exists for: checking that a package was fully delivered. See GitHub #158.
    int64_t audit_rows_fetched_ = 0;
    int64_t audit_package_size_bytes_ = 0;
    bool audit_written_ = false;

    // Incremental pagination state
    // When a multi-page response is being consumed, pending_next_url_ holds the
    // __next URL of the page currently loaded into odata_bind_data_. It is cleared
    // when the last page is reached.
    std::string pending_next_url_;
    bool initial_load_in_progress_;
    bool delta_fetch_in_progress_;
    /// Whether the FIRST response of the current initial load carried
    /// `Preference-Applied: odata.track-changes`. Only that response answers the `Prefer` header we
    /// sent; the follow-on `__next` page requests do not carry the preference and so cannot
    /// re-confirm it. Captured here so the deferred multi-page state transition uses the server's
    /// actual answer rather than re-inferring it. See GitHub #97.
    bool initial_load_preference_applied_ = false;

    // Column projection: saved in ActivateColumns and re-applied to each replacement
    // odata_bind_data_ created by FetchAndLoadNextPage, so column mapping is consistent
    // across all pages.
    std::vector<duckdb::column_t> active_column_ids_;

    // Delta token staging. SAP's ODQ discards a delta package once its token has been
    // acknowledged, so advancing the subscription before the rows have actually reached
    // DuckDB loses them permanently: a cancelled query, a failed page or a crash between
    // the two would skip that package forever. The token extracted from the final page is
    // therefore held here and only written to the repository once the scan has drained.
    // See GitHub #62.
    std::string staged_delta_token_;
    OdpOperation staged_operation_ = OdpOperation::INITIAL_LOAD;
    bool staged_preference_applied_ = false;
    bool has_staged_delta_token_ = false;

    /**
     * @brief Commit a staged delta token once every row has been handed to DuckDB.
     *
     * Called when the scan reports exhaustion. A no-op when nothing is staged, so it is
     * safe to call on every drained fetch.
     */
    void CommitStagedDeltaToken();

    // ========================================================================
    // Initialization and Setup
    // ========================================================================

    /**
     * @brief Setup authentication parameters from secret
     */
    void SetupAuthentication();

    /**
     * @brief Create and configure the underlying ODataReadBindData
     */
    void CreateODataBindData();

    /**
     * @brief Validate the entity set URL for ODP compatibility
     */
    void ValidateEntitySetUrl() const;

    // ========================================================================
    // ODP Request Handling
    // ========================================================================

    /**
     * @brief Handle initial load request with change tracking
     * @return True if successful, false otherwise
     */
    bool HandleInitialLoad();

    /**
     * @brief Handle delta fetch request using current token
     * @return True if successful, false otherwise
     */
    bool HandleDeltaFetch();

    /**
     * @brief Process request result and update subscription state
     * @param result Request result from orchestrator
     * @param operation Type of operation performed
     */
    void ProcessRequestResult(const OdpRequestOrchestrator::OdpRequestResult& result, OdpOperation operation);


    /**
     * @brief Update OData client with pre-fetched response content
     * @param url URL that was used to fetch the response
     * @param response_content Pre-fetched JSON response content
     */
    void UpdateODataClientWithResponse(const std::string& url, const std::string& response_content);

    /**
     * @brief Fetch the next ODP page and reload odata_bind_data_ with its content.
     *
     * Called from FetchNextResult() when the current page is exhausted and
     * pending_next_url_ is non-empty. On the last page (no further __next),
     * the deferred state transition (delta token save) is performed here.
     */
    void FetchAndLoadNextPage();

    // ========================================================================
    // Error Handling and Recovery
    // ========================================================================

    /**
     * @brief Handle request errors and determine recovery strategy
     * @param error Exception that occurred
     * @param operation_type Type of operation that failed
     */
    void HandleRequestError(const std::exception& error, const std::string& operation_type);

    /**
     * @brief Check if error indicates expired/invalid delta token
     * @param error Exception to analyze
     * @return True if token-related error
     */
    bool IsTokenError(const std::exception& error) const;

    // ========================================================================
    // Utility Methods
    // ========================================================================

    /**
     * @brief Log current state for debugging
     */
    void LogCurrentState() const;

    /**
     * @brief Extract entity set name from URL
     * @param url Full entity set URL
     * @return Entity set name
     */
    static std::string ExtractEntitySetName(const std::string& url);
};

} // namespace erpl_web
