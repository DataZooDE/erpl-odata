#include "odp_subscription_repository.hpp"
#include "tracing.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace erpl_web {

namespace {

// Deliberately still "erpl_web": this is the on-disk schema that existing users' ODP subscription and delta-token
// state lives in. Renaming the extension to erpl_odata must not orphan those queues, so the identifier is frozen.
constexpr const char* SCHEMA_NAME = "erpl_web";
constexpr const char* SUBSCRIPTIONS_TABLE = "odp_subscriptions";
constexpr const char* AUDIT_TABLE = "odp_subscription_audit";
constexpr const char* AUDIT_SEQUENCE = "odp_audit_id_seq";

// The full projection of a subscription row; every read uses it so the column
// indices below stay in one place.
constexpr const char* SUBSCRIPTION_COLUMNS =
    "subscription_id, service_url, entity_set_name, secret_name, delta_token, "
    "created_at, last_updated, subscription_status, preference_applied, schema_version";

enum SubscriptionColumn : duckdb::idx_t {
    COL_SUBSCRIPTION_ID = 0,
    COL_SERVICE_URL = 1,
    COL_ENTITY_SET_NAME = 2,
    COL_SECRET_NAME = 3,
    COL_DELTA_TOKEN = 4,
    COL_CREATED_AT = 5,
    COL_LAST_UPDATED = 6,
    COL_SUBSCRIPTION_STATUS = 7,
    COL_PREFERENCE_APPLIED = 8,
    COL_SCHEMA_VERSION = 9
};

std::string QuoteIdentifier(const std::string& name) {
    return "\"" + duckdb::StringUtil::Replace(name, "\"", "\"\"") + "\"";
}

// nextval() needs a constant sequence name, so it cannot take a bound
// parameter; the qualified name goes into a SQL string literal instead.
std::string QuoteStringLiteral(const std::string& value) {
    return "'" + duckdb::StringUtil::Replace(value, "'", "''") + "'";
}

// std::localtime is not reentrant and this runs on DuckDB worker threads.
std::tm ToLocalTm(std::time_t value) {
    std::tm out {};
#ifdef _WIN32
    localtime_s(&out, &value);
#else
    localtime_r(&value, &out);
#endif
    return out;
}

// How long a session that lost the subscription INSERT race waits for the winner to commit.
constexpr idx_t RACE_LOOKUP_ATTEMPTS = 40;
constexpr std::chrono::milliseconds RACE_LOOKUP_INTERVAL{25};

bool IsUsableStateCatalog(duckdb::AttachedDatabase& database) {
    if (database.IsSystem() || database.IsTemporary()) {
        return false;
    }
    // A read-only catalog cannot hold ODP state: CREATE SCHEMA fails there, and every later query in the
    // session used to fail with "database has been invalidated" (GitHub #251).
    if (database.IsReadOnly()) {
        return false;
    }
    auto& catalog = database.GetCatalog();
    return catalog.IsDuckCatalog() && !catalog.InMemory();
}

} // namespace

// ============================================================================
// OdpSubscription Implementation
// ============================================================================

OdpSubscription::OdpSubscription(const std::string& service_url,
                               const std::string& entity_set_name,
                               const std::string& secret_name)
    : service_url(service_url)
    , entity_set_name(entity_set_name)
    , secret_name(secret_name.empty() ? "default" : secret_name)
    , created_at(std::chrono::system_clock::now())
    , last_updated(std::chrono::system_clock::now())
    , subscription_status("active")
    , preference_applied(false)
{
    subscription_id = OdpSubscriptionRepository::GenerateSubscriptionId(service_url, entity_set_name,
                                                                        this->secret_name);
}

// ============================================================================
// OdpAuditEntry Implementation
// ============================================================================

OdpAuditEntry::OdpAuditEntry(const std::string& subscription_id, const std::string& operation_type)
    : audit_id(0) // Allocated from the audit sequence on insert
    , subscription_id(subscription_id)
    , operation_type(operation_type)
    , request_timestamp(std::chrono::system_clock::now())
    , rows_fetched(0)
    , package_size_bytes(0)
{
}

// ============================================================================
// OdpSubscriptionRepository Implementation
// ============================================================================

OdpSubscriptionRepository::OdpSubscriptionRepository(duckdb::ClientContext& context)
    : context(context)
    , schema_initialized(false)
    , tables_initialized(false)
{
    ERPL_TRACE_INFO("ODP_REPOSITORY", "OdpSubscriptionRepository initialized");
}

const std::string& OdpSubscriptionRepository::GetStateCatalog() {
    ResolveStateCatalog();
    return state_catalog;
}

