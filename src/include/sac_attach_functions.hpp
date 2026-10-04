#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace erpl_web {

/**
 * SAC Storage Extension
 *
 * ATTACH ... (TYPE sac) is not implemented: SAC's catalog wire format cannot be verified without a
 * real tenant response (GitHub #243), so there is nothing to build an attached catalog from. The
 * extension is still registered, and fails the ATTACH with a clear error, for two reasons:
 *
 *  - StorageExtension does not initialise its callbacks, so an empty constructor left `attach` and
 *    `create_transaction_manager` as uninitialised pointers. DuckDB then opened a plain database file
 *    named after the URL or dereferenced garbage, depending on heap contents (GitHub #252).
 *  - DuckDB only routes ATTACH to a storage extension when BOTH callbacks are set.
 */
class SacStorageExtension : public duckdb::StorageExtension {
public:
    SacStorageExtension();
};

/**
 * Create SAC Storage Extension instance
 */
duckdb::unique_ptr<SacStorageExtension> CreateSacStorageExtension();

} // namespace erpl_web
