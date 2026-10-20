#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

#include "odata_catalog.hpp"
#include "odata_client.hpp"
#include "datazoo/oauth2/http_client.hpp"

#include <unordered_map>
#include <mutex>
#include <string>
#include <memory>

using namespace duckdb;

namespace erpl_web {

class BcCatalog; // forward declaration

// -----------------------------------------------------------------------
// BcTableEntry — overrides GetScanFunction to use BC URL logic
// -----------------------------------------------------------------------

class BcTableEntry : public ODataTableEntry {
public:
    BcTableEntry(duckdb::Catalog &catalog, duckdb::SchemaCatalogEntry &schema, duckdb::CreateTableInfo &info);

    duckdb::TableFunction GetScanFunction(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::FunctionData> &bind_data) override;
};

// -----------------------------------------------------------------------
// BcSchemaEntry — standalone schema entry that creates BcTableEntry
// -----------------------------------------------------------------------

class BcSchemaEntry : public duckdb::SchemaCatalogEntry {
public:
    BcSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info);

    optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
    optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
    optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info, TableCatalogEntry &table) override;
    optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
    optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
    optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction, CreateTableFunctionInfo &info) override;
    optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction, CreateCopyFunctionInfo &info) override;
    optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction, CreatePragmaFunctionInfo &info) override;
    optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
    optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
    void Alter(CatalogTransaction transaction, AlterInfo &info) override;
    void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
    void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
    void DropEntry(ClientContext &context, DropInfo &info) override;
    optional_ptr<CatalogEntry> GetEntry(CatalogTransaction transaction, CatalogType type, const string &name);
    optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

    void GetTableInfo(const std::string &table_name,
                      duckdb::ColumnList &columns,
                      std::vector<duckdb::unique_ptr<duckdb::Constraint>> &constraints);

private:
    void LoadTables();

    mutable std::mutex tables_mutex;
    bool tables_loaded = false;
    std::unordered_map<std::string, duckdb::unique_ptr<BcTableEntry>> table_entries;
};

// -----------------------------------------------------------------------
// BcCatalog — top-level catalog for a single BC company attachment
// -----------------------------------------------------------------------

class BcCatalog : public duckdb::Catalog {
public:
    BcCatalog(duckdb::AttachedDatabase &db,
              const std::string &base_url,
              const std::string &company_url,
              std::shared_ptr<HttpAuthParams> auth_params);
    ~BcCatalog() = default;

    duckdb::string GetCatalogType() override;

    void Initialize(bool load_builtin) override;
    void Initialize(duckdb::optional_ptr<duckdb::ClientContext> context, bool load_builtin) override;
    void FinalizeLoad(duckdb::optional_ptr<duckdb::ClientContext> context) override;
    duckdb::string GetDBPath() override;
    bool InMemory() override;
    bool SupportsTimeTravel() const override;
    duckdb_compat::DefaultSchemaName GetDefaultSchema() const override;
    duckdb::DatabaseSize GetDatabaseSize(duckdb::ClientContext &context) override;
    duckdb::vector<duckdb::MetadataBlockInfo> GetMetadataInfo(duckdb::ClientContext &context) override;

    duckdb::optional_ptr<duckdb::SchemaCatalogEntry> LookupSchema(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info, duckdb::OnEntryNotFound if_not_found) override;
    duckdb::optional_ptr<duckdb::CatalogEntry> CreateSchema(duckdb::CatalogTransaction transaction, duckdb::CreateSchemaInfo &info) override;
    void ScanSchemas(duckdb::ClientContext &context, std::function<void(duckdb::SchemaCatalogEntry &)> callback) override;
    duckdb::optional_ptr<duckdb::SchemaCatalogEntry> GetSchema(duckdb::CatalogTransaction transaction, const std::string &schema_name, duckdb::OnEntryNotFound if_not_found, duckdb::QueryErrorContext error_context = duckdb::QueryErrorContext());

    duckdb::PhysicalOperator &PlanCreateTableAs(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalCreateTable &op, duckdb::PhysicalOperator &plan) override;
    duckdb::PhysicalOperator &PlanInsert(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalInsert &op, duckdb::optional_ptr<duckdb::PhysicalOperator> plan) override;
    duckdb::PhysicalOperator &PlanDelete(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalDelete &op, duckdb::PhysicalOperator &plan) override;
    duckdb::PhysicalOperator &PlanUpdate(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalUpdate &op, duckdb::PhysicalOperator &plan) override;
    duckdb::unique_ptr<duckdb::LogicalOperator> BindCreateIndex(duckdb::Binder &binder, duckdb::CreateStatement &stmt, duckdb::TableCatalogEntry &table, duckdb::unique_ptr<duckdb::LogicalOperator> plan) override;
    void DropSchema(duckdb::ClientContext &context, duckdb::DropInfo &info) override;

    // BC-specific accessors used by BcTableEntry::GetScanFunction and BcTransaction
    std::string CompanyUrl() const;
    std::string MetadataContextUrl() const;
    ODataServiceClient &GetServiceClient();
    BcSchemaEntry &GetMainSchema();

    void GetTableInfo(const std::string &table_name,
                      duckdb::ColumnList &columns,
                      std::vector<duckdb::unique_ptr<duckdb::Constraint>> &constraints);

private:
    ODataServiceClient service_client_;
    std::string company_url_;
    std::string metadata_url_;
    std::string path_;
    std::unique_ptr<BcSchemaEntry> main_schema_;
};

} // namespace erpl_web