void OdpSubscriptionRepository::ResolveStateCatalog() {
    if (!state_catalog.empty()) {
        return;
    }

    // Enumerating attached catalogs reads through the client's transaction context, so a
    // transaction has to be active. When the repository is driven from a table function
    // there already is one - and RunFunctionInTransaction would DEADLOCK there, because it
    // takes the client-context lock the running query already holds. So the caller's
    // transaction is used when it exists, and one is started only for standalone callers
    // such as unit tests.
    std::string default_name;
    duckdb::vector<std::string> read_only_catalogs;
    const auto resolve = [&]() {
    auto& database_manager = duckdb::DatabaseManager::Get(context);

    // Prefer the catalog that is currently default, so an explicit USE keeps
    // working -- but only when it can actually hold the state durably.
    default_name = duckdb::DatabaseManager::GetDefaultDatabase(context);
    auto default_database = database_manager.GetDatabase(context, default_name);
    if (default_database && IsUsableStateCatalog(*default_database)) {
        state_catalog = default_database->GetName();
    } else {
        // Otherwise take the first persistent DuckDB catalog, by name, so the
        // choice does not depend on hash-map iteration order.
        std::vector<std::string> candidates;
        for (auto& database : database_manager.GetDatabases(context)) {
            if (database && IsUsableStateCatalog(*database)) {
                candidates.push_back(database->GetName());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        if (!candidates.empty()) {
            state_catalog = candidates.front();
        }
    }

    // Remembered only to explain a failure: a persistent catalog that was skipped because it is read-only.
    for (auto& database : database_manager.GetDatabases(context)) {
        if (database && database->IsReadOnly() && !database->IsSystem() && !database->IsTemporary() &&
            database->GetCatalog().IsDuckCatalog() && !database->GetCatalog().InMemory()) {
            read_only_catalogs.push_back(database->GetName());
        }
    }
    std::sort(read_only_catalogs.begin(), read_only_catalogs.end());
    };

    if (context.transaction.HasActiveTransaction()) {
        resolve();
    } else {
        context.RunFunctionInTransaction(resolve);
    }

    if (state_catalog.empty()) {
        throw duckdb::InvalidInputException(
            "ODP delta state requires a persistent DuckDB database, but this session has none "
            "attached (the current catalog '" + default_name + "' is in-memory or not a DuckDB "
            "catalog). Delta tokens stored there are discarded when the session ends, so every "
            "read would silently become a full extraction. Start DuckDB with a database file "
            "(for example `duckdb odp_state.db`), or ATTACH one before reading ODP sources "
            "(for example ATTACH 'odp_state.db' AS odp_state)." +
            (read_only_catalogs.empty()
                 ? std::string()
                 : " Read-only catalogs cannot hold state and were ignored: " +
                       duckdb::StringUtil::Join(read_only_catalogs, ", ") + "."));
    }

    qualified_schema = QuoteIdentifier(state_catalog) + "." + QuoteIdentifier(SCHEMA_NAME);
    ERPL_TRACE_INFO("ODP_REPOSITORY", "Resolved ODP state catalog: " + state_catalog);
}

std::string OdpSubscriptionRepository::QualifiedTable(const std::string& table_name) const {
    return qualified_schema + "." + QuoteIdentifier(table_name);
}

void OdpSubscriptionRepository::EnsureSchemaExists() {
    if (schema_initialized) {
        return;
    }

    ResolveStateCatalog();
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Ensuring erpl_web schema exists in catalog " + state_catalog);

    InitializeSchema();
    schema_initialized = true;
    ERPL_TRACE_INFO("ODP_REPOSITORY", "Schema erpl_web initialized successfully");
}

void OdpSubscriptionRepository::EnsureTablesExist() {
    if (tables_initialized) {
        return;
    }

    EnsureSchemaExists();
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Ensuring ODP tables exist");

    MigrateSubscriptionsToV2();
    InitializeTables();
    VerifySchemaVersion();
    tables_initialized = true;
    ERPL_TRACE_INFO("ODP_REPOSITORY", "ODP tables initialized successfully");
}

std::string OdpSubscriptionRepository::CreateSubscription(const std::string& service_url,
                                                        const std::string& entity_set_name,
                                                        const std::string& secret_name) {
    ERPL_TRACE_INFO("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "Creating subscription for URL: %s, Entity: %s, Secret: %s",
        service_url, entity_set_name, secret_name));

    EnsureTablesExist();

    if (!IsValidOdpUrl(service_url)) {
        // A URL that does not look like an ODP entity set is suspicious but not
        // provably wrong -- the naming convention is a convention, not a rule.
        ERPL_TRACE_WARN("ODP_REPOSITORY",
            "Service URL does not look like an ODP entity set (expected EntityOf*/FactsOf*/AttrOf*): " + service_url);
    }

    // (service_url, entity_set_name, secret_name) is unique, so a stale non-active row
    // for the same triple is revived rather than duplicated. Switching to a different
    // secret finds no row here and creates a new subscription with no delta token, which
    // is a full initial load -- the honest outcome, since the old position was
    // established under credentials this caller is no longer using (#236).
    const std::string effective_secret = secret_name.empty() ? "default" : secret_name;
    auto existing = Execute(
        "SELECT " + std::string(SUBSCRIPTION_COLUMNS) + " FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
        " WHERE service_url = ? AND entity_set_name = ? AND secret_name = ?",
        {duckdb::Value(service_url), duckdb::Value(entity_set_name), duckdb::Value(effective_secret)});

    if (existing->RowCount() > 0) {
        auto subscription = RowToSubscription(*existing, 0);
        if (subscription.subscription_status == "active") {
            ERPL_TRACE_WARN("ODP_REPOSITORY", "Active subscription already exists: " + subscription.subscription_id);
            return subscription.subscription_id;
        }

        // Whether the delta position survives reactivation depends on why the subscription
        // stopped being active (#236).
        //
        // 'error' means an extraction failed for a reason that does NOT invalidate the
        // position. A failure that genuinely invalidates it -- a 410 on the delta link, or
        // SAP reporting DELTATOKEN -- never reaches this status at all: the reader
        // recognises it (OdpODataReadBindData::IsTokenError), clears the token itself and
        // goes back to initial load. So the stored token here is one the service would
        // still honour, and discarding it turns a transient network blip into a full
        // re-extraction of an SAP source.
        //
        // 'terminated' is different and must still clear it: the ODQ queue behind the
        // subscription is gone, so resuming from its token would silently skip rows.
        const bool position_survives = subscription.subscription_status == "error";

        ERPL_TRACE_INFO("ODP_REPOSITORY", duckdb::StringUtil::Format(
            "Reactivating subscription %s (was '%s'); delta position is %s",
            subscription.subscription_id, subscription.subscription_status,
            position_survives ? "preserved" : "discarded"));

        if (position_survives) {
            Execute("UPDATE " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
                    " SET subscription_status = 'active', secret_name = ?, last_updated = ? "
                    "WHERE subscription_id = ?",
                    {duckdb::Value(effective_secret),
                     TimePointToValue(std::chrono::system_clock::now()),
                     duckdb::Value(subscription.subscription_id)});
        } else {
            Execute("UPDATE " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
                    " SET subscription_status = 'active', delta_token = '', preference_applied = FALSE, "
                    "secret_name = ?, last_updated = ? WHERE subscription_id = ?",
                    {duckdb::Value(effective_secret),
                     TimePointToValue(std::chrono::system_clock::now()),
                     duckdb::Value(subscription.subscription_id)});
        }
        return subscription.subscription_id;
    }

    OdpSubscription subscription(service_url, entity_set_name, secret_name);
    try {
        Execute("INSERT INTO " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " (" + SUBSCRIPTION_COLUMNS + ") "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {duckdb::Value(subscription.subscription_id),
                 duckdb::Value(subscription.service_url),
                 duckdb::Value(subscription.entity_set_name),
                 duckdb::Value(subscription.secret_name),
                 duckdb::Value(subscription.delta_token),
                 TimePointToValue(subscription.created_at),
                 TimePointToValue(subscription.last_updated),
                 duckdb::Value(subscription.subscription_status),
                 duckdb::Value::BOOLEAN(subscription.preference_applied),
                 duckdb::Value::INTEGER(SCHEMA_VERSION)});
    } catch (const std::exception& insert_error) {
        // The SELECT above and this INSERT are separate statements, so a second session reading the
        // same (service_url, entity_set_name, secret_name) for the first time passes the same check and
        // loses on the UNIQUE/PRIMARY KEY constraint. The winner's row is not visible until its
        // transaction commits, so look for it for a short, bounded time; if it shows up that session won
        // and its row is adopted instead of failing the read. If it never does, the INSERT failed for
        // some other reason and that is what is reported (GitHub #251).
        for (idx_t attempt = 0; attempt < RACE_LOOKUP_ATTEMPTS; attempt++) {
            auto raced = Execute(
                "SELECT " + std::string(SUBSCRIPTION_COLUMNS) + " FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
                " WHERE service_url = ? AND entity_set_name = ? AND secret_name = ?",
                {duckdb::Value(service_url), duckdb::Value(entity_set_name), duckdb::Value(effective_secret)});
            if (raced->RowCount() > 0) {
                ERPL_TRACE_INFO("ODP_REPOSITORY", "Lost a race creating the subscription, adopting the winner: " +
                                                      std::string(insert_error.what()));
                return RowToSubscription(*raced, 0).subscription_id;
            }
            std::this_thread::sleep_for(RACE_LOOKUP_INTERVAL);
        }
        throw;
    }

    ERPL_TRACE_INFO("ODP_REPOSITORY", "Subscription created successfully: " + subscription.subscription_id);
    return subscription.subscription_id;
}

std::optional<OdpSubscription> OdpSubscriptionRepository::GetSubscription(const std::string& subscription_id) {
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Getting subscription: " + subscription_id);

    EnsureTablesExist();

    auto result = Execute(
        "SELECT " + std::string(SUBSCRIPTION_COLUMNS) + " FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
        " WHERE subscription_id = ?",
        {duckdb::Value(subscription_id)});

    if (result->RowCount() == 0) {
        ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Subscription not found: " + subscription_id);
        return std::nullopt;
    }

    return RowToSubscription(*result, 0);
}

std::optional<OdpSubscription> OdpSubscriptionRepository::FindActiveSubscription(
    const std::string& service_url, const std::string& entity_set_name, const std::string& secret_name) {

    const std::string effective_secret = secret_name.empty() ? "default" : secret_name;

    ERPL_TRACE_DEBUG("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "Finding active subscription for URL: %s, Entity: %s, Secret: %s",
        service_url, entity_set_name, effective_secret));

    EnsureTablesExist();

    // Note the absence of a try/catch: a failing query must surface, not be
    // reported as "no subscription" -- that answer silently restarts the whole
    // extraction from scratch (#96).
    auto result = Execute(
        "SELECT " + std::string(SUBSCRIPTION_COLUMNS) + " FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
        " WHERE service_url = ? AND entity_set_name = ? AND secret_name = ? "
        "AND subscription_status = 'active' "
        "ORDER BY created_at DESC LIMIT 1",
        {duckdb::Value(service_url), duckdb::Value(entity_set_name), duckdb::Value(effective_secret)});

    if (result->RowCount() == 0) {
        return std::nullopt;
    }

    auto subscription = RowToSubscription(*result, 0);
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Active subscription found: " + subscription.subscription_id);
    return subscription;
}

