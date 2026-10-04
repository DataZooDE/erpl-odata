#include "odp_odata_read_bind_data.hpp"
#include "duckdb_argument_helper.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "odata_url_helpers.hpp"
#include "secret_functions.hpp"
#include "tracing.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace erpl_web {

namespace {

// SAP ODP marks the END of a change-tracked extraction by returning the delta token on
// "__next", not on "__delta". Following such a link as though it were another page walks
// past the end of the extraction and loses the token, leaving the subscription in
// initial-load mode so the next read re-extracts everything. A "__next" carrying a delta
// sigil is therefore terminal, not a page. See GitHub #102.
bool IsDeltaLink(const std::string &url)
{
    return !ODataDeltaLink::ExtractToken(url).empty();
}

} // namespace


OdpODataReadBindData::OdpODataReadBindData(duckdb::ClientContext& context,
                                         const std::string& entity_set_url,
                                         const std::string& secret_name,
                                         bool force_full_load,
                                         const std::string& import_delta_token,
                                         std::optional<uint32_t> max_page_size)
    : context_(context)
    , entity_set_url_(entity_set_url)
    , secret_name_(secret_name.empty() ? "default" : secret_name)
    , max_page_size_(max_page_size)
    , force_full_load_(force_full_load)
    , import_delta_token_(import_delta_token)
    , initialized_(false)
    , first_fetch_completed_(false)
    , current_audit_id_(-1)
    , initial_load_in_progress_(false)
    , delta_fetch_in_progress_(false)
{
    ERPL_TRACE_INFO("ODP_BIND_DATA", duckdb::StringUtil::Format(
        "Creating ODP bind data - URL: %s, Secret: %s, ForceFullLoad: %s, ImportToken: %s",
        entity_set_url_, secret_name_, force_full_load ? "true" : "false",
        import_delta_token.empty() ? "NONE" : "PROVIDED"));

    // Just validate URL in constructor - defer initialization to explicit Initialize() call
    ValidateEntitySetUrl();
}

duckdb::unique_ptr<OdpODataReadBindData> OdpODataReadBindData::CloneForScan() const {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Cloning ODP bind data for a new execution");

    // force_full_load and import_delta_token are ONE-SHOT bind options, and are
    // deliberately not forwarded to the clone.
    //
    // Bind has already applied them: force_full_load reset the subscription, and
    // import_delta_token was written to it. Replaying them per execution would mean a
    // prepared statement re-imports the same token on every EXECUTE, so execution 2 could
    // never advance past the token execution 1 consumed - the subscription would be pinned
    // forever at the imported position. The clone instead resolves the subscription bind
    // left behind, which is exactly the state those options produced (GitHub #188).
    auto clone = duckdb::make_uniq<OdpODataReadBindData>(
        context_, entity_set_url_, secret_name_, /*force_full_load=*/false,
        /*import_delta_token=*/std::string(), max_page_size_);

    if (initialized_) {
        clone->Initialize();
    }

    // Column activation is held on the wrapper rather than the inner bind data (GitHub
    // #58): the inner instance is replaced wholesale on every page, and only the wrapper
    // remembers the selection well enough to re-apply it to the replacement. A clone that
    // did not carry it would run page 2 onwards in all-columns mode.
    if (!active_column_ids_.empty()) {
        clone->ActivateColumns(active_column_ids_);
    }

    // Result modifiers (pushed LIMIT / ORDER BY) are deliberately not carried, because
    // AddResultModifiers is intentionally never called on either bind data - see the note
    // at odata_read_functions.hpp. If that path is ever wired up, modifier state has to be
    // copied here too.

    return clone;
}

// ============================================================================
// Core DuckDB Table Function Interface (Delegated)
// ============================================================================

std::vector<std::string> OdpODataReadBindData::GetResultNames(bool all_columns) {
    if (!odata_bind_data_) {
        throw duckdb::InternalException("OData bind data not initialized");
    }
    return odata_bind_data_->GetResultNames(all_columns);
}

std::vector<duckdb::LogicalType> OdpODataReadBindData::GetResultTypes(bool all_columns) {
    if (!odata_bind_data_) {
        throw duckdb::InternalException("OData bind data not initialized");
    }
    return odata_bind_data_->GetResultTypes(all_columns);
}

