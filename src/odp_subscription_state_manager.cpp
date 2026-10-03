#include "odp_subscription_state_manager.hpp"
#include "tracing.hpp"

namespace erpl_web {

OdpSubscriptionStateManager::OdpSubscriptionStateManager(duckdb::ClientContext& context,
                                                       const std::string& service_url,
                                                       const std::string& entity_set_name,
                                                       const std::string& secret_name,
                                                       bool force_full_load,
                                                       const std::string& import_delta_token)
    : repository_(std::make_unique<OdpSubscriptionRepository>(context))
    , current_phase_(SubscriptionPhase::INITIAL_LOAD)
    , service_url_(service_url)
    , entity_set_name_(entity_set_name)
    , secret_name_(secret_name.empty() ? "default" : secret_name)
    , force_full_load_(force_full_load)
    , import_delta_token_(import_delta_token)
    , current_audit_id_(-1)
    , operation_start_time_(std::chrono::system_clock::now())
{
    ERPL_TRACE_INFO("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Initializing state manager for URL: %s, Entity: %s, Secret: %s, ForceFullLoad: %s",
        service_url_, entity_set_name_, secret_name_, force_full_load_ ? "true" : "false"));
    
    InitializeSubscription();
    LogCurrentState();
}

std::string OdpSubscriptionStateManager::GetCurrentDeltaToken() const {
    return current_subscription_.delta_token;
}

bool OdpSubscriptionStateManager::IsSubscriptionActive() const {
    return current_subscription_.subscription_status == "active" && 
           current_phase_ != SubscriptionPhase::TERMINATED &&
           current_phase_ != SubscriptionPhase::ERROR_STATE;
}

void OdpSubscriptionStateManager::TransitionToInitialLoad() {
    ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Transitioning to INITIAL_LOAD phase");
    
    current_phase_ = SubscriptionPhase::INITIAL_LOAD;
    // Clear the token in the database as well, not just in memory: the next
    // advance compares against the stored value, and an in-memory-only clear
    // would make that comparison fail against a token nobody changed.
    if (!current_subscription_.delta_token.empty()) {
        UpdateDeltaToken("");
    }
    current_subscription_.preference_applied = false;
    
    UpdateSubscriptionStatus("active");
    LogCurrentState();
}

void OdpSubscriptionStateManager::TransitionToDeltaFetch(const std::string& delta_token, bool preference_applied) {
    ERPL_TRACE_INFO("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Transitioning to DELTA_FETCH phase with token: %s, PreferenceApplied: %s",
        delta_token, preference_applied ? "true" : "false"));
    
    current_phase_ = SubscriptionPhase::DELTA_FETCH;
    current_subscription_.preference_applied = preference_applied;

    // UpdateDeltaToken takes the currently-held token as the compare-and-swap
    // expectation, so the new one must NOT be assigned first - doing so compares the
    // new value against the stored old one and reports a phantom concurrent modification
    // on every transition. UpdateDeltaToken assigns it once the swap succeeds.
    UpdateDeltaToken(delta_token);
    LogCurrentState();
}

void OdpSubscriptionStateManager::TransitionToTerminated() {
    ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Transitioning to TERMINATED phase");
    
    current_phase_ = SubscriptionPhase::TERMINATED;
    UpdateSubscriptionStatus("terminated");
    LogCurrentState();
}

