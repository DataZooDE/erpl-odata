#include "graph_excel_catalog.hpp"
#include "graph_excel_client.hpp"
#include "graph_write_helpers.hpp"
#include "tracing.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "yyjson.hpp"
#include <mutex>
#include "graph_json_scan.hpp"

using namespace duckdb_yyjson;
using namespace duckdb;

namespace erpl_web {

// -------------------------------------------------------------------------------------------------
// Internal scan types (file-scope)
// -------------------------------------------------------------------------------------------------

struct ExcelTableScanBindData : public duckdb::TableFunctionData {
	std::string file_path;
	std::string table_name;
	std::string drive_id;
	std::shared_ptr<HttpAuthParams> auth_params;
	std::vector<std::string> column_names;
	duckdb::TableCatalogEntry *table_entry = nullptr;
};

static duckdb::BindInfo ExcelTableScanGetBindInfo(const duckdb::optional_ptr<duckdb::FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ExcelTableScanBindData>();
	return duckdb::BindInfo(*bind_data.table_entry);
}

static void ExcelTableScan(duckdb::ClientContext &context,
                            duckdb::TableFunctionInput &data,
                            duckdb::DataChunk &output)
{
	auto &bind_data = data.bind_data->Cast<ExcelTableScanBindData>();
	auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

	if (state.done) {
		output.SetCardinality(0);
		return;
	}

	// Lazy-fetch on first call
	if (!state.parsed_doc && state.json_response.empty()) {
		GraphExcelClient client(bind_data.auth_params);
		state.json_response = client.GetTableRowsByPath(bind_data.file_path, bind_data.table_name, bind_data.drive_id);
	}

	if (!state.parsed_doc) {
		if (!state.InitIterator()) {
			state.done = true;
			output.SetCardinality(0);
			return;
		}
	}

	const size_t col_count = bind_data.column_names.size();
	idx_t row_idx = 0;
	yyjson_val *item;

	while (row_idx < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
		// Each row object: { "values": [[cell1, cell2, ...]], ... }
		yyjson_val *values_arr = yyjson_obj_get(item, "values");
		yyjson_val *inner_arr  = (values_arr && yyjson_is_arr(values_arr))
		                             ? yyjson_arr_get_first(values_arr) : nullptr;

		if (!inner_arr || !yyjson_is_arr(inner_arr)) {
			for (size_t col = 0; col < col_count; col++) {
				duckdb::FlatVector::Validity(output.data[col]).SetInvalid(row_idx);
			}
			row_idx++;
			continue;
		}

		yyjson_arr_iter cell_iter;
		yyjson_arr_iter_init(inner_arr, &cell_iter);
		for (size_t col = 0; col < col_count; col++) {
			yyjson_val *cell = yyjson_arr_iter_next(&cell_iter);
			if (!cell || yyjson_is_null(cell)) {
				duckdb::FlatVector::Validity(output.data[col]).SetInvalid(row_idx);
			} else {
				std::string str_val;
				if (yyjson_is_str(cell)) {
					str_val = yyjson_get_str(cell);
				} else {
					// Numeric/boolean cells: write as JSON literal string
					char *tmp = yyjson_val_write(cell, 0, nullptr);
					if (tmp) {
						str_val = tmp;
						free(tmp);
					}
				}
				duckdb::FlatVector::GetData<duckdb::string_t>(output.data[col])[row_idx] =
				    duckdb::StringVector::AddString(output.data[col], str_val);
			}
		}
		row_idx++;
	}

	if (row_idx < STANDARD_VECTOR_SIZE) { state.done = true; }
	output.SetCardinality(row_idx);
}

// -------------------------------------------------------------------------------------------------
// Write global state and physical operator
// -------------------------------------------------------------------------------------------------

struct ExcelWriteGlobalState : public duckdb::GlobalSinkState {
	std::shared_ptr<HttpAuthParams> auth_params;
	std::string file_path;
	std::string table_name;
	std::string drive_id;
	idx_t affected_count = 0;
	std::mutex lock;

