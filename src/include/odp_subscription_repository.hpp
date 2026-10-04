#pragma once

#include "duckdb.hpp"
#include "tracing.hpp"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace erpl_web {

// Data structures for ODP subscription management
struct OdpSubscription {
    std::string subscription_id;
    std::string service_url;
    std::string entity_set_name;
    std::string secret_name;
    std::string delta_token;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_updated;
    std::string subscription_status; // active, terminated, expired, error
    bool preference_applied;

    OdpSubscription() = default;
    OdpSubscription(const std::string& service_url, const std::string& entity_set_name,
                   const std::string& secret_name = "");
};

struct OdpAuditEntry {
    int64_t audit_id; // Allocated from a database SEQUENCE
    std::string subscription_id;
    std::string operation_type; // initial_load, delta_fetch, terminate
    std::chrono::system_clock::time_point request_timestamp;
    std::optional<std::chrono::system_clock::time_point> response_timestamp;
    std::string request_url;
    std::optional<int> http_status_code;
    int64_t rows_fetched;
    int64_t package_size_bytes;
    std::string delta_token_before;
    std::string delta_token_after;
    std::string error_message;
    std::optional<int64_t> duration_ms;

    OdpAuditEntry() : audit_id(0), rows_fetched(0), package_size_bytes(0) {}
    OdpAuditEntry(const std::string& subscription_id, const std::string& operation_type);
};

// Repository class for managing ODP subscriptions and audit data.
//
// All statements are prepared with bound parameters -- service URLs, entity set
// names and delta tokens are attacker-influenced values that must never be
// interpolated into SQL text (GitHub #99).
//
// All objects are qualified with a state catalog that is resolved once, at
// construction, to a persistent DuckDB catalog. Without that, delta state lands
// in whatever catalog happens to be default at query time -- an in-memory
// database that discards the tokens at exit, or a foreign attached database that
// the service URLs and secret names get exported into (GitHub #92).
class OdpSubscriptionRepository {
public:
    // Layout version of the erpl_web ODP tables. Bumped whenever the columns
    // change so an older layout is detected rather than silently misread.
    static constexpr int32_t SCHEMA_VERSION = 2;

    // Upper bound on an error body persisted into the audit table. A full SAP
    // error body can carry request context and is not worth keeping verbatim.
    static constexpr size_t MAX_AUDIT_ERROR_LENGTH = 1000;

    explicit OdpSubscriptionRepository(duckdb::ClientContext& context);
    ~OdpSubscriptionRepository() = default;

    // Non-copyable, non-movable
    OdpSubscriptionRepository(const OdpSubscriptionRepository&) = delete;
    OdpSubscriptionRepository& operator=(const OdpSubscriptionRepository&) = delete;
    OdpSubscriptionRepository(OdpSubscriptionRepository&&) = delete;
    OdpSubscriptionRepository& operator=(OdpSubscriptionRepository&&) = delete;

    // Schema management
    void EnsureSchemaExists();
    void EnsureTablesExist();

    // Name of the catalog the ODP state tables live in. Resolved lazily on the
    // first schema access and stable for the lifetime of the repository.
    const std::string& GetStateCatalog();

    // Subscription management
    std::string CreateSubscription(const std::string& service_url,
                                 const std::string& entity_set_name,
                                 const std::string& secret_name = "");
    std::optional<OdpSubscription> GetSubscription(const std::string& subscription_id);
    // Returns the active subscription for the pair, or nullopt when there is
    // none. A query failure throws rather than being reported as "not found" --
    // reporting it as not-found triggers a silent full re-extraction (#96).
    std::optional<OdpSubscription> FindActiveSubscription(const std::string& service_url,
                                                        const std::string& entity_set_name,
                                                        const std::string& secret_name);
    std::vector<OdpSubscription> ListAllSubscriptions();

    // Compare-and-swap advance of the delta token. The update only applies when
    // the stored token still equals expected_delta_token; a concurrent reader
    // that advanced the stream first makes this throw instead of silently
    // splitting the delta stream (#95).
    void AdvanceDeltaToken(const std::string& subscription_id,
                           const std::string& expected_delta_token,
                           const std::string& new_delta_token);
    bool UpdateSubscriptionStatus(const std::string& subscription_id, const std::string& status);
    // Forgets the stored delta position (token and Preference-Applied) so the next read is a full load.
    bool ResetDeltaPosition(const std::string& subscription_id);
    bool RemoveSubscription(const std::string& subscription_id);

    // Audit management
    int64_t CreateAuditEntry(const OdpAuditEntry& entry);
    bool UpdateAuditEntry(const OdpAuditEntry& entry);

    // Update ONLY the delivered totals. UpdateAuditEntry assigns every column, so using it
    // to record what a scan delivered blanks the delta token, status and error the page
    // boundaries had already written. See GitHub #158.
    bool UpdateAuditTotals(int64_t audit_id, int64_t rows_fetched, int64_t package_size_bytes);

    // Utility methods
    static std::string GenerateSubscriptionId(const std::string& service_url,
                                             const std::string& entity_set_name,
                                             const std::string& secret_name);
    static std::string CleanUrlForId(const std::string& url);
    static std::string SubscriptionsTableBody();
    // True when the path of the URL looks like an ODP entity set (EntityOf*,
    // FactsOf*, AttrOf*). The query string is stripped before matching -- it
    // regularly carries $format/$select and made valid URLs fail the check.
    static bool IsValidOdpUrl(const std::string& url);
    // Truncates an error body to MAX_AUDIT_ERROR_LENGTH, appending an elision
    // marker when it had to cut.
    static std::string TruncateForAudit(const std::string& message);

private:
    duckdb::ClientContext& context;
    std::string state_catalog;
    std::string qualified_schema; // "catalog"."erpl_web"
    bool schema_initialized;
    bool tables_initialized;

    // Helper methods
    void ResolveStateCatalog();
    std::string QualifiedTable(const std::string& table_name) const;
    void InitializeSchema();
    // Rewrites a v1 table (keyed on service_url + entity_set_name) into the v2 layout,
    // which adds secret_name to the unique key. Preserves delta tokens (#236).
    void MigrateSubscriptionsToV2();
    void InitializeTables();
    void VerifySchemaVersion();

    // Executes a statement with bound parameters. Throws on any failure; the
    // SQL text is a fixed template so it carries no caller data.
    duckdb::unique_ptr<duckdb::MaterializedQueryResult> Execute(const std::string& sql,
                                                                duckdb::vector<duckdb::Value> params);
    static int64_t RowsChanged(duckdb::MaterializedQueryResult& result);
    static duckdb::Value TimePointToValue(const std::chrono::system_clock::time_point& tp);
    static std::chrono::system_clock::time_point ValueToTimePoint(const duckdb::Value& value);
    static OdpSubscription RowToSubscription(duckdb::MaterializedQueryResult& result, duckdb::idx_t row);
};

} // namespace erpl_web
