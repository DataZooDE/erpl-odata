#include "odp_pragma_functions.hpp"
#include "odp_subscription_repository.hpp"
#include "tracing.hpp"
#include "duckdb_argument_helper.hpp"
#include "telemetry.hpp"
#include "scan_row_cursor.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

// ============================================================================
// ODP List Subscriptions Table Function
// ============================================================================

duckdb::unique_ptr<duckdb::FunctionData> OdpListSubscriptionsBind(duckdb::ClientContext &context,
                                                                  duckdb::TableFunctionBindInput &input,
                                                                  duckdb::vector<duckdb::LogicalType> &return_types,
                                                                  duckdb::vector<std::string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("odp_list_subscriptions");
    ERPL_TRACE_DEBUG("ODP_LIST_SUBSCRIPTIONS_BIND", "=== BINDING ODP_LIST_SUBSCRIPTIONS FUNCTION ===");
    
    // Set up return schema
    return_types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),    // subscription_id
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),    // service_url
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),    // entity_set_name
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),    // secret_name
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),    // delta_token
        duckdb::LogicalType(duckdb::LogicalTypeId::TIMESTAMP),  // created_at
        duckdb::LogicalType(duckdb::LogicalTypeId::TIMESTAMP),  // last_updated
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),    // subscription_status
        duckdb::LogicalType(duckdb::LogicalTypeId::BOOLEAN)     // preference_applied
    };
    
    names = {
        "subscription_id", "service_url", "entity_set_name", "secret_name", 
        "delta_token", "created_at", "last_updated", "subscription_status", "preference_applied"
    };
    
    // The subscription list is read per execution (see OdpListSubscriptionsInit), not here:
    // it changes whenever a subscription is created or removed, and a bound plan is re-executed.
    auto bind_data = duckdb::make_uniq<OdpListSubscriptionsBindData>();
    
    ERPL_TRACE_INFO("ODP_LIST_SUBSCRIPTIONS_BIND", "Bound function with 9 columns");
    return std::move(bind_data);
}

void OdpListSubscriptionsScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data, duckdb::DataChunk &output) {
    auto &state = data.global_state->Cast<OdpListSubscriptionsGlobalState>();

    ERPL_TRACE_DEBUG("ODP_LIST_SUBSCRIPTIONS_SCAN", "Starting subscription list scan");
    
    try {
        // Return subscriptions in batches
        const idx_t target = STANDARD_VECTOR_SIZE;
        idx_t start_idx = state.current_index;
        idx_t end_idx = std::min(start_idx + target, static_cast<idx_t>(state.subscriptions.size()));
        idx_t count = end_idx - start_idx;
        
        if (count == 0) {
            ERPL_TRACE_DEBUG("ODP_LIST_SUBSCRIPTIONS_SCAN", "No more subscriptions to return");
            output.SetCardinality(0);
            return;
        }
        
        ERPL_TRACE_DEBUG("ODP_LIST_SUBSCRIPTIONS_SCAN", duckdb::StringUtil::Format(
            "Returning %zu subscriptions (indices %zu to %zu)", count, start_idx, end_idx - 1));
        
        // Fill output chunk
        for (idx_t i = 0; i < count; i++) {
            idx_t row_idx = start_idx + i;
            const auto& subscription = state.subscriptions[row_idx];
            
            output.SetValue(0, i, duckdb::Value(subscription.subscription_id));
            output.SetValue(1, i, duckdb::Value(subscription.service_url));
            output.SetValue(2, i, duckdb::Value(subscription.entity_set_name));
            output.SetValue(3, i, duckdb::Value(subscription.secret_name));
            output.SetValue(4, i, subscription.delta_token.empty() ? 
                          duckdb::Value() : duckdb::Value(subscription.delta_token));
            // Convert std::chrono::time_point to duckdb::timestamp_t
            auto created_time_t = std::chrono::system_clock::to_time_t(subscription.created_at);
            auto updated_time_t = std::chrono::system_clock::to_time_t(subscription.last_updated);
            
            output.SetValue(5, i, duckdb::Value::TIMESTAMP(duckdb::Timestamp::FromEpochSeconds(created_time_t)));
            output.SetValue(6, i, duckdb::Value::TIMESTAMP(duckdb::Timestamp::FromEpochSeconds(updated_time_t)));
            output.SetValue(7, i, duckdb::Value(subscription.subscription_status));
            output.SetValue(8, i, duckdb::Value(subscription.preference_applied));
        }
        
        output.SetCardinality(count);
        state.current_index = end_idx;
        
        ERPL_TRACE_INFO("ODP_LIST_SUBSCRIPTIONS_SCAN", duckdb::StringUtil::Format("Returned %zu subscriptions", count));
        
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_LIST_SUBSCRIPTIONS_SCAN", "Scan failed: " + std::string(e.what()));
        throw;
    }
}