std::vector<OdpSubscription> OdpSubscriptionRepository::ListAllSubscriptions() {
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Listing all subscriptions");

    EnsureTablesExist();

    auto result = Execute(
        "SELECT " + std::string(SUBSCRIPTION_COLUMNS) + " FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
        " ORDER BY created_at DESC", {});

    std::vector<OdpSubscription> subscriptions;
    subscriptions.reserve(result->RowCount());
    for (duckdb::idx_t i = 0; i < result->RowCount(); i++) {
        subscriptions.push_back(RowToSubscription(*result, i));
    }

    ERPL_TRACE_INFO("ODP_REPOSITORY", duckdb::StringUtil::Format("Found %d subscriptions", subscriptions.size()));
    return subscriptions;
}

void OdpSubscriptionRepository::AdvanceDeltaToken(const std::string& subscription_id,
                                                  const std::string& expected_delta_token,
                                                  const std::string& new_delta_token) {
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Advancing delta token for subscription " + subscription_id);

    EnsureTablesExist();

    auto result = Execute(
        "UPDATE " + QualifiedTable(SUBSCRIPTIONS_TABLE) +
        " SET delta_token = ?, last_updated = ? "
        "WHERE subscription_id = ? AND delta_token IS NOT DISTINCT FROM ?",
        {duckdb::Value(new_delta_token),
         TimePointToValue(std::chrono::system_clock::now()),
         duckdb::Value(subscription_id),
         duckdb::Value(expected_delta_token)});

    if (RowsChanged(*result) > 0) {
        ERPL_TRACE_INFO("ODP_REPOSITORY", "Delta token advanced for subscription " + subscription_id);
        return;
    }

    // Nothing changed: either the subscription is gone, or another session
    // advanced the same delta stream while this one was fetching.
    auto current = Execute(
        "SELECT delta_token FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " WHERE subscription_id = ?",
        {duckdb::Value(subscription_id)});

    if (current->RowCount() == 0) {
        throw duckdb::InvalidInputException(
            "ODP subscription '" + subscription_id + "' no longer exists; its delta token could not be advanced.");
    }

    throw duckdb::TransactionException(
        "Concurrent modification of ODP subscription '" + subscription_id + "': the stored delta token changed "
        "while this read was running, so another session is reading the same delta stream. Continuing would split "
        "the change stream between the two readers. Re-run the query once the other reader has finished.");
}

