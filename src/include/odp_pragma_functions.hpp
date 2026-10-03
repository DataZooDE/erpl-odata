#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "odp_subscription_repository.hpp"
#include "scan_row_cursor.hpp"

namespace erpl_web {

// ============================================================================
// ODP List Subscriptions Table Function
// ============================================================================

/**
 * @brief Bind data for odp_odata_list_subscriptions: nothing to remember, the list is per execution
 */
class OdpListSubscriptionsBindData : public duckdb::TableFunctionData {
};

/**
 * @brief Per-execution state: the subscriptions as they are when the scan starts, plus the cursor
 *
 * They live here and not on the bind data because a bound plan is re-executed (GitHub #253, #75).
 */
struct OdpListSubscriptionsGlobalState : public ScanRowCursorState {
    std::vector<OdpSubscription> subscriptions;
};

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> OdpListSubscriptionsInit(duckdb::ClientContext &context,
                                                                             duckdb::TableFunctionInitInput &input);

/**
 * @brief Bind function for odp_odata_list_subscriptions table function
 * 
 * Returns all active ODP subscriptions with their current status, delta tokens,
 * and metadata. This function takes no parameters and returns a table with
 * subscription details.
 * 
 * @param context DuckDB client context
 * @param input Function binding input (no parameters expected)
 * @param return_types Output parameter for column types
 * @param names Output parameter for column names
 * @return Unique pointer to OdpListSubscriptionsBindData
 */
duckdb::unique_ptr<duckdb::FunctionData> OdpListSubscriptionsBind(duckdb::ClientContext &context, 
                                                                  duckdb::TableFunctionBindInput &input,
                                                                  duckdb::vector<duckdb::LogicalType> &return_types,
                                                                  duckdb::vector<std::string> &names);

/**
 * @brief Scan function for odp_odata_list_subscriptions table function
 * 
 * Retrieves and returns subscription data from the ODP subscription repository.
 * Returns data in batches for efficient processing.
 * 
 * @param context DuckDB client context
 * @param data Table function input with bind data
 * @param output Output data chunk to fill
 */
void OdpListSubscriptionsScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data, duckdb::DataChunk &output);

// ============================================================================
// ODP Remove Subscription Pragma Function
// ============================================================================

/**
 * @brief Pragma function for forgetting ODP subscriptions locally
 *
 * This only touches the local subscription record. It does NOT terminate the subscription in the
 * SAP system's ODQ; do that in ODQMON (see docs/ODP.md).
 *
 * Usage:
 * - PRAGMA odp_odata_remove_subscription('subscription_id')
 * - PRAGMA odp_odata_remove_subscription(['id1', 'id2'], true)
 *
 * Parameters:
 * - subscription_id: Single ID (VARCHAR) or list of IDs (LIST). Every id must exist, otherwise
 *   InvalidInputException is thrown and nothing is removed. Repeated ids are removed once. The ids
 *   are checked up front, but the removals themselves are separate writes, not one transaction.
 * - keep_local_data: Optional BOOLEAN (default: false)
 *   - false: delete the local subscription record
 *   - true: keep the local record and mark it 'terminated'
 *
 * @param context DuckDB client context
 * @param parameters Function parameters (subscription_id(s), keep_local_data)
 */
void OdpRemoveSubscriptionPragma(duckdb::ClientContext &context, const duckdb::FunctionParameters &parameters);

// ============================================================================
// Function Registration
// ============================================================================

/**
 * @brief Create the odp_odata_list_subscriptions table function set
 * 
 * Registers the table function for listing ODP subscriptions.
 * Returns a table with columns:
 * - subscription_id (VARCHAR): Unique subscription identifier
 * - service_url (VARCHAR): OData service URL
 * - entity_set_name (VARCHAR): Entity set name
 * - secret_name (VARCHAR): Authentication secret name
 * - delta_token (VARCHAR): Current delta token (nullable)
 * - created_at (TIMESTAMP): Subscription creation time
 * - last_updated (TIMESTAMP): Last update time
 * - subscription_status (VARCHAR): Current status (active, terminated, error)
 * - preference_applied (BOOLEAN): Whether change tracking is enabled
 * 
 * @return TableFunctionSet for registration with DuckDB
 */
duckdb::TableFunctionSet CreateOdpListSubscriptionsFunction();

/**
 * @brief Create the odp_odata_remove_subscription pragma function
 * 
 * Registers the pragma function for removing ODP subscriptions.
 * Supports both single subscription removal and batch operations.
 * 
 * @return PragmaFunctionSet (one- and two-argument forms) for registration with DuckDB
 */
duckdb::PragmaFunctionSet CreateOdpRemoveSubscriptionFunction();

} // namespace erpl_web