// ============================================================================
// ODP Remove Subscription Pragma Function
// ============================================================================

void OdpRemoveSubscriptionPragma(duckdb::ClientContext &context, const duckdb::FunctionParameters &parameters) {
    PostHogTelemetry::Instance().RecordFunctionCall("odp_remove_subscription");
    ERPL_TRACE_DEBUG("ODP_REMOVE_SUBSCRIPTION", "=== EXECUTING ODP_REMOVE_SUBSCRIPTION PRAGMA ===");
    
    if (parameters.values.empty()) {
        throw duckdb::InvalidInputException("odp_odata_remove_subscription requires at least one parameter: subscription_id");
    }
    
    // Extract subscription ID(s)
    std::vector<std::string> subscription_ids;
    if (parameters.values[0].type().id() == duckdb::LogicalTypeId::LIST) {
        // Handle list of subscription IDs
        auto list_value = parameters.values[0];
        auto list_children = duckdb::ListValue::GetChildren(list_value);
        for (const auto& child : list_children) {
            subscription_ids.push_back(child.ToString());
        }
    } else {
        // Single subscription ID
        subscription_ids.push_back(parameters.values[0].ToString());
    }
    
    // Extract keep_local_data parameter (default: false)
    bool keep_local_data = false;
    if (parameters.values.size() > 1) {
        keep_local_data = parameters.values[1].GetValue<bool>();
    }
    
    ERPL_TRACE_INFO("ODP_REMOVE_SUBSCRIPTION", duckdb::StringUtil::Format(
        "Removing %zu subscription(s), keep_local_data: %s", 
        subscription_ids.size(), keep_local_data ? "true" : "false"));
    
    try {
        OdpSubscriptionRepository repository(context);

        // Resolve every id before touching any of them: a typo must be reported, and must not leave
        // a list half-removed.
        std::vector<OdpSubscription> subscriptions;
        for (const auto& subscription_id : subscription_ids) {
            auto subscription = repository.GetSubscription(subscription_id);
            if (!subscription.has_value()) {
                throw duckdb::InvalidInputException("Unknown ODP subscription id '%s'; nothing was removed. "
                                                    "List ids with odp_odata_list_subscriptions().",
                                                    subscription_id);
            }
            subscriptions.push_back(std::move(subscription.value()));
        }

        for (const auto& subscription : subscriptions) {
            const auto& subscription_id = subscription.subscription_id;

            // Only the local record is touched; nothing is sent to SAP, so the audit entry carries
            // no HTTP status (an earlier version recorded a fabricated 200).
            OdpAuditEntry audit_entry;
            audit_entry.subscription_id = subscription_id;
            audit_entry.request_timestamp = std::chrono::system_clock::now();
            audit_entry.delta_token_before = subscription.delta_token;

            if (keep_local_data) {
                if (!repository.UpdateSubscriptionStatus(subscription_id, "terminated")) {
                    throw duckdb::IOException("Could not mark ODP subscription '%s' as terminated", subscription_id);
                }
                audit_entry.operation_type = "subscription_terminated";
            } else {
                if (!repository.RemoveSubscription(subscription_id)) {
                    throw duckdb::IOException("Could not remove ODP subscription '%s'", subscription_id);
                }
                audit_entry.operation_type = "subscription_removed";
            }

            repository.CreateAuditEntry(audit_entry);
            ERPL_TRACE_INFO("ODP_REMOVE_SUBSCRIPTION", audit_entry.operation_type + ": " + subscription_id);
        }

        ERPL_TRACE_INFO("ODP_REMOVE_SUBSCRIPTION", "Completed processing all subscriptions");

    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_REMOVE_SUBSCRIPTION", "Error: " + std::string(e.what()));
        throw;
    }
}