bool OdpSubscriptionRepository::UpdateSubscriptionStatus(const std::string& subscription_id,
                                                       const std::string& status) {
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "Updating subscription status for %s: %s", subscription_id, status));

    EnsureTablesExist();

    auto result = Execute(
        "UPDATE " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " SET subscription_status = ?, last_updated = ? "
        "WHERE subscription_id = ?",
        {duckdb::Value(status),
         TimePointToValue(std::chrono::system_clock::now()),
         duckdb::Value(subscription_id)});

    if (RowsChanged(*result) == 0) {
        ERPL_TRACE_WARN("ODP_REPOSITORY", "No subscription found with ID: " + subscription_id);
        return false;
    }

    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Subscription status updated successfully");
    return true;
}

bool OdpSubscriptionRepository::RemoveSubscription(const std::string& subscription_id) {
    ERPL_TRACE_INFO("ODP_REPOSITORY", "Removing subscription: " + subscription_id);

    EnsureTablesExist();

    auto result = Execute(
        "DELETE FROM " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " WHERE subscription_id = ?",
        {duckdb::Value(subscription_id)});

    if (RowsChanged(*result) == 0) {
        ERPL_TRACE_WARN("ODP_REPOSITORY", "No subscription found with ID: " + subscription_id);
        return false;
    }

    ERPL_TRACE_INFO("ODP_REPOSITORY", "Subscription removed successfully");
    return true;
}

