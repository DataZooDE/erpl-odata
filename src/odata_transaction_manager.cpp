#include "odata_transaction_manager.hpp"
#include "duckdb_compat.hpp"

namespace erpl_web {

ODataTransaction &GetODataTransaction(duckdb::CatalogTransaction transaction) {
	if (!transaction.transaction) {
		throw InternalException("No transaction!?");
	}
	return transaction.transaction->Cast<ODataTransaction>();
}

// -------------------------------------------------------------------------------------------------
ODataTransaction::ODataTransaction(ODataCatalog &odata_catalog, duckdb::TransactionManager &manager, duckdb::ClientContext &context)
    : duckdb::Transaction(manager, context), odata_catalog(odata_catalog)
{ }

ODataTransaction::~ODataTransaction() 
{ }

void ODataTransaction::Start() { /* Intentionally left blank */ }
void ODataTransaction::Commit() { /* Intentionally left blank */ }
void ODataTransaction::Rollback() { /* Intentionally left blank */ }

optional_ptr<CatalogEntry> ODataTransaction::GetCatalogEntry(const string &entry_name) 
{
	auto entry = catalog_entries.find(entry_name);
	if (entry != catalog_entries.end()) {
		return entry->second.get();
	}

    duckdb::CreateTableInfo info(odata_catalog.GetMainSchema(), duckdb_compat::Ident(entry_name));
    odata_catalog.GetTableInfo(entry_name, info.columns, info.constraints);
    D_ASSERT(!info.columns.empty());

    auto result = duckdb::make_uniq<ODataTableEntry>(odata_catalog, odata_catalog.GetMainSchema(), info);
    auto result_ptr = result.get();
	catalog_entries[entry_name] = std::move(result);
	
    return result_ptr;
}

void ODataTransaction::DropEntry(CatalogType type, const string &table_name, bool cascade) {
    /* Intentionally left blank */
}

void ODataTransaction::ClearTableEntry(const string &table_name) {
    catalog_entries.erase(table_name);
}

// -------------------------------------------------------------------------------------------------

ODataTransactionManager::ODataTransactionManager(duckdb::AttachedDatabase &db, ODataCatalog &odata_catalog)
    : duckdb::TransactionManager(db), odata_catalog(odata_catalog)
{ }

duckdb::Transaction& ODataTransactionManager::StartTransaction(duckdb::ClientContext &context) 
{
    auto transaction = duckdb::make_uniq<ODataTransaction>(odata_catalog, *this, context);
    transaction->Start();
	auto &result = *transaction;
	std::lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

duckdb::ErrorData ODataTransactionManager::CommitTransaction(duckdb::ClientContext &context, duckdb::Transaction &transaction)
{
    auto &odata_transaction = transaction.Cast<ODataTransaction>();
	odata_transaction.Commit();
	std::lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void ODataTransactionManager::RollbackTransaction(duckdb::Transaction &transaction)
{
    auto &odata_transaction = transaction.Cast<ODataTransaction>();
	odata_transaction.Rollback();
	std::lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void ODataTransactionManager::Checkpoint(duckdb::ClientContext &context, bool force)
{
    /* Intentionally left blank */
}


} // namespace erpl_web
