#include "delta_share_storage.hpp"

namespace erpl_web {

// ATTACH is not yet implemented for Delta Sharing
// Use delta_share_show_shares/schemas/tables() table functions for discovery instead
static duckdb::unique_ptr<duckdb::Catalog> DeltaShareAttach(
	duckdb::optional_ptr<duckdb::StorageExtensionInfo> storage_info,
	duckdb::ClientContext& context,
	duckdb::AttachedDatabase& db,
	const std::string& name,
	duckdb::AttachInfo& info,
	duckdb::AttachOptions& options) {
	// Returning nullptr would hand DuckDB a catalog-less attached database; say it plainly instead.
	throw duckdb::NotImplementedException(
	    "ATTACH ... (TYPE delta_share) is not implemented yet. Use delta_share_show_shares(), "
	    "delta_share_show_schemas() and delta_share_show_tables() for discovery, and delta_share_scan() to read.");
}

// DuckDB only routes ATTACH to a storage extension when BOTH callbacks are set (database.cpp,
// CreateAttachedDatabase). With this one left as nullptr it silently ignored DeltaShareAttach and
// created a plain DuckDB file named after the path, so the error above was unreachable.
// DeltaShareAttach always throws first, so this is never invoked.
static duckdb::unique_ptr<duckdb::TransactionManager> DeltaShareCreateTransactionManager(
	duckdb::optional_ptr<duckdb::StorageExtensionInfo> storage_info,
	duckdb::AttachedDatabase& db,
	duckdb::Catalog& catalog) {
	throw duckdb::InternalException("Delta Sharing has no transaction manager: ATTACH is not implemented");
}

DeltaShareStorageExtension::DeltaShareStorageExtension() {
	attach = DeltaShareAttach;
	create_transaction_manager = DeltaShareCreateTransactionManager;
}

duckdb::unique_ptr<DeltaShareStorageExtension> CreateDeltaShareStorageExtension() {
	return duckdb::make_uniq<DeltaShareStorageExtension>();
}

} // namespace erpl_web
