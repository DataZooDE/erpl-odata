#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"

namespace erpl_web {

/**
 * @brief Bind function for odp_odata_read table function
 * 
 * Validates parameters and creates OdpODataReadBindData with proper initialization.
 * Supports URL validation, delta token management, and subscription lifecycle.
 * 
 * @param context DuckDB client context
 * @param input Function binding input with parameters
 * @param return_types Output parameter for column types
 * @param names Output parameter for column names
 * @return Unique pointer to OdpODataReadBindData
 */
duckdb::unique_ptr<duckdb::FunctionData> OdpODataReadBind(duckdb::ClientContext &context, 
                                                          duckdb::TableFunctionBindInput &input,
                                                          duckdb::vector<duckdb::LogicalType> &return_types,
                                                          duckdb_compat::BindNames &names);

/**
 * @brief Scan function for odp_odata_read table function
 * 
 * Delegates to OData scan while managing ODP-specific delta token logic.
 * Handles automatic subscription state transitions and audit logging.
 * 
 * @param context DuckDB client context
 * @param data Table function input data
 * @param output Output data chunk to populate
 */
void OdpODataReadScan(duckdb::ClientContext &context, duckdb::TableFunctionInput &data, duckdb::DataChunk &output);

/**
 * @brief Initialize global state for odp_odata_read table function
 * 
 * Sets up global state by delegating to OData initialization while
 * maintaining ODP subscription context.
 * 
 * @param context DuckDB client context
 * @param input Table function initialization input
 * @return Unique pointer to global table function state
 */
duckdb::unique_ptr<duckdb::GlobalTableFunctionState> OdpODataReadInitGlobalState(duckdb::ClientContext &context,
                                                                                 duckdb::TableFunctionInitInput &input);

/**
 * @brief Progress reporting for odp_odata_read table function
 * 
 * Reports scan progress by delegating to the underlying OData bind data.
 * 
 * @param context DuckDB client context
 * @param func_data Function bind data
 * @param global_state Global table function state (unused)
 * @return Progress fraction (0.0 to 1.0)
 */
double OdpODataReadProgress(duckdb::ClientContext &context, const duckdb::FunctionData *func_data,
                           const duckdb::GlobalTableFunctionState *global_state);

/**
 * @brief Create the odp_odata_read table function set
 * 
 * Registers the ODP OData read function with support for:
 * - Required parameter: entity_set_url (VARCHAR)
 * - Optional named parameters:
 *   - secret (VARCHAR): DuckDB secret name for authentication
 *   - force_full_load (BOOLEAN): Force full reload instead of delta
 *   - import_delta_token (VARCHAR): Import existing delta token
 *   - max_page_size (UBIGINT): Override default page size (validated, must be > 0)
 * 
 * @return TableFunctionSet for registration with DuckDB
 */
duckdb::TableFunctionSet CreateOdpODataReadFunction();

} // namespace erpl_web