bool OdpODataReadBindData::HasMoreResults() {
    if (!odata_bind_data_) {
        return false;
    }
    // A pending __next URL means odata_bind_data_ will be reloaded on next
    // FetchNextResult call even after the current page returns 0 rows.
    if (!pending_next_url_.empty()) {
        return true;
    }
    return odata_bind_data_->HasMoreResults();
}

unsigned int OdpODataReadBindData::FetchNextResult(duckdb::DataChunk &output) {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "FetchNextResult called");
    
    if (!initialized_) {
        throw duckdb::InternalException("ODP bind data not initialized");
    }
    
    try {
        // Handle first fetch - determine if we need initial load or delta fetch
        if (!first_fetch_completed_) {
            bool success = false;
            
            // Diagnostic: log current phase and token before request
            ERPL_TRACE_INFO("ODP_BIND_DATA", duckdb::StringUtil::Format(
                "Pre-request state: Phase=%s, Token=%s",
                OdpSubscriptionStateManager::PhaseToString(state_manager_->GetCurrentPhase()),
                state_manager_->GetCurrentDeltaToken().empty() ? "<EMPTY>" : state_manager_->GetCurrentDeltaToken().substr(0, 64)));

            if (state_manager_->ShouldPerformInitialLoad()) {
                ERPL_TRACE_INFO("ODP_BIND_DATA", "Performing initial load");
                success = HandleInitialLoad();
            } else if (state_manager_->ShouldPerformDeltaFetch()) {
                ERPL_TRACE_INFO("ODP_BIND_DATA", "Performing delta fetch");
                success = HandleDeltaFetch();
            } else {
                // A state this reader cannot fetch from, not a broken invariant: InternalException would invalidate
                // the whole database instance (GitHub #251).
                throw duckdb::IOException("ODP subscription '%s' is in phase %s and cannot be read; run the "
                                          "query again to start a new extraction",
                                          state_manager_->GetSubscriptionId(),
                                          OdpSubscriptionStateManager::PhaseToString(state_manager_->GetCurrentPhase()));
            }
            
            if (!success) {
                // Deliberately NOT an InternalException. DuckDB reads ExceptionType::INTERNAL
                // as "this process's invariants are broken" and invalidates the whole database
                // instance, so every later query - including ones unrelated to ODP - fails with
                // "Failure within transaction management!". A service refusing an extraction is
                // an ordinary I/O outcome (GitHub #173).
                throw duckdb::IOException(
                    "ODP data fetch failed for " + entity_set_url_ + ": " +
                    (last_fetch_error_.empty() ? std::string("the service gave no reason")
                                               : last_fetch_error_));
            }
            
            first_fetch_completed_ = true;
        }
        
        // Drain the current page; when it is exhausted, keep loading pages until one
        // yields rows or there is no next page left.
        //
        // This has to be a LOOP, not a single retry. A page that legitimately yields zero
        // rows while still carrying a __next is a real shape - ODP delta packages and
        // skip-token pages both produce it - and stopping after one attempt would end the
        // scan there, handing back a partial extraction presented as complete and then
        // committing the delta token over rows that were never delivered.
        //
        // The page count is bounded so a service answering with an endless run of empty
        // pages fails loudly instead of spinning.
        //
        // It must FAIL, not yield. Breaking out with zero rows while a next page is still
        // pending does not "resume on the next call" - a zero-row chunk is how a DuckDB
        // table function says end-of-scan, and OdpODataReadScan reads that zero as the end
        // and calls FinalizeScan(), which COMMITS THE STAGED DELTA TOKEN. That is precisely
        // the outcome the loop above exists to prevent: a partial extraction presented as
        // complete, with the token advanced over rows never delivered. The old comment
        // claimed the opposite of what the code did.
        constexpr unsigned int MAX_EMPTY_PAGES_PER_CALL = 64;
        const unsigned int rows_fetched = DrainEmptyOdpPages(
            [&] { return odata_bind_data_->FetchNextResult(output); },
            [&] { return !pending_next_url_.empty(); },
            [&] { FetchAndLoadNextPage(); },
            MAX_EMPTY_PAGES_PER_CALL, "");

        // Every row has now been handed to DuckDB and no further page is pending, so the
        // delta package has actually been delivered and its token may safely be advanced.
        if (rows_fetched == 0 && pending_next_url_.empty()) {
            CommitStagedDeltaToken();
        }

        ERPL_TRACE_DEBUG("ODP_BIND_DATA", duckdb::StringUtil::Format("Fetched %u rows", rows_fetched));

        // Accumulate; the audit row is written once, when the scan finishes. Writing here
        // meant one UPDATE per 2048 rows - each on a freshly constructed duckdb::Connection
        // that takes the connection-manager lock - on the row-emission hot path, and
        // because the UPDATE assigns rather than adds, the row ended up holding the last
        // chunk's count rather than the total. See GitHub #158.
        audit_rows_fetched_ += static_cast<int64_t>(rows_fetched);
        audit_package_size_bytes_ +=
            static_cast<int64_t>(output.size() * output.GetTypes().size() * 8);

        return rows_fetched;
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Error in FetchNextResult: " + std::string(e.what()));
        HandleRequestError(e, first_fetch_completed_ ? "data_fetch" : "initial_setup");
        throw;
    }
}

