#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
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
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/storage/database_size.hpp"

#include "datazoo/oauth2/http_client.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace erpl_web {

class ExcelCatalog;     // forward
class ExcelTableEntry;  // forward

// ============================================================================
// ExcelSchemaEntry — single "main" schema that owns all table entries
// ============================================================================

class ExcelSchemaEntry : public duckdb::SchemaCatalogEntry {
public:
	ExcelSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info);

	duckdb::optional_ptr<duckdb::CatalogEntry> CreateTable(duckdb::CatalogTransaction transaction, duckdb::BoundCreateTableInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateFunction(duckdb::CatalogTransaction transaction, duckdb::CreateFunctionInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateIndex(duckdb::CatalogTransaction transaction, duckdb::CreateIndexInfo &info, duckdb::TableCatalogEntry &table) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateView(duckdb::CatalogTransaction transaction, duckdb::CreateViewInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateSequence(duckdb::CatalogTransaction transaction, duckdb::CreateSequenceInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateTableFunction(duckdb::CatalogTransaction transaction, duckdb::CreateTableFunctionInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateCopyFunction(duckdb::CatalogTransaction transaction, duckdb::CreateCopyFunctionInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreatePragmaFunction(duckdb::CatalogTransaction transaction, duckdb::CreatePragmaFunctionInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateCollation(duckdb::CatalogTransaction transaction, duckdb::CreateCollationInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateType(duckdb::CatalogTransaction transaction, duckdb::CreateTypeInfo &info) override;
	void Alter(duckdb::CatalogTransaction transaction, duckdb::AlterInfo &info) override;
	void Scan(duckdb::ClientContext &context, duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) override;
	void Scan(duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) override;
	void DropEntry(duckdb::ClientContext &context, duckdb::DropInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> LookupEntry(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info) override;

private:
	void LoadTables();

	mutable std::mutex tables_mutex_;
	bool tables_loaded_ = false;
	std::unordered_map<std::string, duckdb::unique_ptr<ExcelTableEntry>> table_entries_;
};

// ============================================================================
// ExcelTableEntry — represents a single Excel named table
// ============================================================================

class ExcelTableEntry : public duckdb::TableCatalogEntry {
public:
	ExcelTableEntry(duckdb::Catalog &catalog,
	                duckdb::SchemaCatalogEntry &schema,
	                duckdb::CreateTableInfo &info,
	                const std::string &file_path,
	                const std::string &table_name,
	                const std::string &drive_id,
	                const std::string &secret_name,
	                std::shared_ptr<HttpAuthParams> auth_params);
	// DuckDB 2.0 makes this virtual. Keep our copy because scan binding needs
	// the column names after the base entry has consumed the constructor list.
	const duckdb::ColumnList &GetColumns() const { return entry_columns_; }

	duckdb::unique_ptr<duckdb::BaseStatistics> GetStatistics(duckdb::ClientContext &context, duckdb::column_t column_id) override;
	duckdb::TableFunction GetScanFunction(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::FunctionData> &bind_data) override;
	duckdb::TableStorageInfo GetStorageInfo(duckdb::ClientContext &context) override;
	void BindUpdateConstraints(duckdb::Binder &binder, duckdb::LogicalGet &get, duckdb::LogicalProjection &proj, duckdb::LogicalUpdate &update, duckdb::ClientContext &context) override;
	duckdb::virtual_column_map_t GetVirtualColumns() const override;

	const std::string &GetFilePath()  const { return file_path_; }
	const std::string &GetTableName() const { return excel_table_name_; }
	const std::string &GetDriveId()   const { return drive_id_; }
	std::shared_ptr<HttpAuthParams> GetAuthParams() const { return auth_params_; }

private:
	ExcelTableEntry(duckdb::Catalog &catalog, duckdb::SchemaCatalogEntry &schema,
	                duckdb::CreateTableInfo &info, const std::string &file_path,
	                const std::string &table_name, const std::string &drive_id,
	                const std::string &secret_name, std::shared_ptr<HttpAuthParams> auth_params,
	                duckdb::ColumnList columns);
	duckdb::ColumnList entry_columns_;
	std::string file_path_;
	std::string excel_table_name_;
	std::string drive_id_;
	std::string secret_name_;
	std::shared_ptr<HttpAuthParams> auth_params_;
};

// ============================================================================
// ExcelCatalog — the top-level catalog for an attached Excel workbook
// ============================================================================

class ExcelCatalog : public duckdb::Catalog {
public:
	ExcelCatalog(duckdb::AttachedDatabase &db,
	             const std::string &file_path,
	             const std::string &drive_id,
	             const std::string &secret_name,
	             std::shared_ptr<HttpAuthParams> auth_params);
	~ExcelCatalog() = default;

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

	ExcelSchemaEntry &GetMainSchema();
	const std::string &GetFilePath()  const { return file_path_; }
	const std::string &GetDriveId()   const { return drive_id_; }
	const std::string &GetSecretName() const { return secret_name_; }
	std::shared_ptr<HttpAuthParams> GetAuthParams() const { return auth_params_; }

private:
	std::string file_path_;
	std::string drive_id_;
	std::string secret_name_;
	std::shared_ptr<HttpAuthParams> auth_params_;
	duckdb::unique_ptr<ExcelSchemaEntry> main_schema_;
};

} // namespace erpl_web
