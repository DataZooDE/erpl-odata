#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb_compat.hpp"
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

#include "datazoo/oauth2/http_client.hpp"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>

namespace erpl_web {

// -------------------------------------------------------------------------------------------------

class SharePointCatalog;  // forward declaration
class SharePointTableEntry;  // forward declaration

class SharePointSchemaEntry : public duckdb::SchemaCatalogEntry {
public:
	SharePointSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info);

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
	duckdb::optional_ptr<duckdb::CatalogEntry> GetEntry(duckdb::CatalogTransaction transaction, duckdb::CatalogType type, const std::string &name);
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

private:
	void LoadTables();

	mutable std::mutex tables_mutex;
	bool tables_loaded;
	std::unordered_map<std::string, duckdb::unique_ptr<SharePointTableEntry>> table_entries;
};

// -------------------------------------------------------------------------------------------------

class SharePointTableEntry : public duckdb::TableCatalogEntry {
public:
	SharePointTableEntry(duckdb::Catalog &catalog,
	                     duckdb::SchemaCatalogEntry &schema,
	                     duckdb::CreateTableInfo &info,
	                     const std::string &site_id,
	                     const std::string &list_id,
	                     const std::string &secret_name,
	                     std::shared_ptr<HttpAuthParams> auth_params);
	// DuckDB 2.0 makes this virtual. Keep our copy because scan binding needs
	// the column names after the base entry has consumed the constructor list.
	const duckdb::ColumnList &GetColumns() const { return entry_columns_; }

	duckdb::unique_ptr<duckdb::BaseStatistics> GetStatistics(duckdb::ClientContext &context, duckdb::column_t column_id) override;
	duckdb::TableFunction GetScanFunction(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::FunctionData> &bind_data) override;
	duckdb::TableStorageInfo GetStorageInfo(duckdb::ClientContext &context) override;
	void BindUpdateConstraints(duckdb::Binder &binder, duckdb::LogicalGet &get, duckdb::LogicalProjection &proj, duckdb::LogicalUpdate &update, duckdb::ClientContext &context) override;

	const std::string &GetListId() const { return list_id_; }
	const std::string &GetSiteId() const { return site_id_; }
	std::shared_ptr<HttpAuthParams> GetAuthParams() const { return auth_params_; }

private:
	SharePointTableEntry(duckdb::Catalog &catalog, duckdb::SchemaCatalogEntry &schema,
	                     duckdb::CreateTableInfo &info, const std::string &site_id,
	                     const std::string &list_id, const std::string &secret_name,
	                     std::shared_ptr<HttpAuthParams> auth_params, duckdb::ColumnList columns);
	duckdb::ColumnList entry_columns_;
	std::string site_id_;
	std::string list_id_;
	std::string secret_name_;
	std::shared_ptr<HttpAuthParams> auth_params_;
	std::vector<std::string> column_names_;
	std::vector<duckdb::LogicalType> column_types_;
};

// -------------------------------------------------------------------------------------------------

class SharePointCatalog : public duckdb::Catalog {
public:
	SharePointCatalog(duckdb::AttachedDatabase &db,
	                  const std::string &site_name_or_id,
	                  const std::string &secret_name,
	                  std::shared_ptr<HttpAuthParams> auth_params);
	~SharePointCatalog() = default;

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

	// SharePoint-specific accessors
	SharePointSchemaEntry &GetMainSchema();
	const std::string &GetSiteId() const;
	const std::string &GetSecretName() const;
	std::shared_ptr<HttpAuthParams> GetAuthParams() const;

protected:
	std::string site_id_;
	std::string secret_name_;
	std::shared_ptr<HttpAuthParams> auth_params_;
	duckdb::unique_ptr<SharePointSchemaEntry> main_schema_;

private:
	std::string path_;
};

} // namespace erpl_web
