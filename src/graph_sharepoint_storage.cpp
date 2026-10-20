#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "graph_sharepoint_storage.hpp"
#include "graph_sharepoint_catalog.hpp"
#include "graph_excel_secret.hpp"
#include "duckdb/common/string_util.hpp"

namespace erpl_web {

// -------------------------------------------------------------------------------------------------
// SharePointTransaction Implementation
// -------------------------------------------------------------------------------------------------

SharePointTransaction::SharePointTransaction(SharePointCatalog &sp_catalog,
                                             duckdb::TransactionManager &manager,
                                             duckdb::ClientContext &context)
    : duckdb::Transaction(manager, context), sp_catalog_(sp_catalog)
{
}

SharePointTransaction::~SharePointTransaction()
{
}

void SharePointTransaction::Start()   { /* Intentionally left blank */ }
void SharePointTransaction::Commit()  { /* Intentionally left blank */ }
void SharePointTransaction::Rollback(){ /* Intentionally left blank */ }

SharePointTransaction &SharePointTransaction::Get(duckdb::ClientContext &context, duckdb::Catalog &catalog) {
	auto &sp_catalog = catalog.Cast<SharePointCatalog>();
	return duckdb::Transaction::Get(context, sp_catalog).Cast<SharePointTransaction>();
}

// -------------------------------------------------------------------------------------------------
// SharePointTransactionManager Implementation
// -------------------------------------------------------------------------------------------------

SharePointTransactionManager::SharePointTransactionManager(duckdb::AttachedDatabase &db, SharePointCatalog &sp_catalog)
    : duckdb::TransactionManager(db), sp_catalog_(sp_catalog)
{
}

duckdb::Transaction &SharePointTransactionManager::StartTransaction(duckdb::ClientContext &context) {
	auto transaction = duckdb::make_uniq<SharePointTransaction>(sp_catalog_, *this, context);
	transaction->Start();
	auto &result = *transaction;
	std::lock_guard<std::mutex> l(transaction_lock_);
	transactions_[result] = std::move(transaction);
	return result;
}

duckdb::ErrorData SharePointTransactionManager::CommitTransaction(duckdb::ClientContext &context, duckdb::Transaction &transaction) {
	auto &sp_transaction = transaction.Cast<SharePointTransaction>();
	sp_transaction.Commit();
	std::lock_guard<std::mutex> l(transaction_lock_);
	transactions_.erase(transaction);
	return duckdb::ErrorData();
}

void SharePointTransactionManager::RollbackTransaction(duckdb::Transaction &transaction) {
	auto &sp_transaction = transaction.Cast<SharePointTransaction>();
	sp_transaction.Rollback();
	std::lock_guard<std::mutex> l(transaction_lock_);
	transactions_.erase(transaction);
}

void SharePointTransactionManager::Checkpoint(duckdb::ClientContext &context, bool force) {
	/* Intentionally left blank */
}

// -------------------------------------------------------------------------------------------------
// SharePointStorageExtension Attach and TransactionManager factory
// -------------------------------------------------------------------------------------------------

static duckdb::unique_ptr<duckdb::Catalog> SharePointAttach(duckdb::optional_ptr<duckdb::StorageExtensionInfo> storage_info,
                                                             duckdb::ClientContext &context,
                                                             duckdb::AttachedDatabase &db,
                                                             const std::string &name,
                                                             duckdb::AttachInfo &info,
                                                             duckdb::AttachOptions &options)
{
	std::string secret_name;

	for (auto &entry : info.options) {
		auto lower_name = duckdb::StringUtil::Lower(entry.first);
		if (lower_name == "type" || lower_name == "read_only" || lower_name == "read_write") {
			// already handled by the core attach machinery
		} else if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else {
			throw duckdb::BinderException("Unrecognized option for SharePoint attach: %s", entry.first);
		}
	}

	auto auth_info = ResolveGraphAuth(context, secret_name);
	return duckdb::make_uniq<SharePointCatalog>(db, info.path, secret_name, auth_info.auth_params);
}

static duckdb::unique_ptr<duckdb::TransactionManager> SharePointCreateTransactionManager(duckdb::optional_ptr<duckdb::StorageExtensionInfo> storage_info,
                                                                                          duckdb::AttachedDatabase &db,
                                                                                          duckdb::Catalog &catalog)
{
	auto &sp_catalog = catalog.Cast<SharePointCatalog>();
	return duckdb::make_uniq<SharePointTransactionManager>(db, sp_catalog);
}

// -------------------------------------------------------------------------------------------------
// SharePointStorageExtension Constructor and Factory
// -------------------------------------------------------------------------------------------------

SharePointStorageExtension::SharePointStorageExtension()
{
	attach = SharePointAttach;
	create_transaction_manager = SharePointCreateTransactionManager;
}

duckdb::unique_ptr<SharePointStorageExtension> CreateSharePointStorageExtension()
{
	return duckdb::make_uniq<SharePointStorageExtension>();
}

} // namespace erpl_web
