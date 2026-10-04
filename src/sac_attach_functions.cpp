#include "sac_attach_functions.hpp"

#include "duckdb/common/exception.hpp"

namespace erpl_web {

// ATTACH ... (TYPE sac) is not implemented; see the note on SacStorageExtension.
static duckdb::unique_ptr<duckdb::Catalog> SacAttach(duckdb::optional_ptr<duckdb::StorageExtensionInfo>,
                                                     duckdb::ClientContext &, duckdb::AttachedDatabase &,
                                                     const std::string &, duckdb::AttachInfo &,
                                                     duckdb::AttachOptions &) {
    throw duckdb::NotImplementedException(
        "ATTACH ... (TYPE sac) is not implemented yet. SAC catalog discovery needs the tenant's OData service "
        "document, which this build cannot parse. Read SAC data with sac_read_planning_data(), "
        "sac_read_analytical() and sac_read_story_data() instead.");
}

// DuckDB requires this callback to be set for ATTACH to reach SacAttach at all. SacAttach always
// throws first, so it is never invoked.
static duckdb::unique_ptr<duckdb::TransactionManager> SacCreateTransactionManager(
    duckdb::optional_ptr<duckdb::StorageExtensionInfo>, duckdb::AttachedDatabase &, duckdb::Catalog &) {
    throw duckdb::InternalException("SAC has no transaction manager: ATTACH is not implemented");
}

SacStorageExtension::SacStorageExtension() {
    attach = SacAttach;
    create_transaction_manager = SacCreateTransactionManager;
}

duckdb::unique_ptr<SacStorageExtension> CreateSacStorageExtension() {
    return duckdb::make_uniq<SacStorageExtension>();
}

} // namespace erpl_web