void OdpSubscriptionStateManager::TransitionToError(const std::string& error_msg) {
    // Best-effort: this runs from catch blocks, so it must never fail louder than the error it is
    // reporting. A throw here REPLACES the real cause - and as an InternalException it also
    // invalidated the database instance - when, for instance, another connection removed the
    // subscription while a scan was running (GitHub #251).
    if (current_phase_ == SubscriptionPhase::ERROR_STATE) {
        // The read path reports one failure twice (the handler, then the catch around the fetch).
        ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", "Already in ERROR_STATE, ignoring: " + error_msg);
        return;
    }
    ERPL_TRACE_ERROR("ODP_STATE_MANAGER", "Transitioning to ERROR_STATE: " + error_msg);

    current_phase_ = SubscriptionPhase::ERROR_STATE;

    // Independent attempts: the status update fails when another connection removed the subscription,
    // and that must not stop the audit row from recording the real cause.
    try {
        UpdateSubscriptionStatus("error");
    } catch (const std::exception& e) {
        ERPL_TRACE_WARN("ODP_STATE_MANAGER", "Could not persist ERROR_STATE for subscription " +
                                                 current_subscription_.subscription_id + ": " + e.what());
    }
    try {
        if (current_audit_id_ > 0) {
            UpdateAuditEntry(current_audit_id_, std::nullopt, 0, 0, "", error_msg);
        }
    } catch (const std::exception& e) {
        ERPL_TRACE_WARN("ODP_STATE_MANAGER", "Could not record the error in audit entry " +
                                                 std::to_string(current_audit_id_) + ": " + e.what());
    }

    LogCurrentState();
}

void OdpSubscriptionStateManager::UpdateDeltaToken(const std::string& token) {
    const std::string expected_token = current_subscription_.delta_token;
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", "Advancing delta token for subscription " +
                                          current_subscription_.subscription_id);

    // Compare-and-swap: the stored token must still be the one this session read,
    // otherwise another session is reading the same delta stream and advancing
    // here would split the changes between the two readers.
    repository_->AdvanceDeltaToken(current_subscription_.subscription_id, expected_token, token);

    current_subscription_.delta_token = token;
    UpdateLastModified();
}

void OdpSubscriptionStateManager::UpdateSubscriptionStatus(const std::string& status) {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Updating subscription status from '%s' to '%s'", current_subscription_.subscription_status, status));
    
    current_subscription_.subscription_status = status;
    UpdateLastModified();
    
    try {
        bool success = repository_->UpdateSubscriptionStatus(current_subscription_.subscription_id, status);
        if (!success) {
            throw duckdb::IOException("Failed to update subscription status in database: subscription '%s' no longer exists",
                                      current_subscription_.subscription_id);
        }
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_STATE_MANAGER", "Error updating subscription status: " + std::string(e.what()));
        throw;
    }
}

int64_t OdpSubscriptionStateManager::CreateAuditEntry(const std::string& operation_type, const std::string& request_url) {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Creating audit entry for operation: %s, URL: %s", operation_type, request_url));
    
    operation_start_time_ = std::chrono::system_clock::now();
    
    OdpAuditEntry entry(current_subscription_.subscription_id, operation_type);
    entry.request_url = request_url;
    entry.delta_token_before = current_subscription_.delta_token;
    
    try {
        current_audit_id_ = repository_->CreateAuditEntry(entry);
        if (current_audit_id_ <= 0) {
            ERPL_TRACE_WARN("ODP_STATE_MANAGER", "Failed to create audit entry");
        } else {
            ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", duckdb::StringUtil::Format("Created audit entry with ID: %lld", current_audit_id_));
        }
        return current_audit_id_;
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_STATE_MANAGER", "Error creating audit entry: " + std::string(e.what()));
        return -1;
    }
}

void OdpSubscriptionStateManager::UpdateAuditTotals(int64_t audit_id, int64_t rows_fetched,
                                                    int64_t package_size_bytes) {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER",
                     duckdb::StringUtil::Format("Recording audit totals for %lld: Rows=%lld, Size=%lld",
                                                audit_id, rows_fetched, package_size_bytes));
    try {
        repository_->UpdateAuditTotals(audit_id, rows_fetched, package_size_bytes);
    } catch (const std::exception& e) {
        // An audit row is a record of the extraction, not part of it; failing to write it
        // must not fail a scan that delivered its rows.
        ERPL_TRACE_ERROR("ODP_STATE_MANAGER",
                         "Failed to record audit totals: " + std::string(e.what()));
    }
}