void OdpODataReadBindData::ActivateColumns(const std::vector<duckdb::column_t> &column_ids) {
    active_column_ids_ = column_ids;
    if (odata_bind_data_) {
        odata_bind_data_->ActivateColumns(column_ids);
    }
}

void OdpODataReadBindData::AddFilters(const duckdb::optional_ptr<duckdb::TableFilterSet> &filters) {
    if (odata_bind_data_) {
        odata_bind_data_->AddFilters(filters);
    }
}

void OdpODataReadBindData::AddResultModifiers(const std::vector<duckdb::unique_ptr<duckdb::BoundResultModifier>> &modifiers) {
    if (odata_bind_data_) {
        odata_bind_data_->AddResultModifiers(modifiers);
    }
}

double OdpODataReadBindData::GetProgressFraction() const {
    if (odata_bind_data_) {
        return odata_bind_data_->GetProgressFraction();
    }
    return 0.0;
}

// ============================================================================
// ODP-Specific Interface
// ============================================================================

std::string OdpODataReadBindData::GetSubscriptionId() const {
    return state_manager_->GetSubscriptionId();
}

std::string OdpODataReadBindData::GetCurrentDeltaToken() const {
    return state_manager_->GetCurrentDeltaToken();
}

bool OdpODataReadBindData::IsSubscriptionActive() const {
    return state_manager_->IsSubscriptionActive();
}

OdpSubscriptionStateManager::SubscriptionPhase OdpODataReadBindData::GetCurrentPhase() const {
    return state_manager_->GetCurrentPhase();
}

std::vector<OdpAuditEntry> OdpODataReadBindData::GetAuditHistory(int days_back) const {
    // This would be implemented by the repository
    // For now, return empty vector
    return std::vector<OdpAuditEntry>();
}

// ============================================================================
// Private Implementation Methods
// ============================================================================

void OdpODataReadBindData::Initialize() {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Initializing ODP bind data");
    
    try {
        // Setup authentication
        SetupAuthentication();
        
        // Create state manager
        std::string entity_set_name = ExtractEntitySetName(entity_set_url_);
        state_manager_ = std::make_unique<OdpSubscriptionStateManager>(
            context_, entity_set_url_, entity_set_name, secret_name_,
            force_full_load_, import_delta_token_);
        
        // Create request orchestrator
        request_orchestrator_ = std::make_unique<OdpRequestOrchestrator>(
            auth_params_, max_page_size_.value_or(15000));
        
        // Create the underlying OData bind data
        CreateODataBindData();
        
        initialized_ = true;
        LogCurrentState();
        
        ERPL_TRACE_INFO("ODP_BIND_DATA", "ODP bind data initialized successfully");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Failed to initialize ODP bind data: " + std::string(e.what()));
        throw;
    }
}

