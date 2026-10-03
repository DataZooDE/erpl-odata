#include "duckdb_argument_helper.hpp"
#include "odata_storage.hpp"
#include "odata_catalog.hpp"

#include "odata_transaction_manager.hpp"

namespace erpl_web {

// -------------------------------------------------------------------------------------------------

static duckdb::unique_ptr<duckdb::Catalog> ODataAttach(duckdb::optional_ptr<duckdb::StorageExtensionInfo> storage_info, 
                                                       duckdb::ClientContext &context,
                                                       duckdb::AttachedDatabase &db, 
                                                       const std::string &name, 
                                                       duckdb::AttachInfo &info,
                                                       duckdb::AttachOptions &options) 
{
    if (options.access_mode != duckdb::AccessMode::READ_ONLY) {
        throw duckdb::BinderException("ODATA storage extension does not support write access");
    }

    std::string ignore_pattern;
    for (auto &entry : info.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "type" || lower_name == "read_only") {
			// already handled
		} else if (lower_name == "ignore") {
			ignore_pattern = entry.second.ToString();
		} else {
			throw duckdb::BinderException("Unrecognized option for OData attach: %s", entry.first);
		}
	}

    auto auth_params = ResolveHttpAuthFromSecrets(context, info.path);
    return duckdb::make_uniq<ODataCatalog>(db, info.path, auth_params, ignore_pattern);
}

static duckdb::unique_ptr<duckdb::TransactionManager> ODataCreateTransactionManager(duckdb::optional_ptr<duckdb::StorageExtensionInfo> storage_info,
                                                                                    duckdb::AttachedDatabase &db, 
                                                                                    duckdb::Catalog &catalog) 
{
    auto &odata_catalog = catalog.Cast<ODataCatalog>();
	return duckdb::make_uniq<ODataTransactionManager>(db, odata_catalog);
}

ODataStorageExtension::ODataStorageExtension()
{ 
    attach = ODataAttach;
	create_transaction_manager = ODataCreateTransactionManager;
}

duckdb::unique_ptr<ODataStorageExtension> CreateODataStorageExtension()
{
    return duckdb::make_uniq<ODataStorageExtension>();
}

} // namespace erpl_web