void OdpSubscriptionStateManager::UpdateAuditEntry(int64_t audit_id,
                                                  const std::optional<int>& http_status_code,
                                                  int64_t rows_fetched,
                                                  int64_t package_size_bytes,
                                                  const std::string& delta_token_after,
                                                  const std::string& error_message,
                                                  const std::optional<int64_t>& duration_ms) {
    
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Updating audit entry %lld: Status=%s, Rows=%lld, Size=%lld",
        audit_id,
        http_status_code.has_value() ? std::to_string(http_status_code.value()) : "N/A",
        rows_fetched,
        package_size_bytes));
    
    try {
        OdpAuditEntry entry;
        entry.audit_id = audit_id;
        entry.subscription_id = current_subscription_.subscription_id;
        entry.response_timestamp = std::chrono::system_clock::now();
        entry.http_status_code = http_status_code;
        entry.rows_fetched = rows_fetched;
        entry.package_size_bytes = package_size_bytes;
        entry.delta_token_after = delta_token_after;
        entry.error_message = error_message;
        
        // Calculate duration if not provided
        if (duration_ms.has_value()) {
            entry.duration_ms = duration_ms;
        } else {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.response_timestamp.value() - operation_start_time_);
            entry.duration_ms = duration.count();
        }
        
        bool success = repository_->UpdateAuditEntry(entry);
        if (!success) {
            ERPL_TRACE_WARN("ODP_STATE_MANAGER", "Failed to update audit entry");
        }
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_STATE_MANAGER", "Error updating audit entry: " + std::string(e.what()));
    }
}

std::string OdpSubscriptionStateManager::PhaseToString(SubscriptionPhase phase) {
    switch (phase) {
        case SubscriptionPhase::INITIAL_LOAD: return "INITIAL_LOAD";
        case SubscriptionPhase::DELTA_FETCH: return "DELTA_FETCH";
        case SubscriptionPhase::TERMINATED: return "TERMINATED";
        case SubscriptionPhase::ERROR_STATE: return "ERROR_STATE";
        default: return "UNKNOWN";
    }
}

void OdpSubscriptionStateManager::LogCurrentState() const {
    ERPL_TRACE_INFO("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Current state - ID: %s, Phase: %s, Status: %s, DeltaToken: %s, PreferenceApplied: %s",
        current_subscription_.subscription_id,
        PhaseToString(current_phase_),
        current_subscription_.subscription_status,
        current_subscription_.delta_token.empty() ? "NONE" : current_subscription_.delta_token.substr(0, 20) + "...",
        current_subscription_.preference_applied ? "true" : "false"));
}

// ============================================================================
// Private Methods
// ============================================================================

void OdpSubscriptionStateManager::InitializeSubscription() {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", "Initializing subscription");
    
    ValidateSubscriptionData();
    
    if (force_full_load_) {
        ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Force full load requested, creating new subscription");
        CreateNewSubscription();
    } else {
        LoadExistingSubscription();
    }
    
    DetermineInitialPhase();
}

void OdpSubscriptionStateManager::LoadExistingSubscription() {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", "Attempting to load existing subscription");
    
    auto existing = repository_->FindActiveSubscription(service_url_, entity_set_name_, secret_name_);
    if (existing.has_value()) {
        ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Found existing subscription: " + existing->subscription_id);
        current_subscription_ = existing.value();
        
        // Handle imported delta token
        if (!import_delta_token_.empty()) {
            ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Importing delta token for existing subscription");
            // UpdateDeltaToken compares against the token currently held, so the
            // in-memory copy must not be overwritten before the swap.
            UpdateDeltaToken(import_delta_token_);
        }
    } else {
        ERPL_TRACE_INFO("ODP_STATE_MANAGER", "No existing subscription found, creating new one");
        CreateNewSubscription();
    }
}