// Builds auth params from a secret the caller named, rather than from whatever secret
// happens to match the URL.
//
// The named secret was logged and stored on the subscription and then never used to pick
// credentials: SetupAuthentication looked the URL up instead. So `secret => 'a'` and
// `secret => 'b'` against the same service both authenticated as whichever secret the URL
// matched - silently, with no indication the requested one had been ignored.
static std::shared_ptr<HttpAuthParams> AuthParamsFromNamedSecret(duckdb::ClientContext &context,
                                                                 const std::string &secret_name) {
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);

    // Naming a secret that does not exist is an error, not a reason to fall back to the URL
    // lookup: falling back is how the caller's choice got ignored in the first place.
    if (!secret_entry) {
        throw duckdb::InvalidInputException(
            "Secret '%s' not found. Create it with CREATE SECRET, or omit the secret argument "
            "to match one by URL.", secret_name.c_str());
    }

    const auto *kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (kv_secret == nullptr) {
        throw duckdb::InvalidInputException("Secret '%s' is not a key-value secret.",
                                            secret_name.c_str());
    }

    auto params = std::make_shared<HttpAuthParams>();
    const auto &type = secret_entry->secret->GetType();
    if (type == "http_basic") {
        // A secret missing its username/password is bad input, not an invariant violation.
        // TryGetValue with error_on_missing throws InternalException, which invalidates the
        // database instance (GitHub #243).
        params->basic_credentials = std::make_tuple(
            RequireSecretValue(*kv_secret, "username", secret_name),
            RequireSecretValue(*kv_secret, "password", secret_name));
    } else if (type == "http_bearer") {
        params->bearer_token = RequireSecretValue(*kv_secret, "token", secret_name);
    } else {
        throw duckdb::InvalidInputException(
            "Secret '%s' has type '%s'; ODP needs an http_basic or http_bearer secret.",
            secret_name.c_str(), type.c_str());
    }
    return params;
}

void OdpODataReadBindData::SetupAuthentication() {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Setting up authentication with secret: " + secret_name_);
    
    try {
        if (!secret_name_.empty() && secret_name_ != "default") {
            auth_params_ = AuthParamsFromNamedSecret(context_, secret_name_);
            ERPL_TRACE_INFO("ODP_BIND_DATA", "Authentication configured from secret '" + secret_name_ + "'");
            return;
        }

        auth_params_ = ResolveHttpAuthFromSecrets(context_, entity_set_url_);
        if (!auth_params_) {
            ERPL_TRACE_WARN("ODP_BIND_DATA", "No authentication parameters found for URL: " + entity_set_url_);
        } else {
            ERPL_TRACE_INFO("ODP_BIND_DATA", "Authentication configured successfully");
        }
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Failed to setup authentication: " + std::string(e.what()));
        throw;
    }
}

void OdpODataReadBindData::CreateODataBindData() {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Creating underlying OData bind data");
    
    try {
        // Create OData bind data using the entity set URL
        odata_bind_data_ = ODataReadBindData::FromEntitySetRoot(entity_set_url_, auth_params_);
        
        if (!odata_bind_data_) {
            throw duckdb::InternalException("Failed to create OData bind data");
        }
        
        ERPL_TRACE_INFO("ODP_BIND_DATA", "OData bind data created successfully");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Failed to create OData bind data: " + std::string(e.what()));
        throw;
    }
}

void OdpODataReadBindData::ValidateEntitySetUrl() const {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Validating entity set URL: " + entity_set_url_);
    
    if (entity_set_url_.empty()) {
        throw duckdb::InvalidInputException("Entity set URL cannot be empty");
    }
    
    // The ODP entity-set naming convention is a hint, not a contract: warn on a
    // URL that does not match it rather than refusing to read it (#105).
    if (!OdpSubscriptionRepository::IsValidOdpUrl(entity_set_url_)) {
        ERPL_TRACE_WARN("ODP_BIND_DATA",
            "Entity set URL does not look like an ODP entity set (expected EntityOf*/FactsOf*/AttrOf*): " +
            entity_set_url_);
    }
    
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Entity set URL validation passed");
}

bool OdpODataReadBindData::HandleInitialLoad() {
    ERPL_TRACE_INFO("ODP_BIND_DATA", "Handling initial load request");

    try {
        current_audit_id_ = state_manager_->CreateAuditEntry("initial_load", entity_set_url_);

        auto result = request_orchestrator_->ExecuteInitialLoad(entity_set_url_, max_page_size_);

        if (!result.response) {
            last_fetch_error_ = "the service returned no response to the initial load";
            return false;
        }

        // Only this first response answers the `Prefer: odata.track-changes` we sent, so record
        // the server's answer now; later pages cannot re-confirm it.
        initial_load_preference_applied_ = result.preference_applied;

        auto first_next = result.response->NextUrl();
        pending_next_url_ = (first_next.has_value() && !first_next->empty() && !IsDeltaLink(*first_next))
            ? first_next.value() : "";

        if (pending_next_url_.empty()) {
            // Single-page response: process state transition immediately.
            ProcessRequestResult(result, "initial_load");
        } else {
            // Multi-page: defer state transition until FetchAndLoadNextPage reaches the last page.
            initial_load_in_progress_ = true;
            ERPL_TRACE_INFO("ODP_BIND_DATA", "Multi-page initial load — deferring state transition to last page");
        }

        UpdateODataClientWithResponse(entity_set_url_, result.response->RawContent());
        return true;

    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Initial load failed: " + std::string(e.what()));
        last_fetch_error_ = e.what();
        state_manager_->TransitionToError("Initial load failed: " + std::string(e.what()));
        return false;
    }
}

