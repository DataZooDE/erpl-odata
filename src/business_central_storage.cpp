#include "business_central_storage.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/attached_database.hpp"
#include "business_central_catalog.hpp"
#include "business_central_client.hpp"
#include "business_central_secret.hpp"
#include "storage_access_mode.hpp"
#include "tracing.hpp"
#include "duckdb/common/string_util.hpp"

namespace erpl_web {

// -----------------------------------------------------------------------
// BcTransaction
// -----------------------------------------------------------------------

BcTransaction::BcTransaction(BcCatalog &bc_catalog, duckdb::TransactionManager &manager, duckdb::ClientContext &context)
    : duckdb::Transaction(manager, context), bc_catalog_(bc_catalog) {
}

BcTransaction::~BcTransaction() = default;

void BcTransaction::Start() { }
void BcTransaction::Commit() { }
void BcTransaction::Rollback() { }

duckdb::optional_ptr<duckdb::CatalogEntry> BcTransaction::GetCatalogEntry(const std::string &table_name) {
    auto it = catalog_entries_.find(table_name);
    if (it != catalog_entries_.end()) {
        return it->second.get();
    }

    duckdb::CreateTableInfo info(bc_catalog_.GetMainSchema(), duckdb_compat::Ident(table_name));
    bc_catalog_.GetTableInfo(table_name, info.columns, info.constraints);
    if (info.columns.empty()) {
        return nullptr;
    }

    auto result = duckdb::make_uniq<BcTableEntry>(bc_catalog_, bc_catalog_.GetMainSchema(), info);
    auto *result_ptr = result.get();
    catalog_entries_[table_name] = std::move(result);
    return result_ptr;
}

BcTransaction &BcTransaction::Get(duckdb::ClientContext &context, duckdb::Catalog &catalog) {
    return duckdb::Transaction::Get(context, catalog).Cast<BcTransaction>();
}

// -----------------------------------------------------------------------
// BcTransactionManager
// -----------------------------------------------------------------------

BcTransactionManager::BcTransactionManager(duckdb::AttachedDatabase &db, BcCatalog &bc_catalog)
    : duckdb::TransactionManager(db), bc_catalog_(bc_catalog) {
}

duckdb::Transaction &BcTransactionManager::StartTransaction(duckdb::ClientContext &context) {
    auto transaction = duckdb::make_uniq<BcTransaction>(bc_catalog_, *this, context);
    transaction->Start();
    auto &result = *transaction;
    std::lock_guard<std::mutex> l(transaction_lock_);
    transactions_[result] = std::move(transaction);
    return result;
}

duckdb::ErrorData BcTransactionManager::CommitTransaction(duckdb::ClientContext &, duckdb::Transaction &transaction) {
    transaction.Cast<BcTransaction>().Commit();
    std::lock_guard<std::mutex> l(transaction_lock_);
    transactions_.erase(transaction);
    return duckdb::ErrorData();
}

void BcTransactionManager::RollbackTransaction(duckdb::Transaction &transaction) {
    transaction.Cast<BcTransaction>().Rollback();
    std::lock_guard<std::mutex> l(transaction_lock_);
    transactions_.erase(transaction);
}

void BcTransactionManager::Checkpoint(duckdb::ClientContext &, bool) {
    // Nothing to checkpoint for a read-only remote catalog
}

// -----------------------------------------------------------------------
// Storage extension attach/transaction callbacks
// -----------------------------------------------------------------------

static duckdb::unique_ptr<duckdb::Catalog> BcAttach(
    duckdb::optional_ptr<duckdb::StorageExtensionInfo>,
    duckdb::ClientContext &context,
    duckdb::AttachedDatabase &db,
    const std::string &,
    duckdb::AttachInfo &info,
    duckdb::AttachOptions &options)
{
    EnforceReadOnlyAttachment(db, info, options, "business_central storage is read-only");

    // info.path is the secret name (e.g. 'bc_dev')
    std::string secret_name = info.path;
    std::string company_param;

    for (auto &entry : info.options) {
        auto lower_key = duckdb::StringUtil::Lower(entry.first);
        if (lower_key == "type" || IsAccessModeOption(lower_key)) {
            // already handled by DuckDB
        } else if (lower_key == "company") {
            company_param = entry.second.ToString();
        } else {
            throw duckdb::BinderException("Unknown option for business_central attach: %s", entry.first);
        }
    }

    if (company_param.empty()) {
        throw duckdb::BinderException("COMPANY option is required for business_central attach (e.g. COMPANY 'CRONUS DE')");
    }

    ERPL_TRACE_DEBUG("BC_STORAGE", "Attaching business_central with secret=" + secret_name + " company=" + company_param);

    auto auth_info = ResolveBusinessCentralAuth(context, secret_name);

    auto company_id = BusinessCentralClientFactory::ResolveCompanyId(
        company_param, auth_info.tenant_id, auth_info.environment, auth_info.auth_params);

    auto base_url = BusinessCentralUrlBuilder::BuildApiUrl(auth_info.tenant_id, auth_info.environment);
    auto company_url = BusinessCentralUrlBuilder::BuildCompanyUrl(base_url, company_id);

    ERPL_TRACE_INFO("BC_STORAGE", "Attaching BC catalog: base=" + base_url + " company=" + company_url);

    return duckdb::make_uniq<BcCatalog>(db, base_url, company_url, auth_info.auth_params);
}

static duckdb::unique_ptr<duckdb::TransactionManager> BcCreateTransactionManager(
    duckdb::optional_ptr<duckdb::StorageExtensionInfo>,
    duckdb::AttachedDatabase &db,
    duckdb::Catalog &catalog)
{
    auto &bc_catalog = catalog.Cast<BcCatalog>();
    return duckdb::make_uniq<BcTransactionManager>(db, bc_catalog);
}

// -----------------------------------------------------------------------
// BcStorageExtension
// -----------------------------------------------------------------------

BcStorageExtension::BcStorageExtension() {
    attach = BcAttach;
    create_transaction_manager = BcCreateTransactionManager;
}

duckdb::unique_ptr<BcStorageExtension> CreateBcStorageExtension() {
    return duckdb::make_uniq<BcStorageExtension>();
}

} // namespace erpl_web