void OdpSubscriptionStateManager::CreateNewSubscription() {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", "Creating new subscription");
    
    try {
        std::string subscription_id = repository_->CreateSubscription(service_url_, entity_set_name_, secret_name_);
        
        auto subscription = repository_->GetSubscription(subscription_id);
        if (!subscription.has_value()) {
            throw duckdb::IOException("Failed to retrieve newly created ODP subscription '%s'", subscription_id);
        }
        
        current_subscription_ = subscription.value();
        
        // Handle imported delta token for new subscription
        if (!import_delta_token_.empty()) {
            ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Setting imported delta token on new subscription");
            UpdateDeltaToken(import_delta_token_);
        }
        
        ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Created new subscription: " + subscription_id);
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_STATE_MANAGER", "Failed to create subscription: " + std::string(e.what()));
        throw;
    }
}

void OdpSubscriptionStateManager::DetermineInitialPhase() {
    ERPL_TRACE_DEBUG("ODP_STATE_MANAGER", "Determining initial phase");
    
    ERPL_TRACE_INFO("ODP_STATE_MANAGER", duckdb::StringUtil::Format(
        "Existing token on load: %s",
        current_subscription_.delta_token.empty() ? "<EMPTY>" : current_subscription_.delta_token.substr(0, 64)));

    if (force_full_load_ || current_subscription_.delta_token.empty()) {
        current_phase_ = SubscriptionPhase::INITIAL_LOAD;
        ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Initial phase: INITIAL_LOAD");
    } else {
        current_phase_ = SubscriptionPhase::DELTA_FETCH;
        ERPL_TRACE_INFO("ODP_STATE_MANAGER", "Initial phase: DELTA_FETCH (existing token found)");
    }
}

void OdpSubscriptionStateManager::ValidateSubscriptionData() const {
    if (service_url_.empty()) {
        throw duckdb::InvalidInputException("Service URL cannot be empty");
    }
    
    if (entity_set_name_.empty()) {
        throw duckdb::InvalidInputException("Entity set name cannot be empty");
    }
    
    // The EntityOf*/FactsOf*/AttrOf* naming is a convention, not a guarantee --
    // an unexpected name is worth a warning, not a refusal to read (#105).
    if (!OdpSubscriptionRepository::IsValidOdpUrl(service_url_)) {
        ERPL_TRACE_WARN("ODP_STATE_MANAGER",
            "Service URL does not look like an ODP entity set (expected EntityOf*/FactsOf*/AttrOf*): " + service_url_);
    }

    if (!import_delta_token_.empty()) {
        if (force_full_load_) {
            // Silently discarding the token here left the caller believing they
            // had resumed a delta stream when they had restarted it (#105).
            throw duckdb::InvalidInputException(
                "force_full_load and import_delta_token cannot be combined: a full load starts a new change stream "
                "and would discard the imported token. Pass one or the other.");
        }
        ValidateDeltaTokenShape(import_delta_token_);
    }
}

void OdpSubscriptionStateManager::ValidateDeltaTokenShape(const std::string& token) {
    constexpr size_t MAX_DELTA_TOKEN_LENGTH = 512;

    if (token.length() > MAX_DELTA_TOKEN_LENGTH) {
        throw duckdb::InvalidInputException(duckdb::StringUtil::Format(
            "import_delta_token is %zu characters long; ODP delta tokens are at most %zu.",
            token.length(), MAX_DELTA_TOKEN_LENGTH));
    }

    for (const char c : token) {
        const bool is_allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '_' || c == '-' || c == '.' || c == ':' || c == '+' || c == '/' ||
                                c == '=' || c == '~';
        if (!is_allowed) {
            throw duckdb::InvalidInputException(
                "import_delta_token contains characters that are not valid in an ODP delta token. Expected only "
                "letters, digits and _-.:+/=~ ; pass the token exactly as odp_list_subscriptions() reports it.");
        }
    }
}

void OdpSubscriptionStateManager::UpdateLastModified() {
    current_subscription_.last_updated = std::chrono::system_clock::now();
}

} // namespace erpl_web