bool OdpODataReadBindData::HandleDeltaFetch() {
    ERPL_TRACE_INFO("ODP_BIND_DATA", "Handling delta fetch request");

    std::string current_token = state_manager_->GetCurrentDeltaToken();
    if (current_token.empty()) {
        ERPL_TRACE_WARN("ODP_BIND_DATA", "No delta token available, falling back to initial load");
        state_manager_->TransitionToInitialLoad();
        return HandleInitialLoad();
    }

    try {
        current_audit_id_ = state_manager_->CreateAuditEntry("delta_fetch", entity_set_url_);

        ERPL_TRACE_INFO("ODP_BIND_DATA", duckdb::StringUtil::Format(
            "Delta fetch with token: %s", current_token.substr(0, 64)));
        auto result = request_orchestrator_->ExecuteDeltaFetch(entity_set_url_, current_token, max_page_size_);

        if (!result.response) {
            last_fetch_error_ = "the service returned no response to the delta fetch";
            return false;
        }

        auto first_next = result.response->NextUrl();
        pending_next_url_ = (first_next.has_value() && !first_next->empty() && !IsDeltaLink(*first_next))
            ? first_next.value() : "";

        if (pending_next_url_.empty()) {
            // Single-page delta response: process state transition immediately.
            ProcessRequestResult(result, "delta_fetch");
        } else {
            // Multi-page: defer state transition until FetchAndLoadNextPage reaches the last page.
            delta_fetch_in_progress_ = true;
            ERPL_TRACE_INFO("ODP_BIND_DATA", "Multi-page delta fetch — deferring state transition to last page");
        }

        // The __delta link is server-supplied and becomes the URL the OData client requests
        // next, with credentials attached. It gets BOTH checks every other follower of a
        // server-supplied link gets: same origin (#101), and sendable at all - a link
        // carrying CR/LF or a space would go into a request line that is written verbatim.
        // This was the last follower holding only the origin half of that pair.
        std::string delta_url = OdpRequestOrchestrator::BuildDeltaUrl(entity_set_url_, current_token);
        if (!result.extracted_delta_url.empty()) {
            if (!IsWireSafeUrl(result.extracted_delta_url)) {
                ERPL_TRACE_WARN("ODP_BIND_DATA", duckdb::StringUtil::Format(
                    "Ignoring server-supplied delta link containing characters that cannot be "
                    "sent in a request (%s)",
                    SummariseUrlForMessage(result.extracted_delta_url)));
            } else if (OdpRequestOrchestrator::IsSameOrigin(entity_set_url_, result.extracted_delta_url)) {
                delta_url = result.extracted_delta_url;
            } else {
                ERPL_TRACE_WARN("ODP_BIND_DATA", duckdb::StringUtil::Format(
                    "Ignoring server-supplied delta link on a different origin than the service (%s -> %s)",
                    entity_set_url_, result.extracted_delta_url));
            }
        }
        ERPL_TRACE_INFO("ODP_BIND_DATA", "Using delta URL for first page injection: " + delta_url);
        UpdateODataClientWithResponse(delta_url, result.response->RawContent());
        return true;

    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Delta fetch failed: " + std::string(e.what()));
        if (IsTokenError(e)) {
            ERPL_TRACE_INFO("ODP_BIND_DATA", "Token error detected, falling back to initial load");
            state_manager_->TransitionToInitialLoad();
            return HandleInitialLoad();
        }
        last_fetch_error_ = e.what();
        state_manager_->TransitionToError("Delta fetch failed: " + std::string(e.what()));
        return false;
    }
}