int64_t OdpSubscriptionRepository::CreateAuditEntry(const OdpAuditEntry& entry) {
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "Creating audit entry for subscription %s, operation: %s",
        entry.subscription_id, entry.operation_type));

    EnsureTablesExist();

    try {
        // The id comes from a sequence: a max(audit_id)+1 read-modify-write hands
        // the same id to two concurrent readers (#95).
        auto result = Execute(
            "INSERT INTO " + QualifiedTable(AUDIT_TABLE) +
            " (audit_id, subscription_id, operation_type, request_timestamp, response_timestamp, "
            "request_url, http_status_code, rows_fetched, package_size_bytes, "
            "delta_token_before, delta_token_after, error_message, duration_ms) "
            "VALUES (nextval(" + QuoteStringLiteral(qualified_schema + "." + QuoteIdentifier(AUDIT_SEQUENCE)) + "), "
            "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING audit_id",
            {duckdb::Value(entry.subscription_id),
             duckdb::Value(entry.operation_type),
             TimePointToValue(entry.request_timestamp),
             entry.response_timestamp.has_value() ? TimePointToValue(entry.response_timestamp.value())
                                                  : duckdb::Value(duckdb::LogicalType::TIMESTAMP),
             duckdb::Value(entry.request_url),
             entry.http_status_code.has_value() ? duckdb::Value::INTEGER(entry.http_status_code.value())
                                                : duckdb::Value(duckdb::LogicalType::INTEGER),
             duckdb::Value::BIGINT(entry.rows_fetched),
             duckdb::Value::BIGINT(entry.package_size_bytes),
             duckdb::Value(entry.delta_token_before),
             duckdb::Value(entry.delta_token_after),
             duckdb::Value(TruncateForAudit(entry.error_message)),
             entry.duration_ms.has_value() ? duckdb::Value::BIGINT(entry.duration_ms.value())
                                           : duckdb::Value(duckdb::LogicalType::BIGINT)});

        if (result->RowCount() == 0) {
            ERPL_TRACE_WARN("ODP_REPOSITORY", "Audit insert returned no id");
            return -1;
        }

        int64_t audit_id = result->GetValue(0, 0).GetValue<int64_t>();
        ERPL_TRACE_INFO("ODP_REPOSITORY", duckdb::StringUtil::Format("Audit entry created with ID: %lld", audit_id));
        return audit_id;

    } catch (const std::exception& e) {
        // Audit is a side channel: losing a row must not fail the read.
        ERPL_TRACE_ERROR("ODP_REPOSITORY", "Error creating audit entry: " + std::string(e.what()));
        return -1;
    }
}

bool OdpSubscriptionRepository::UpdateAuditTotals(int64_t audit_id, int64_t rows_fetched,
                                                 int64_t package_size_bytes)
{
    EnsureTablesExist();

    try {
        auto result = Execute(
            "UPDATE " + QualifiedTable(AUDIT_TABLE) +
                " SET response_timestamp = ?, rows_fetched = ?, package_size_bytes = ? "
                "WHERE audit_id = ?",
            {TimePointToValue(std::chrono::system_clock::now()),
             duckdb::Value::BIGINT(rows_fetched),
             duckdb::Value::BIGINT(package_size_bytes),
             duckdb::Value::BIGINT(audit_id)});
        return result != nullptr;
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_REPOSITORY",
                         "Failed to update audit totals: " + std::string(e.what()));
        return false;
    }
}