	ExcelWriteGlobalState(std::shared_ptr<HttpAuthParams> ap,
	                      std::string fp, std::string tn, std::string di)
	    : auth_params(std::move(ap)), file_path(std::move(fp)),
	      table_name(std::move(tn)), drive_id(std::move(di)) {}
};

class PhysicalExcelInsert : public duckdb::PhysicalOperator {
public:
	PhysicalExcelInsert(duckdb::PhysicalPlan &physical_plan,
	                    duckdb::vector<duckdb::LogicalType> types,
	                    std::shared_ptr<HttpAuthParams> auth_params,
	                    std::string file_path,
	                    std::string table_name,
	                    std::string drive_id,
	                    std::vector<std::string> column_names,
	                    duckdb::idx_t estimated_cardinality)
	    : duckdb::PhysicalOperator(physical_plan, duckdb::PhysicalOperatorType::EXTENSION,
	                               std::move(types), estimated_cardinality),
	      auth_params_(std::move(auth_params)),
	      file_path_(std::move(file_path)),
	      table_name_(std::move(table_name)),
	      drive_id_(std::move(drive_id)),
	      column_names_(std::move(column_names)) {}

	duckdb::unique_ptr<duckdb::GlobalSinkState> GetGlobalSinkState(duckdb::ClientContext &context) const override {
		return duckdb::make_uniq<ExcelWriteGlobalState>(auth_params_, file_path_, table_name_, drive_id_);
	}
	duckdb::unique_ptr<duckdb::LocalSinkState> GetLocalSinkState(duckdb::ExecutionContext &context) const override {
		return duckdb::make_uniq<duckdb::LocalSinkState>();
	}

	duckdb::SinkResultType Sink(duckdb::ExecutionContext &context,
	                            duckdb::DataChunk &chunk,
	                            duckdb::OperatorSinkInput &input) const override {
		auto &g = input.global_state.Cast<ExcelWriteGlobalState>();
		// Build 2-D array JSON for this chunk and POST to the add rows endpoint
		const std::string rows_json = DataChunkToExcelAddRowsBody(chunk, column_names_);
		GraphExcelClient client(g.auth_params);
		const idx_t added = client.AddTableRows(g.file_path, g.table_name, rows_json, g.drive_id);
		std::lock_guard<std::mutex> lk(g.lock);
		g.affected_count += added;
		return duckdb::SinkResultType::NEED_MORE_INPUT;
	}

	duckdb::SinkFinalizeType Finalize(duckdb::Pipeline &pipeline, duckdb::Event &event,
	                                  duckdb::ClientContext &context,
	                                  duckdb::OperatorSinkFinalizeInput &input) const override {
		return duckdb::SinkFinalizeType::READY;
	}

	bool IsSink() const override { return true; }
	bool ParallelSink() const override { return false; }

	duckdb::unique_ptr<duckdb::GlobalSourceState> GetGlobalSourceState(duckdb::ClientContext &context) const override {
		return duckdb::make_uniq<duckdb::GlobalSourceState>();
	}

#ifdef DUCKDB_HAS_EXTENSION_CALLBACK_MANAGER
	duckdb::SourceResultType GetDataInternal(duckdb::ExecutionContext &context, duckdb::DataChunk &chunk,
	                                         duckdb::OperatorSourceInput &input) const override {
#else
	duckdb::SourceResultType GetData(duckdb::ExecutionContext &context, duckdb::DataChunk &chunk,
	                                 duckdb::OperatorSourceInput &input) const override {
#endif
		auto &g = sink_state->Cast<ExcelWriteGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, duckdb::Value::BIGINT(static_cast<int64_t>(g.affected_count)));
		return duckdb::SourceResultType::FINISHED;
	}
	bool IsSource() const override { return true; }

private:
	std::shared_ptr<HttpAuthParams> auth_params_;
	std::string file_path_;
	std::string table_name_;
	std::string drive_id_;
	std::vector<std::string> column_names_;
};

// -------------------------------------------------------------------------------------------------
// ExcelSchemaEntry Implementation
// -------------------------------------------------------------------------------------------------

ExcelSchemaEntry::ExcelSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info)
    : duckdb::SchemaCatalogEntry(catalog, info)
{
}

duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateTable(duckdb::CatalogTransaction, duckdb::BoundCreateTableInfo &) {
	throw duckdb::NotImplementedException("Creating tables in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateFunction(duckdb::CatalogTransaction, duckdb::CreateFunctionInfo &) {
	throw duckdb::NotImplementedException("Creating functions in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateIndex(duckdb::CatalogTransaction, duckdb::CreateIndexInfo &, duckdb::TableCatalogEntry &) {
	throw duckdb::NotImplementedException("Creating indexes in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateView(duckdb::CatalogTransaction, duckdb::CreateViewInfo &) {
	throw duckdb::NotImplementedException("Creating views in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateSequence(duckdb::CatalogTransaction, duckdb::CreateSequenceInfo &) {
	throw duckdb::NotImplementedException("Creating sequences in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateTableFunction(duckdb::CatalogTransaction, duckdb::CreateTableFunctionInfo &) {
	throw duckdb::NotImplementedException("Creating table functions in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateCopyFunction(duckdb::CatalogTransaction, duckdb::CreateCopyFunctionInfo &) {
	throw duckdb::NotImplementedException("Creating copy functions in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreatePragmaFunction(duckdb::CatalogTransaction, duckdb::CreatePragmaFunctionInfo &) {
	throw duckdb::NotImplementedException("Creating pragma functions in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateCollation(duckdb::CatalogTransaction, duckdb::CreateCollationInfo &) {
	throw duckdb::NotImplementedException("Creating collations in Excel workbook catalogs is not supported");
}
duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::CreateType(duckdb::CatalogTransaction, duckdb::CreateTypeInfo &) {
	throw duckdb::NotImplementedException("Creating types in Excel workbook catalogs is not supported");
}
void ExcelSchemaEntry::Alter(duckdb::CatalogTransaction, duckdb::AlterInfo &) {
	throw duckdb::NotImplementedException("Altering Excel workbook schemas is not supported");
}

void ExcelSchemaEntry::Scan(duckdb::ClientContext &context, duckdb::CatalogType type,
                             const std::function<void(duckdb::CatalogEntry &)> &callback) {
	if (type == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex_);
		if (!tables_loaded_) {
			LoadTables();
			tables_loaded_ = true;
		}
		for (auto &pair : table_entries_) {
			callback(*pair.second);
		}
	}
}

void ExcelSchemaEntry::Scan(duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) {
	if (type == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex_);
		if (!tables_loaded_) {
			LoadTables();
			tables_loaded_ = true;
		}
		for (auto &pair : table_entries_) {
			callback(*pair.second);
		}
	}
}

void ExcelSchemaEntry::DropEntry(duckdb::ClientContext &, duckdb::DropInfo &) {
	throw duckdb::NotImplementedException("Dropping entries from Excel workbook schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ExcelSchemaEntry::LookupEntry(duckdb::CatalogTransaction transaction,
                                                                           const duckdb::EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex_);
		if (!tables_loaded_) {
			LoadTables();
			tables_loaded_ = true;
		}
		auto it = table_entries_.find(lookup_info.GetEntryName());
		if (it != table_entries_.end()) {
			return it->second.get();
		}
	}
	return nullptr;
}

void ExcelSchemaEntry::LoadTables() {
	auto &excel_catalog = static_cast<ExcelCatalog &>(catalog);
	const std::string &file_path  = excel_catalog.GetFilePath();
	const std::string &drive_id   = excel_catalog.GetDriveId();
	const std::string &secret_name = excel_catalog.GetSecretName();
	auto auth_params = excel_catalog.GetAuthParams();

	try {
		GraphExcelClient client(auth_params);
		const std::string tables_json = client.ListTablesByPath(file_path, drive_id);

		yyjson_doc *doc = yyjson_read(tables_json.c_str(), tables_json.length(), 0);
		// Owns the document across the throws below; a failed column request must not leak it.
		std::unique_ptr<yyjson_doc, void (*)(yyjson_doc *)> doc_guard(doc, yyjson_doc_free);
		if (!doc) {
			throw duckdb::IOException("Microsoft Graph returned an unparseable table list for workbook '" + file_path + "'");
		}

		yyjson_val *root = yyjson_doc_get_root(doc);
		yyjson_val *arr  = yyjson_obj_get(root, "value");
		if (!arr || !yyjson_is_arr(arr)) {
			throw duckdb::IOException("Microsoft Graph table list for workbook '" + file_path + "' has no 'value' array");
		}

		table_entries_.clear();

		size_t idx, max;
		yyjson_val *table_item;

		yyjson_arr_foreach(arr, idx, max, table_item) {
			yyjson_val *name_val = yyjson_obj_get(table_item, "name");
			if (!name_val || !yyjson_is_str(name_val)) { continue; }
			const std::string table_name = yyjson_get_str(name_val);

			// Fetch column names for this table
			// A failed request aborts the whole load. Skipping the table would cache a catalog that is
			// silently missing it even after the service recovers (GitHub #253).
			std::vector<std::string> col_names = client.GetTableColumnsByPath(file_path, table_name, drive_id);

			duckdb::CreateTableInfo table_info;
			table_info.table  = table_name;
			table_info.schema = name;

			if (col_names.empty()) {
				// Fallback: at least one generic column so the entry is usable
				table_info.columns.AddColumn(duckdb::ColumnDefinition("column_0", duckdb::LogicalType::VARCHAR));
				col_names.push_back("column_0");
			} else {
				for (const auto &col_name : col_names) {
					table_info.columns.AddColumn(duckdb::ColumnDefinition(col_name, duckdb::LogicalType::VARCHAR));
				}
			}

			auto entry = duckdb::make_uniq<ExcelTableEntry>(
			    catalog, *this, table_info,
			    file_path, table_name, drive_id, secret_name, auth_params);
			table_entries_[table_name] = std::move(entry);
		}


	} catch (const std::exception &e) {
		// Propagate and leave tables_loaded_ unset: an expired token or a 403 must not read as a
		// workbook with no tables, nor be cached as one (GitHub #253).
		ERPL_TRACE_ERROR("EXCEL_CATALOG", "Failed to load tables for " + file_path + ": " + std::string(e.what()));
		throw;
	}
}

// -------------------------------------------------------------------------------------------------
// ExcelTableEntry Implementation
// -------------------------------------------------------------------------------------------------

ExcelTableEntry::ExcelTableEntry(duckdb::Catalog &catalog,
                                 duckdb::SchemaCatalogEntry &schema,
                                 duckdb::CreateTableInfo &info,
                                 const std::string &file_path,
                                 const std::string &table_name,
                                 const std::string &drive_id,
                                 const std::string &secret_name,
                                 std::shared_ptr<HttpAuthParams> auth_params)
    : duckdb::TableCatalogEntry(catalog, schema, info),
      file_path_(file_path),
      excel_table_name_(table_name),
      drive_id_(drive_id),
      secret_name_(secret_name),
      auth_params_(auth_params)
{
}

duckdb::unique_ptr<duckdb::BaseStatistics> ExcelTableEntry::GetStatistics(duckdb::ClientContext &, duckdb::column_t) {
	return nullptr;
}

duckdb::TableFunction ExcelTableEntry::GetScanFunction(duckdb::ClientContext &,
                                                        duckdb::unique_ptr<duckdb::FunctionData> &bind_data) {
	auto scan_data = duckdb::make_uniq<ExcelTableScanBindData>();
	scan_data->file_path   = file_path_;
	scan_data->table_name  = excel_table_name_;
	scan_data->drive_id    = drive_id_;
	scan_data->auth_params = auth_params_;
	scan_data->table_entry = this;

	for (const auto &col : columns.Physical()) {
		scan_data->column_names.push_back(col.Name());
	}

	bind_data = std::move(scan_data);

	duckdb::TableFunction fn("excel_table_scan", {}, ExcelTableScan, nullptr,
	                         GraphJsonArrayScanState::Init);
	fn.get_bind_info = ExcelTableScanGetBindInfo;
	return fn;
}

duckdb::TableStorageInfo ExcelTableEntry::GetStorageInfo(duckdb::ClientContext &) {
	return duckdb::TableStorageInfo();
}

void ExcelTableEntry::BindUpdateConstraints(duckdb::Binder &, duckdb::LogicalGet &,
                                             duckdb::LogicalProjection &, duckdb::LogicalUpdate &,
                                             duckdb::ClientContext &) {
	// External table — no-op.
}

duckdb::virtual_column_map_t ExcelTableEntry::GetVirtualColumns() const {
	// Excel tables are remote-only; no rowid or other virtual columns are supported.
	return {};
}

// -------------------------------------------------------------------------------------------------
// ExcelCatalog Implementation
// -------------------------------------------------------------------------------------------------

ExcelCatalog::ExcelCatalog(duckdb::AttachedDatabase &db,
                            const std::string &file_path,
                            const std::string &drive_id,
                            const std::string &secret_name,
                            std::shared_ptr<HttpAuthParams> auth_params)
    : duckdb::Catalog(db),
      file_path_(file_path),
      drive_id_(drive_id),
      secret_name_(secret_name),
      auth_params_(auth_params)
{
	duckdb::CreateSchemaInfo schema_info;
	schema_info.schema = "main";
	main_schema_ = duckdb::make_uniq<ExcelSchemaEntry>(*this, schema_info);
}

duckdb::string ExcelCatalog::GetCatalogType() { return "excel_workbook"; }
void ExcelCatalog::Initialize(bool) {}
void ExcelCatalog::Initialize(duckdb::optional_ptr<duckdb::ClientContext>, bool) {}
void ExcelCatalog::FinalizeLoad(duckdb::optional_ptr<duckdb::ClientContext>) {}
duckdb::string ExcelCatalog::GetDBPath() { return file_path_; }
bool ExcelCatalog::InMemory() { return true; }
bool ExcelCatalog::SupportsTimeTravel() const { return false; }
duckdb::string ExcelCatalog::GetDefaultSchema() const { return "main"; }
duckdb::DatabaseSize ExcelCatalog::GetDatabaseSize(duckdb::ClientContext &) { return duckdb::DatabaseSize(); }
duckdb::vector<duckdb::MetadataBlockInfo> ExcelCatalog::GetMetadataInfo(duckdb::ClientContext &) { return {}; }

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> ExcelCatalog::LookupSchema(duckdb::CatalogTransaction,
                                                                              const duckdb::EntryLookupInfo &lookup_info,
                                                                              duckdb::OnEntryNotFound if_not_found) {
	if (lookup_info.GetEntryName() == "main") { return main_schema_.get(); }
	if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
		throw duckdb::CatalogException("Schema \"%s\" not found", lookup_info.GetEntryName());
	}
	return nullptr;
}

duckdb::optional_ptr<duckdb::CatalogEntry> ExcelCatalog::CreateSchema(duckdb::CatalogTransaction, duckdb::CreateSchemaInfo &) {
	throw duckdb::NotImplementedException("Creating schemas in Excel workbook catalogs is not supported");
}

void ExcelCatalog::ScanSchemas(duckdb::ClientContext &, std::function<void(duckdb::SchemaCatalogEntry &)> callback) {
	callback(*main_schema_);
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> ExcelCatalog::GetSchema(duckdb::CatalogTransaction transaction,
                                                                           const std::string &schema_name,
                                                                           duckdb::OnEntryNotFound if_not_found,
                                                                           duckdb::QueryErrorContext) {
	if (schema_name == "main") { return main_schema_.get(); }
	if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
		throw duckdb::CatalogException("Schema \"%s\" not found", schema_name);
	}
	return nullptr;
}

duckdb::PhysicalOperator &ExcelCatalog::PlanCreateTableAs(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &,
                                                           duckdb::LogicalCreateTable &, duckdb::PhysicalOperator &) {
	throw duckdb::NotImplementedException("CREATE TABLE AS is not supported on Excel workbook catalogs");
}

duckdb::PhysicalOperator &ExcelCatalog::PlanInsert(duckdb::ClientContext &context,
                                                    duckdb::PhysicalPlanGenerator &planner,
                                                    duckdb::LogicalInsert &op,
                                                    duckdb::optional_ptr<duckdb::PhysicalOperator> plan) {
	if (!plan) {
		throw duckdb::InvalidInputException("INSERT requires a source (VALUES or SELECT)");
	}

	auto &excel_table = op.table.Cast<ExcelTableEntry>();

	duckdb::PhysicalOperator *child_plan = plan.get();
	if (!op.column_index_map.empty()) {
		child_plan = &planner.ResolveDefaultsProjection(op, *child_plan);
	}

	// Column names in physical order (must match the API row column order)
	std::vector<std::string> col_names;
	for (const auto &col : op.table.GetColumns().Physical()) {
		col_names.push_back(col.Name());
	}

	auto &insert_op = planner.Make<PhysicalExcelInsert>(
	    op.types, excel_table.GetAuthParams(),
	    excel_table.GetFilePath(), excel_table.GetTableName(), excel_table.GetDriveId(),
	    std::move(col_names), op.estimated_cardinality);
	insert_op.children.push_back(*child_plan);
	return insert_op;
}

duckdb::PhysicalOperator &ExcelCatalog::PlanDelete(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &,
                                                    duckdb::LogicalDelete &, duckdb::PhysicalOperator &) {
	throw duckdb::NotImplementedException("DELETE is not supported on Excel workbook catalogs");
}

duckdb::PhysicalOperator &ExcelCatalog::PlanUpdate(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &,
                                                    duckdb::LogicalUpdate &, duckdb::PhysicalOperator &) {
	throw duckdb::NotImplementedException("UPDATE is not supported on Excel workbook catalogs");
}

duckdb::unique_ptr<duckdb::LogicalOperator> ExcelCatalog::BindCreateIndex(duckdb::Binder &, duckdb::CreateStatement &,
                                                                            duckdb::TableCatalogEntry &,
                                                                            duckdb::unique_ptr<duckdb::LogicalOperator>) {
	throw duckdb::NotImplementedException("CREATE INDEX is not supported on Excel workbook catalogs");
}

void ExcelCatalog::DropSchema(duckdb::ClientContext &, duckdb::DropInfo &) {
	throw duckdb::NotImplementedException("DROP SCHEMA is not supported on Excel workbook catalogs");
}

ExcelSchemaEntry &ExcelCatalog::GetMainSchema() { return *main_schema_; }

} // namespace erpl_web