void OdpODataReadBindData::ProcessRequestResult(const OdpRequestOrchestrator::OdpRequestResult& result,
                                              const std::string& operation_type) {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", duckdb::StringUtil::Format(
        "Processing %s result - Status: %d, DeltaToken: %s, PreferenceApplied: %s",
        operation_type, result.http_status_code,
        result.extracted_delta_token.empty() ? "NONE" : "PRESENT",
        result.preference_applied ? "YES" : "NO"));
    
    // Update audit entry with response details
    if (current_audit_id_ > 0) {
        state_manager_->UpdateAuditEntry(current_audit_id_, result.http_status_code, 0, 
                                       result.response_size_bytes, result.extracted_delta_token);
    }
    
    // Stage the token rather than committing it here. At this point the response has
    // merely been parsed - not a single row has been handed to DuckDB yet. Because SAP's
    // ODQ discards a delta package once its token is acknowledged, committing now means a
    // cancelled query, a failed page or a crash during the scan skips that package
    // permanently. CommitStagedDeltaToken() writes it once the scan has drained.
    // See GitHub #62.
    if (operation_type == "initial_load") {
        if (!result.extracted_delta_token.empty() && result.preference_applied) {
            staged_delta_token_ = result.extracted_delta_token;
            staged_operation_type_ = operation_type;
            staged_preference_applied_ = result.preference_applied;
            has_staged_delta_token_ = true;
            ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Staged delta token from initial load, pending scan completion");
        } else {
            // Initial load without change tracking - stay in initial load mode
            ERPL_TRACE_WARN("ODP_BIND_DATA", "Initial load completed but change tracking not established");
        }
    } else if (operation_type == "delta_fetch") {
        if (!result.extracted_delta_token.empty()) {
            staged_delta_token_ = result.extracted_delta_token;
            staged_operation_type_ = operation_type;
            staged_preference_applied_ = false;
            has_staged_delta_token_ = true;
            ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Staged delta token from delta fetch, pending scan completion");
        }
    }
}

void OdpODataReadBindData::FinalizeScan() {
    CommitStagedDeltaToken();
    WriteAuditTotals();
}

void OdpODataReadBindData::WriteAuditTotals() {
    if (current_audit_id_ <= 0 || audit_written_) {
        return;
    }
    // Written once per extraction, with the totals the scan actually delivered. Guarded so
    // a scan finalised more than once does not overwrite a complete row with a second,
    // emptier one.
    audit_written_ = true;
    // Only the totals. UpdateAuditEntry assigns every column, so using it here blanked the
    // delta_token_after and status that the page-boundary updates had recorded - the field
    // that says which package this audit row is about.
    state_manager_->UpdateAuditTotals(current_audit_id_, audit_rows_fetched_,
                                      audit_package_size_bytes_);
    ERPL_TRACE_DEBUG("ODP_BIND_DATA",
                     duckdb::StringUtil::Format("Wrote audit totals: %lld rows, %lld bytes",
                                                (long long)audit_rows_fetched_,
                                                (long long)audit_package_size_bytes_));
}

void OdpODataReadBindData::CommitStagedDeltaToken() {
    if (!has_staged_delta_token_) {
        return;
    }

    ERPL_TRACE_INFO("ODP_BIND_DATA",
                    "Scan drained; committing staged delta token for " + staged_operation_type_);

    const auto token = staged_delta_token_;
    const auto operation_type = staged_operation_type_;
    const auto preference_applied = staged_preference_applied_;

    // Persist FIRST. Clearing the staging fields up front would discard the token if the
    // write then failed, and the next read would re-extract from the previous position.
    // The flag is cleared only once the write has succeeded, which also makes a second
    // call a no-op when the scan is drained more than once.
    if (operation_type == "initial_load") {
        state_manager_->TransitionToDeltaFetch(token, preference_applied);
    } else {
        state_manager_->UpdateDeltaToken(token);
    }

    has_staged_delta_token_ = false;
    staged_delta_token_.clear();
    staged_operation_type_.clear();
}

void OdpODataReadBindData::UpdateODataClientWithResponse(const std::string& url, const std::string& response_content) {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Updating OData client with pre-fetched response for URL: " + url);
    
    try {
        // Create OData client factory probe result
        ODataClientFactory::ProbeResult probe_result;
        probe_result.normalized_url = HttpUrl(url);
        probe_result.auth_params = auth_params_;
        probe_result.initial_content = response_content;
        probe_result.version = ODataVersion::V2; // ODP is always OData v2
        probe_result.is_service_root = false;
        
        // Create entity set client
        auto client = ODataClientFactory::CreateEntitySetClient(probe_result);
        
        // Create bind data with pre-fetched content
        odata_bind_data_ = ODataReadBindData::FromEntitySetClient(client, response_content);
        
        if (!odata_bind_data_) {
            throw duckdb::InternalException("Failed to update OData client with response");
        }

        // The replacement carries no column selection, so it has to be re-applied here -
        // at the single place the instance is replaced - rather than at each call site.
        // FetchAndLoadNextPage did re-apply it; HandleInitialLoad and HandleDeltaFetch did
        // not, so the FIRST page of every scan ran in all-columns mode and an explicitly
        // projected column came back NULL for every row, with the row count still correct
        // (GitHub #179).
        if (!active_column_ids_.empty()) {
            odata_bind_data_->ActivateColumns(active_column_ids_);
        }

        ERPL_TRACE_DEBUG("ODP_BIND_DATA", "OData client updated successfully with pre-fetched response");
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Failed to update OData client with response: " + std::string(e.what()));
        throw;
    }
}