bool OdpSubscriptionRepository::UpdateAuditEntry(const OdpAuditEntry& entry) {
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "Updating audit entry %lld for subscription %s",
        entry.audit_id, entry.subscription_id));

    EnsureTablesExist();

    try {
        auto result = Execute(
            "UPDATE " + QualifiedTable(AUDIT_TABLE) + " SET response_timestamp = ?, http_status_code = ?, "
            "rows_fetched = ?, package_size_bytes = ?, delta_token_after = ?, error_message = ?, duration_ms = ? "
            "WHERE audit_id = ?",
            {TimePointToValue(std::chrono::system_clock::now()),
             entry.http_status_code.has_value() ? duckdb::Value::INTEGER(entry.http_status_code.value())
                                                : duckdb::Value(duckdb::LogicalType::INTEGER),
             duckdb::Value::BIGINT(entry.rows_fetched),
             duckdb::Value::BIGINT(entry.package_size_bytes),
             duckdb::Value(entry.delta_token_after),
             duckdb::Value(TruncateForAudit(entry.error_message)),
             entry.duration_ms.has_value() ? duckdb::Value::BIGINT(entry.duration_ms.value())
                                           : duckdb::Value(duckdb::LogicalType::BIGINT),
             duckdb::Value::BIGINT(entry.audit_id)});

        ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Audit entry updated successfully");
        return RowsChanged(*result) > 0;

    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_REPOSITORY", "Error updating audit entry: " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// Static Utility Methods
// ============================================================================

std::string OdpSubscriptionRepository::GenerateSubscriptionId(const std::string& service_url,
                                                             const std::string& entity_set_name,
                                                             const std::string& secret_name) {
    // Generate timestamp prefix: YYYYMMDD_HHMMSS
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    auto local_tm = ToLocalTm(now_time);

    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(&local_tm, "%Y%m%d_%H%M%S");

    std::string cleaned_url = CleanUrlForId(service_url);
    std::string cleaned_entity = CleanUrlForId(entity_set_name);

    std::string cleaned_secret = CleanUrlForId(secret_name.empty() ? "default" : secret_name);

    std::string subscription_id =
        timestamp_stream.str() + "_" + cleaned_url + "_" + cleaned_entity + "_" + cleaned_secret;

    ERPL_TRACE_DEBUG("ODP_REPOSITORY", "Generated subscription ID: " + subscription_id);
    return subscription_id;
}

std::string OdpSubscriptionRepository::CleanUrlForId(const std::string& url) {
    // Remove protocol, replace special characters with underscores
    std::string cleaned = url;

    std::regex protocol_regex("^https?://");
    cleaned = std::regex_replace(cleaned, protocol_regex, "");

    std::regex special_chars_regex("[^a-zA-Z0-9_]");
    cleaned = std::regex_replace(cleaned, special_chars_regex, "_");

    std::regex multiple_underscores_regex("_{2,}");
    cleaned = std::regex_replace(cleaned, multiple_underscores_regex, "_");

    if (!cleaned.empty() && cleaned.front() == '_') {
        cleaned = cleaned.substr(1);
    }
    if (!cleaned.empty() && cleaned.back() == '_') {
        cleaned = cleaned.substr(0, cleaned.length() - 1);
    }

    if (cleaned.length() > 100) {
        cleaned = cleaned.substr(0, 100);
    }

    return cleaned;
}

bool OdpSubscriptionRepository::IsValidOdpUrl(const std::string& url) {
    // Match on the path only. Matching the whole URL made every service URL that
    // already carried a query string (?$format=json, ?$select=...) fail (#105).
    std::string path = url;
    auto fragment_pos = path.find('#');
    if (fragment_pos != std::string::npos) {
        path = path.substr(0, fragment_pos);
    }
    auto query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }

    std::regex odp_pattern(".*/(EntityOf|FactsOf|AttrOf)[^/]*$", std::regex_constants::icase);
    bool is_odp = std::regex_search(path, odp_pattern);

    ERPL_TRACE_DEBUG("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "URL validation for %s: %s", url, is_odp ? "VALID" : "INVALID"));

    return is_odp;
}

std::string OdpSubscriptionRepository::TruncateForAudit(const std::string& message) {
    if (message.length() <= MAX_AUDIT_ERROR_LENGTH) {
        return message;
    }
    return message.substr(0, MAX_AUDIT_ERROR_LENGTH) + "... [truncated]";
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void OdpSubscriptionRepository::InitializeSchema() {
    Execute("CREATE SCHEMA IF NOT EXISTS " + qualified_schema, {});
}

// The subscriptions table body, shared by the fresh create and by the v1 migration so the
// two cannot drift into different layouts.
std::string OdpSubscriptionRepository::SubscriptionsTableBody() {
    return std::string("(")
         + "subscription_id VARCHAR PRIMARY KEY, "
           "service_url VARCHAR NOT NULL, "
           "entity_set_name VARCHAR NOT NULL, "
           "secret_name VARCHAR, "
           "delta_token VARCHAR, "
           "created_at TIMESTAMP DEFAULT NOW(), "
           "last_updated TIMESTAMP DEFAULT NOW(), "
           "subscription_status VARCHAR DEFAULT 'active', "
           "preference_applied BOOLEAN DEFAULT FALSE, "
           "schema_version INTEGER NOT NULL DEFAULT 1, "
           // One subscription per source AND credential: without the uniqueness two
           // sessions racing on the first read each insert their own row and split the
           // delta stream. The secret belongs in the key because a delta position is
           // only meaningful relative to the credential that established it -- two
           // callers reading the same entity set with different secrets previously
           // shared one cursor, so one silently advanced the other's position (#236).
           "UNIQUE (service_url, entity_set_name, secret_name))";
}

void OdpSubscriptionRepository::InitializeTables() {
    Execute("CREATE TABLE IF NOT EXISTS " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " " +
            SubscriptionsTableBody(), {});

    Execute("CREATE SEQUENCE IF NOT EXISTS " + qualified_schema + "." + QuoteIdentifier(AUDIT_SEQUENCE) +
            " START 1", {});

    Execute("CREATE TABLE IF NOT EXISTS " + QualifiedTable(AUDIT_TABLE) + " ("
            "audit_id BIGINT PRIMARY KEY, "
            "subscription_id VARCHAR NOT NULL, "
            "operation_type VARCHAR NOT NULL, "
            "request_timestamp TIMESTAMP DEFAULT NOW(), "
            "response_timestamp TIMESTAMP, "
            "request_url VARCHAR, "
            "http_status_code INTEGER, "
            "rows_fetched BIGINT DEFAULT 0, "
            "package_size_bytes BIGINT DEFAULT 0, "
            "delta_token_before VARCHAR, "
            "delta_token_after VARCHAR, "
            "error_message VARCHAR, "
            "duration_ms BIGINT)", {});
}

// Rewrite a v1 subscriptions table -- keyed (service_url, entity_set_name) -- into the v2
// layout, which adds secret_name to the unique key (#236).
//
// This has to migrate rather than refuse. The precedent for an unreadable old table is to
// tell the operator to drop the schema (see VerifySchemaVersion), but that is the wrong
// trade here: dropping the table discards every delta token, and a lost delta token means
// a full re-extraction of an SAP source, which is exactly the expense this work exists to
// avoid. Old rows are unique on the pair and therefore already unique on the triple, so
// the copy cannot collide.
void OdpSubscriptionRepository::MigrateSubscriptionsToV2() {
    auto table_exists = Execute(
        "SELECT count(*) FROM duckdb_tables() WHERE database_name = ? AND schema_name = ? "
        "AND table_name = ?",
        {duckdb::Value(state_catalog), duckdb::Value(SCHEMA_NAME), duckdb::Value(SUBSCRIPTIONS_TABLE)});
    if (table_exists->RowCount() == 0 || table_exists->GetValue(0, 0).GetValue<int64_t>() == 0) {
        return;  // fresh install; InitializeTables creates it at v2 directly
    }

    // A table that predates schema_version entirely is not ours to migrate: VerifySchemaVersion
    // reports it with instructions, and guessing at its layout here would corrupt it.
    auto has_version = Execute(
        "SELECT count(*) FROM duckdb_columns() WHERE database_name = ? AND schema_name = ? "
        "AND table_name = ? AND column_name = 'schema_version'",
        {duckdb::Value(state_catalog), duckdb::Value(SCHEMA_NAME), duckdb::Value(SUBSCRIPTIONS_TABLE)});
    if (has_version->RowCount() == 0 || has_version->GetValue(0, 0).GetValue<int64_t>() == 0) {
        return;
    }

    // The unique key is the thing that actually changed, so detect on it rather than on the
    // stored schema_version: a table whose constraint already names secret_name is at v2
    // however its rows are stamped.
    auto already_v2 = Execute(
        "SELECT count(*) FROM duckdb_constraints() WHERE database_name = ? AND schema_name = ? "
        "AND table_name = ? AND constraint_type = 'UNIQUE' "
        "AND list_contains(constraint_column_names, 'secret_name')",
        {duckdb::Value(state_catalog), duckdb::Value(SCHEMA_NAME), duckdb::Value(SUBSCRIPTIONS_TABLE)});
    if (already_v2->RowCount() > 0 && already_v2->GetValue(0, 0).GetValue<int64_t>() > 0) {
        return;
    }

    ERPL_TRACE_INFO("ODP_REPOSITORY",
                    "Migrating ODP subscriptions table to v2 (secret_name joins the unique key)");

    const std::string legacy_table = std::string(SUBSCRIPTIONS_TABLE) + "_v1_migrating";
    const std::string qualified_legacy = qualified_schema + "." + QuoteIdentifier(legacy_table);

    // A leftover from a migration interrupted part-way would make the rename fail.
    Execute("DROP TABLE IF EXISTS " + qualified_legacy, {});
    Execute("ALTER TABLE " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " RENAME TO " +
            QuoteIdentifier(legacy_table), {});
    Execute("CREATE TABLE " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " " + SubscriptionsTableBody(), {});

    // Rows written before secret_name was recorded hold NULL or ''; both mean the default
    // lookup, which is the name the rest of the repository uses for it.
    Execute("INSERT INTO " + QualifiedTable(SUBSCRIPTIONS_TABLE) + " (" + SUBSCRIPTION_COLUMNS + ") "
            "SELECT subscription_id, service_url, entity_set_name, "
            "COALESCE(NULLIF(secret_name, ''), 'default'), delta_token, created_at, last_updated, "
            "subscription_status, preference_applied, ? FROM " + qualified_legacy,
            {duckdb::Value::INTEGER(SCHEMA_VERSION)});

    Execute("DROP TABLE " + qualified_legacy, {});
    ERPL_TRACE_INFO("ODP_REPOSITORY", "ODP subscriptions table migrated to v2");
}

void OdpSubscriptionRepository::VerifySchemaVersion() {
    // CREATE TABLE IF NOT EXISTS silently accepts a pre-existing table with an
    // older layout. Detect that here instead of misreading its rows (#96).
    auto result = Execute(
        "SELECT count(*) FROM duckdb_columns() WHERE database_name = ? AND schema_name = ? "
        "AND table_name = ? AND column_name = 'schema_version'",
        {duckdb::Value(state_catalog), duckdb::Value(SCHEMA_NAME), duckdb::Value(SUBSCRIPTIONS_TABLE)});

    const int64_t column_count = result->RowCount() > 0 ? result->GetValue(0, 0).GetValue<int64_t>() : 0;
    if (column_count == 0) {
        throw duckdb::InvalidInputException(
            "The ODP state table " + state_catalog + ".erpl_web." + SUBSCRIPTIONS_TABLE + " was written by an "
            "older version of erpl_web and has no schema_version column. Its delta tokens cannot be read safely. "
            "Drop the erpl_web schema in that database (DROP SCHEMA " + state_catalog + ".erpl_web CASCADE) and "
            "re-subscribe, or point ODP at a different database.");
    }
}

duckdb::unique_ptr<duckdb::MaterializedQueryResult> OdpSubscriptionRepository::Execute(
    const std::string& sql, duckdb::vector<duckdb::Value> params) {
    // Only the statement label is traced: the SQL carries no caller data now
    // that every value is bound, and the values themselves may be credentials.
    ERPL_TRACE_DEBUG("ODP_REPOSITORY", duckdb::StringUtil::Format(
        "Executing ODP state statement (%zu bound parameters)", params.size()));

    // Constructed from the DatabaseInstance rather than via GetDatabase(context):
    // the latter resolves through the client's transaction context, which is not
    // active when the repository is driven from bind or from a scan callback.
    duckdb::Connection connection(*context.db);
    // IOException, not InternalException: these statements fail for reasons outside the extension's
    // control (a read-only or full state catalog, a constraint hit by a concurrent session), and
    // InternalException invalidates the whole database instance (GitHub #251).
    auto prepared = connection.Prepare(sql);
    if (!prepared || prepared->HasError()) {
        throw duckdb::IOException("Failed to prepare ODP state statement: " +
                                        (prepared ? prepared->GetError() : std::string("no statement returned")));
    }

    auto result = prepared->Execute(params, false);
    if (!result) {
        throw duckdb::IOException("Failed to execute ODP state statement: no result returned");
    }
    if (result->HasError()) {
        throw duckdb::IOException("Failed to execute ODP state statement: " + result->GetError());
    }

    auto* materialized = dynamic_cast<duckdb::MaterializedQueryResult*>(result.get());
    if (!materialized) {
        throw duckdb::IOException("Failed to execute ODP state statement: non-materialized result returned");
    }
    result.release();
    return duckdb::unique_ptr<duckdb::MaterializedQueryResult>(materialized);
}

int64_t OdpSubscriptionRepository::RowsChanged(duckdb::MaterializedQueryResult& result) {
    if (result.RowCount() == 0 || result.ColumnCount() == 0) {
        return 0;
    }
    auto value = result.GetValue(0, 0);
    if (value.IsNull()) {
        return 0;
    }
    return value.GetValue<int64_t>();
}

duckdb::Value OdpSubscriptionRepository::TimePointToValue(const std::chrono::system_clock::time_point& tp) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    return duckdb::Value::TIMESTAMP(duckdb::timestamp_t(micros));
}

std::chrono::system_clock::time_point OdpSubscriptionRepository::ValueToTimePoint(const duckdb::Value& value) {
    if (value.IsNull()) {
        return std::chrono::system_clock::time_point {};
    }
    const auto timestamp = value.GetValue<duckdb::timestamp_t>();
    return std::chrono::system_clock::time_point(std::chrono::microseconds(timestamp.value));
}

OdpSubscription OdpSubscriptionRepository::RowToSubscription(duckdb::MaterializedQueryResult& result,
                                                             duckdb::idx_t row) {
    const int32_t row_version = result.GetValue(COL_SCHEMA_VERSION, row).GetValue<int32_t>();
    if (row_version != SCHEMA_VERSION) {
        throw duckdb::InvalidInputException(duckdb::StringUtil::Format(
            "ODP subscription row has schema version %d but this build understands version %d. "
            "Its delta token cannot be interpreted safely; drop the erpl_web schema and re-subscribe.",
            row_version, SCHEMA_VERSION));
    }

    OdpSubscription subscription;
    subscription.subscription_id = result.GetValue(COL_SUBSCRIPTION_ID, row).ToString();
    subscription.service_url = result.GetValue(COL_SERVICE_URL, row).ToString();
    subscription.entity_set_name = result.GetValue(COL_ENTITY_SET_NAME, row).ToString();
    subscription.secret_name = result.GetValue(COL_SECRET_NAME, row).ToString();
    auto token_value = result.GetValue(COL_DELTA_TOKEN, row);
    subscription.delta_token = token_value.IsNull() ? std::string() : token_value.ToString();
    subscription.created_at = ValueToTimePoint(result.GetValue(COL_CREATED_AT, row));
    subscription.last_updated = ValueToTimePoint(result.GetValue(COL_LAST_UPDATED, row));
    subscription.subscription_status = result.GetValue(COL_SUBSCRIPTION_STATUS, row).ToString();
    subscription.preference_applied = result.GetValue(COL_PREFERENCE_APPLIED, row).GetValue<bool>();
    return subscription;
}

} // namespace erpl_web