// ============================================================================
// Bind Data Implementation
// ============================================================================

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> OdpListSubscriptionsInit(duckdb::ClientContext &context,
                                                                             duckdb::TableFunctionInitInput &) {
    ERPL_TRACE_DEBUG("ODP_LIST_SUBSCRIPTIONS_INIT", "Loading subscriptions from repository");

    auto state = duckdb::make_uniq<OdpListSubscriptionsGlobalState>();
    try {
        OdpSubscriptionRepository repository(context);
        state->subscriptions = repository.ListAllSubscriptions();
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("ODP_LIST_SUBSCRIPTIONS_INIT", "Failed to load subscriptions: " + std::string(e.what()));
        throw;
    }

    ERPL_TRACE_INFO("ODP_LIST_SUBSCRIPTIONS_INIT", duckdb::StringUtil::Format(
        "Loaded %zu subscriptions", state->subscriptions.size()));
    return std::move(state);
}

// ============================================================================
// Function Registration
// ============================================================================

duckdb::TableFunctionSet CreateOdpListSubscriptionsFunction() {
    ERPL_TRACE_DEBUG("ODP_PRAGMA_REGISTRATION", "=== REGISTERING ODP_LIST_SUBSCRIPTIONS FUNCTION ===");
    
    duckdb::TableFunctionSet function_set("odp_odata_list_subscriptions");
    
    // Table function: odp_odata_list_subscriptions()
    duckdb::TableFunction list_function(
        {},  // No parameters
        OdpListSubscriptionsScan,
        OdpListSubscriptionsBind
    );
    
    list_function.init_global = OdpListSubscriptionsInit;

    function_set.AddFunction(list_function);
    
    ERPL_TRACE_INFO("ODP_PRAGMA_REGISTRATION", "ODP_LIST_SUBSCRIPTIONS function registered successfully");
    return function_set;
}

duckdb::PragmaFunctionSet CreateOdpRemoveSubscriptionFunction() {
    ERPL_TRACE_DEBUG("ODP_PRAGMA_REGISTRATION", "=== REGISTERING ODP_REMOVE_SUBSCRIPTION PRAGMA ===");

    // Both arities: the documented one-argument form used to fail to bind because only
    // (ANY, BOOLEAN) was registered, which made the flag mandatory.
    duckdb::PragmaFunctionSet function_set("odp_odata_remove_subscription");
    function_set.AddFunction(duckdb::PragmaFunction::PragmaCall(
        "odp_odata_remove_subscription", OdpRemoveSubscriptionPragma, {duckdb::LogicalType::ANY}));
    function_set.AddFunction(duckdb::PragmaFunction::PragmaCall(
        "odp_odata_remove_subscription", OdpRemoveSubscriptionPragma,
        {duckdb::LogicalType::ANY, duckdb::LogicalType::BOOLEAN}));

    ERPL_TRACE_INFO("ODP_PRAGMA_REGISTRATION", "ODP_REMOVE_SUBSCRIPTION pragma registered successfully");
    return function_set;
}

} // namespace erpl_web