void OdpODataReadBindData::HandleRequestError(const std::exception& error, const std::string& operation_type) {
    ERPL_TRACE_ERROR("ODP_BIND_DATA", duckdb::StringUtil::Format(
        "Handling error in %s: %s", operation_type, error.what()));
    
    // Update audit entry with error
    if (current_audit_id_ > 0) {
        state_manager_->UpdateAuditEntry(current_audit_id_, std::nullopt, 0, 0, "", error.what());
    }
    
    // Transition to error state
    state_manager_->TransitionToError(error.what());
}

bool OdpODataReadBindData::IsTokenError(const std::exception& error) const {
    // Substring-matching the message text used to classify almost any failure as
    // a token error -- a network message mentioning "delta" was enough -- and
    // that discards a perfectly good delta token and re-extracts everything
    // (#94). Only a structured signal counts now.
    const auto* http_error = dynamic_cast<const OdpHttpException*>(&error);
    if (!http_error) {
        return false;
    }

    // 410 Gone is the OData answer to a delta link that is no longer valid.
    if (http_error->HttpStatusCode() == 410) {
        return true;
    }

    // SAP reports an invalid or expired delta token through the error code in
    // the payload; the code text carries DELTATOKEN.
    std::string code = http_error->SapErrorCode();
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return code.find("DELTATOKEN") != std::string::npos;
}

void OdpODataReadBindData::LogCurrentState() const {
    ERPL_TRACE_INFO("ODP_BIND_DATA", duckdb::StringUtil::Format(
        "ODP Bind Data State - Subscription: %s, Phase: %s, Initialized: %s, FirstFetch: %s",
        GetSubscriptionId(),
        OdpSubscriptionStateManager::PhaseToString(GetCurrentPhase()),
        initialized_ ? "YES" : "NO",
        first_fetch_completed_ ? "YES" : "NO"));
}

std::string OdpODataReadBindData::ExtractEntitySetName(const std::string& url) {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Extracting entity set name from URL: " + url);
    
    // Extract the last path segment as entity set name
    std::regex entity_set_regex(R"(/([^/\?]+)(?:\?|$))");
    std::smatch match;
    
    if (std::regex_search(url, match, entity_set_regex)) {
        std::string entity_set_name = match[1].str();
        ERPL_TRACE_DEBUG("ODP_BIND_DATA", "Extracted entity set name: " + entity_set_name);
        return entity_set_name;
    }
    
    // Fallback: use the full URL as entity set name (cleaned)
    std::string cleaned_name = OdpSubscriptionRepository::CleanUrlForId(url);
    ERPL_TRACE_WARN("ODP_BIND_DATA", "Could not extract entity set name, using cleaned URL: " + cleaned_name);
    return cleaned_name;
}

ODataReadBindData& OdpODataReadBindData::GetODataBindData() {
    if (!odata_bind_data_) {
        throw duckdb::InternalException("OData bind data not initialized");
    }
    return *odata_bind_data_;
}

const ODataReadBindData& OdpODataReadBindData::GetODataBindData() const {
    if (!odata_bind_data_) {
        throw duckdb::InternalException("OData bind data not initialized");
    }
    return *odata_bind_data_;
}

void OdpODataReadBindData::ProcessScanResult(const duckdb::DataChunk& output) {
    ERPL_TRACE_DEBUG("ODP_BIND_DATA", duckdb::StringUtil::Format(
        "Processing scan result with %zu rows", output.size()));
    
    try {
        // Update audit entry with row count
        if (current_audit_id_ > 0) {
            state_manager_->UpdateAuditEntry(current_audit_id_, std::nullopt, 
                                           static_cast<int64_t>(output.size()), 0);
        }
        
        // Check if this completes a fetch operation
        if (!first_fetch_completed_) {
            first_fetch_completed_ = true;
            
            // If we're in initial load and got data, we might need to transition to delta fetch
            if (state_manager_->GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::INITIAL_LOAD) {
                // The transition will be handled by the request orchestrator when it extracts delta tokens
                ERPL_TRACE_DEBUG("ODP_BIND_DATA", "First fetch completed in initial load phase");
            }
        }
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_BIND_DATA", "Error processing scan result: " + std::string(e.what()));
        // Don't throw - this is just for tracking, shouldn't break the scan
    }
}

