#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "graph_excel_storage.hpp"
#include "graph_excel_catalog.hpp"
#include "graph_excel_secret.hpp"
#include "graph_sharepoint_client.hpp"
#include "duckdb/common/string_util.hpp"

namespace erpl_web {

// -------------------------------------------------------------------------------------------------
// ExcelTransaction
// -------------------------------------------------------------------------------------------------

ExcelTransaction::ExcelTransaction(ExcelCatalog &catalog,
                                   duckdb::TransactionManager &manager,
                                   duckdb::ClientContext &context)
    : duckdb::Transaction(manager, context)
{
}

ExcelTransaction::~ExcelTransaction() {}

void ExcelTransaction::Start()    { /* Intentionally left blank */ }
void ExcelTransaction::Commit()   { /* Intentionally left blank */ }
void ExcelTransaction::Rollback() { /* Intentionally left blank */ }

ExcelTransaction &ExcelTransaction::Get(duckdb::ClientContext &context, duckdb::Catalog &catalog) {
	auto &excel_catalog = catalog.Cast<ExcelCatalog>();
	return duckdb::Transaction::Get(context, excel_catalog).Cast<ExcelTransaction>();
}

// -------------------------------------------------------------------------------------------------
// ExcelTransactionManager
// -------------------------------------------------------------------------------------------------

ExcelTransactionManager::ExcelTransactionManager(duckdb::AttachedDatabase &db, ExcelCatalog &catalog)
    : duckdb::TransactionManager(db), catalog_(catalog)
{
}

duckdb::Transaction &ExcelTransactionManager::StartTransaction(duckdb::ClientContext &context) {
	auto transaction = duckdb::make_uniq<ExcelTransaction>(catalog_, *this, context);
	transaction->Start();
	auto &result = *transaction;
	std::lock_guard<std::mutex> l(transaction_lock_);
	transactions_[result] = std::move(transaction);
	return result;
}

duckdb::ErrorData ExcelTransactionManager::CommitTransaction(duckdb::ClientContext &context,
                                                              duckdb::Transaction &transaction) {
	auto &excel_transaction = transaction.Cast<ExcelTransaction>();
	excel_transaction.Commit();
	std::lock_guard<std::mutex> l(transaction_lock_);
	transactions_.erase(transaction);
	return duckdb::ErrorData();
}

void ExcelTransactionManager::RollbackTransaction(duckdb::Transaction &transaction) {
	auto &excel_transaction = transaction.Cast<ExcelTransaction>();
	excel_transaction.Rollback();
	std::lock_guard<std::mutex> l(transaction_lock_);
	transactions_.erase(transaction);
}

void ExcelTransactionManager::Checkpoint(duckdb::ClientContext &, bool) {
	/* Intentionally left blank */
}

// -------------------------------------------------------------------------------------------------
// ExcelStorageExtension — Attach and TransactionManager factory
// -------------------------------------------------------------------------------------------------

static duckdb::unique_ptr<duckdb::Catalog> ExcelAttach(duckdb::optional_ptr<duckdb::StorageExtensionInfo>,
                                                         duckdb::ClientContext &context,
                                                         duckdb::AttachedDatabase &db,
                                                         const std::string &name,
                                                         duckdb::AttachInfo &info,
                                                         duckdb::AttachOptions &options)
{
	std::string secret_name;
	std::string drive_param;
	std::string site_param;

	for (auto &entry : info.options) {
		const auto lower_name = duckdb::StringUtil::Lower(entry.first);
		if (lower_name == "type" || lower_name == "read_only" || lower_name == "read_write") {
			// handled by core attach machinery
		} else if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "drive") {
			drive_param = entry.second.ToString();
		} else if (lower_name == "site") {
			site_param = entry.second.ToString();
		} else {
			throw duckdb::BinderException("Unrecognized option for Excel workbook attach: %s", entry.first);
		}
	}

	auto auth_info = ResolveGraphAuth(context, secret_name);

	// Resolve drive name → GUID when a site is given alongside a non-GUID drive value
	std::string drive_id = drive_param;
	if (!drive_param.empty() && !site_param.empty()) {
		GraphSharePointClient sp_client(auth_info.auth_params);
		const auto site_id = sp_client.ResolveSiteId(site_param);
		drive_id = sp_client.ResolveDriveId(site_id, drive_param);
	}

	return duckdb::make_uniq<ExcelCatalog>(db, info.path, drive_id, secret_name, auth_info.auth_params);
}

static duckdb::unique_ptr<duckdb::TransactionManager>
ExcelCreateTransactionManager(duckdb::optional_ptr<duckdb::StorageExtensionInfo>,
                               duckdb::AttachedDatabase &db,
                               duckdb::Catalog &catalog)
{
	auto &excel_catalog = catalog.Cast<ExcelCatalog>();
	return duckdb::make_uniq<ExcelTransactionManager>(db, excel_catalog);
}

// -------------------------------------------------------------------------------------------------
// ExcelStorageExtension Constructor and Factory
// -------------------------------------------------------------------------------------------------

ExcelStorageExtension::ExcelStorageExtension()
{
	attach = ExcelAttach;
	create_transaction_manager = ExcelCreateTransactionManager;
}

duckdb::unique_ptr<ExcelStorageExtension> CreateExcelStorageExtension()
{
	return duckdb::make_uniq<ExcelStorageExtension>();
}

} // namespace erpl_web