void OdpODataReadBindData::FetchAndLoadNextPage() {
    D_ASSERT(!pending_next_url_.empty());

    const std::string url_to_fetch = pending_next_url_;
    ERPL_TRACE_INFO("ODP_BIND_DATA", "Fetching next ODP page: " + url_to_fetch);

    auto next_result = request_orchestrator_->ExecuteNextPage(url_to_fetch);
    if (!next_result.response) {
        // Ending pagination here would hand the user a partial extraction presented as a
        // complete one, and the staged delta token would then be committed over rows that
        // were never delivered. Fail loudly instead. See GitHub #93.
        pending_next_url_ = "";
        has_staged_delta_token_ = false;
        staged_delta_token_.clear();
        throw duckdb::IOException(
            "ODP pagination failed: no response for next page '" + url_to_fetch +
            "'. The extraction is incomplete and the delta token was not advanced, so the "
            "subscription still points at the last position it completed. Re-running resumes "
            "from there rather than re-extracting in full.");
    }

    // Determine whether there is yet another page after this one.
    auto further_next = next_result.response->NextUrl();
    pending_next_url_ = (further_next.has_value() && !further_next->empty() && !IsDeltaLink(*further_next))
        ? further_next.value() : "";

    // When this is the last page, perform the state transition that was deferred
    // in HandleInitialLoad / HandleDeltaFetch.
    if (pending_next_url_.empty()) {
        // Extract and normalize the delta token from this final page.
        std::string raw_token    = OdpRequestOrchestrator::ExtractDeltaToken(*next_result.response);
        std::string delta_url    = OdpRequestOrchestrator::ExtractDeltaUrl(*next_result.response);
        std::string norm_token   = OdpRequestOrchestrator::NormalizeDeltaToken(raw_token);
        std::string norm_url     = delta_url.empty() ? "" : OdpRequestOrchestrator::NormalizeDeltaUrl(delta_url);

        if (!norm_token.empty()) {
            ERPL_TRACE_INFO("ODP_BIND_DATA", "Last page delta token: " + norm_token.substr(0, 64));
        } else {
            ERPL_TRACE_WARN("ODP_BIND_DATA", "No delta token found on last pagination page");
        }

        OdpRequestOrchestrator::OdpRequestResult last_page;
        last_page.response             = next_result.response;
        last_page.http_status_code     = next_result.http_status_code;
        last_page.response_size_bytes  = next_result.response_size_bytes;
        last_page.extracted_delta_token = norm_token;
        last_page.extracted_delta_url   = norm_url;

        if (initial_load_in_progress_) {
            initial_load_in_progress_  = false;
            // Carry forward the preference the SERVER confirmed on the first page. Deriving it
            // from "we got a token" here would re-introduce the silent-data-loss path that
            // GitHub #97 closed: a token without change tracking transitions the subscription to
            // DELTA_FETCH over data that was never tracked.
            last_page.preference_applied = initial_load_preference_applied_;
            ProcessRequestResult(last_page, "initial_load");
        } else if (delta_fetch_in_progress_) {
            delta_fetch_in_progress_   = false;
            last_page.preference_applied = false; // unused for delta_fetch path
            ProcessRequestResult(last_page, "delta_fetch");
        }
    }

    // Reload odata_bind_data_ with the new page's JSON content.
    // Use entity_set_url_ as the canonical URL; the actual content comes from the fetched page.
    UpdateODataClientWithResponse(entity_set_url_, next_result.response->RawContent());

    // Column projection is re-applied inside UpdateODataClientWithResponse itself, so it
    // covers this call and the two in HandleInitialLoad / HandleDeltaFetch alike. It used
    // to be re-applied only here, which is why only page two onwards was ever projected
    // correctly (GitHub #179).

    ERPL_TRACE_INFO("ODP_BIND_DATA", pending_next_url_.empty()
        ? "Last ODP page loaded into odata_bind_data_"
        : "Intermediate ODP page loaded — more pages pending");
}

} // namespace erpl_web
