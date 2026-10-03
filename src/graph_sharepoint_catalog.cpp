#include "graph_sharepoint_catalog.hpp"
#include "graph_sharepoint_client.hpp"
#include "graph_sharepoint_type_mapper.hpp"
#include "graph_write_helpers.hpp"
#include "tracing.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "yyjson.hpp"
#include <algorithm>
#include <mutex>
#include <unordered_set>
#include "graph_json_scan.hpp"

using namespace duckdb_yyjson;
using namespace duckdb;

namespace erpl_web {

// -------------------------------------------------------------------------------------------------
// Internal scan types (file-scope)
// -------------------------------------------------------------------------------------------------

struct SharePointListScanBindData : public duckdb::TableFunctionData {
	std::string site_id;
	std::string list_id;
	std::shared_ptr<HttpAuthParams> auth_params;
	std::vector<std::string> column_names;
	std::vector<duckdb::LogicalType> column_types;
	// Pointer back to the owning table entry so LogicalGet::GetTable() works,
	// which is required for DELETE/UPDATE to be recognized as a base table operation.
	duckdb::TableCatalogEntry *table_entry = nullptr;
};

// Local state stores which columns (by column_t ID) the current execution is projecting.
// This enables projection pushdown: the scan outputs exactly the requested columns.
struct SharePointListScanLocalState : public duckdb::LocalTableFunctionState {
	std::vector<duckdb::column_t> column_ids;
};

static duckdb::BindInfo SharePointListScanGetBindInfo(const duckdb::optional_ptr<duckdb::FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<SharePointListScanBindData>();
	return duckdb::BindInfo(*bind_data.table_entry);
}

static duckdb::unique_ptr<duckdb::LocalTableFunctionState> SharePointListScanInitLocal(
    duckdb::ExecutionContext &context, duckdb::TableFunctionInitInput &input,
    duckdb::GlobalTableFunctionState *) {
	auto state = duckdb::make_uniq<SharePointListScanLocalState>();
	state->column_ids = input.column_ids;
	return state;
}

static void SharePointListScan(duckdb::ClientContext &context,
                                duckdb::TableFunctionInput &data,
                                duckdb::DataChunk &output)
{
	auto &bind_data = data.bind_data->Cast<SharePointListScanBindData>();
	auto &local_state = data.local_state->Cast<SharePointListScanLocalState>();
	auto &state = data.global_state->Cast<GraphJsonArrayScanState>();

	if (state.done) {
		output.SetCardinality(0);
		return;
	}

	// Lazy-fetch on first call
	if (!state.parsed_doc && state.json_response.empty()) {
		GraphSharePointClient client(bind_data.auth_params);
		state.json_response = client.GetListItems(bind_data.site_id, bind_data.list_id);
	}

	// Parse once; keep doc alive in bind_data so iterator stays valid across batches
	if (!state.parsed_doc) {
		if (!state.InitIterator()) {
			state.done = true;
			output.SetCardinality(0);
			return;
		}
	}

	const size_t output_col_count = local_state.column_ids.size();
	idx_t row = 0;
	yyjson_val *item;

	while (row < STANDARD_VECTOR_SIZE && (item = yyjson_arr_iter_next(&state.item_iter))) {
		yyjson_val *fields_obj = yyjson_obj_get(item, "fields");

		for (size_t col = 0; col < output_col_count; col++) {
			const duckdb::column_t col_id = local_state.column_ids[col];
			if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
				// Output the SharePoint numeric item id as a BIGINT rowid.
				// This allows DuckDB's DELETE machinery to identify rows without
				// a separate virtual-rowid mechanism.
				yyjson_val *id_val = yyjson_obj_get(item, "id");
				if (id_val && yyjson_is_str(id_val)) {
					try {
						output.data[col].SetValue(row, duckdb::Value::BIGINT(
						    std::stoll(yyjson_get_str(id_val))));
					} catch (...) {
						output.data[col].SetValue(row, duckdb::Value(duckdb::LogicalType::BIGINT));
					}
				} else {
					output.data[col].SetValue(row, duckdb::Value(duckdb::LogicalType::BIGINT));
				}
			} else {
				const std::string &col_name = bind_data.column_names[col_id];
				yyjson_val *field_val = (col_name == "id")
				    ? yyjson_obj_get(item, "id")
				    : (fields_obj ? yyjson_obj_get(fields_obj, col_name.c_str()) : nullptr);
				WriteSharePointField(output.data[col], row, field_val, bind_data.column_types[col_id]);
			}
		}
		row++;
	}

	if (row < STANDARD_VECTOR_SIZE) { state.done = true; }
	output.SetCardinality(row);
}

// -------------------------------------------------------------------------------------------------
// Physical write operators (INSERT / UPDATE / DELETE)
// -------------------------------------------------------------------------------------------------

// Shared global sink state for all three write operators
struct SharePointWriteGlobalState : public GlobalSinkState {
	explicit SharePointWriteGlobalState(std::shared_ptr<HttpAuthParams> auth_params_,
	                                    std::string site_id_, std::string list_id_)
	    : auth_params(std::move(auth_params_)), site_id(std::move(site_id_)),
	      list_id(std::move(list_id_)), affected_count(0) {}

	std::shared_ptr<HttpAuthParams> auth_params;
	std::string site_id;
	std::string list_id;
	std::mutex lock;
	idx_t affected_count;
};

// -------------------------------------------------------------------------------------------------
// PhysicalSharePointInsert
// -------------------------------------------------------------------------------------------------

class PhysicalSharePointInsert : public PhysicalOperator {
public:
	PhysicalSharePointInsert(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                          std::shared_ptr<HttpAuthParams> auth_params,
	                          std::string site_id, std::string list_id,
	                          std::vector<std::string> column_names,
	                          idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      auth_params_(std::move(auth_params)), site_id_(std::move(site_id)),
	      list_id_(std::move(list_id)), column_names_(std::move(column_names)) {}

	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<SharePointWriteGlobalState>(auth_params_, site_id_, list_id_);
	}
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override {
		return make_uniq<LocalSinkState>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &g = input.global_state.Cast<SharePointWriteGlobalState>();
		GraphSharePointClient client(g.auth_params);

		for (idx_t row = 0; row < chunk.size(); row++) {
			const std::string fields_json = DataChunkRowToSharePointFieldsJson(chunk, row, column_names_);
			client.CreateListItem(g.site_id, g.list_id, fields_json);
		}
		lock_guard<std::mutex> lk(g.lock);
		g.affected_count += chunk.size();
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override {
		return SinkFinalizeType::READY;
	}
	bool IsSink() const override { return true; }
	bool ParallelSink() const override { return false; }

	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
#ifdef DUCKDB_HAS_EXTENSION_CALLBACK_MANAGER
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
#else
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk,
	                         OperatorSourceInput &input) const override {
#endif
		auto &g = sink_state->Cast<SharePointWriteGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(g.affected_count)));
		return SourceResultType::FINISHED;
	}
	bool IsSource() const override { return true; }

private:
	std::shared_ptr<HttpAuthParams> auth_params_;
	std::string site_id_;
	std::string list_id_;
	std::vector<std::string> column_names_;
};

// -------------------------------------------------------------------------------------------------
// PhysicalSharePointDelete
// -------------------------------------------------------------------------------------------------

class PhysicalSharePointDelete : public PhysicalOperator {
public:
	PhysicalSharePointDelete(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                          std::shared_ptr<HttpAuthParams> auth_params,
	                          std::string site_id, std::string list_id,
	                          idx_t id_col_idx, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      auth_params_(std::move(auth_params)), site_id_(std::move(site_id)),
	      list_id_(std::move(list_id)), id_col_idx_(id_col_idx) {}

	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<SharePointWriteGlobalState>(auth_params_, site_id_, list_id_);
	}
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override {
		return make_uniq<LocalSinkState>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &g = input.global_state.Cast<SharePointWriteGlobalState>();
		GraphSharePointClient client(g.auth_params);

		for (idx_t row = 0; row < chunk.size(); row++) {
			const Value id_val = chunk.GetValue(id_col_idx_, row);
			if (id_val.IsNull()) {
				continue;
			}
			client.DeleteListItem(g.site_id, g.list_id, id_val.ToString());
		}
		lock_guard<std::mutex> lk(g.lock);
		g.affected_count += chunk.size();
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override {
		return SinkFinalizeType::READY;
	}
	bool IsSink() const override { return true; }
	bool ParallelSink() const override { return false; }

	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
#ifdef DUCKDB_HAS_EXTENSION_CALLBACK_MANAGER
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
#else
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk,
	                         OperatorSourceInput &input) const override {
#endif
		auto &g = sink_state->Cast<SharePointWriteGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(g.affected_count)));
		return SourceResultType::FINISHED;
	}
	bool IsSource() const override { return true; }

private:
	std::shared_ptr<HttpAuthParams> auth_params_;
	std::string site_id_;
	std::string list_id_;
	idx_t id_col_idx_;
};

// -------------------------------------------------------------------------------------------------
// PhysicalSharePointUpdate
// -------------------------------------------------------------------------------------------------

class PhysicalSharePointUpdate : public PhysicalOperator {
public:
	// num_table_cols: columns from the original scan (id at index 0)
	// column_names: all table column names in physical order
	// update_col_physical_indices: physical indices of the updated columns (from op.columns)
	PhysicalSharePointUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                          std::shared_ptr<HttpAuthParams> auth_params,
	                          std::string site_id, std::string list_id,
	                          idx_t num_table_cols,
	                          std::vector<std::string> column_names,
	                          std::vector<idx_t> update_col_physical_indices,
	                          idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      auth_params_(std::move(auth_params)), site_id_(std::move(site_id)),
	      list_id_(std::move(list_id)), num_table_cols_(num_table_cols),
	      column_names_(std::move(column_names)),
	      update_col_physical_indices_(std::move(update_col_physical_indices)) {}

	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<SharePointWriteGlobalState>(auth_params_, site_id_, list_id_);
	}
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override {
		return make_uniq<LocalSinkState>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &g = input.global_state.Cast<SharePointWriteGlobalState>();
		GraphSharePointClient client(g.auth_params);

		const idx_t m = update_col_physical_indices_.size();
		for (idx_t row = 0; row < chunk.size(); row++) {
			// Column 0 is always "id"
			const Value id_val = chunk.GetValue(0, row);
			if (id_val.IsNull()) {
				continue;
			}
			const std::string item_id = id_val.ToString();

			// Build fields JSON from the new values (columns after the original table columns)
			std::string fields_json = "{";
			bool first = true;
			for (idx_t i = 0; i < m; i++) {
				const idx_t phys_idx = update_col_physical_indices_[i];
				if (phys_idx >= column_names_.size()) { continue; }
				const std::string &col_name = column_names_[phys_idx];
				if (col_name == "id") { continue; }

				const Value new_val = chunk.GetValue(num_table_cols_ + i, row);
				if (!first) { fields_json += ','; }
				first = false;
				fields_json += '"';
				for (char c : col_name) {
					if (c == '"') { fields_json += "\\\""; }
					else if (c == '\\') { fields_json += "\\\\"; }
					else { fields_json += c; }
				}
				fields_json += "\":";
				fields_json += DuckDbValueToJsonLiteral(new_val);
			}
			fields_json += '}';

			client.UpdateListItem(g.site_id, g.list_id, item_id, fields_json);
		}
		lock_guard<std::mutex> lk(g.lock);
		g.affected_count += chunk.size();
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override {
		return SinkFinalizeType::READY;
	}
	bool IsSink() const override { return true; }
	bool ParallelSink() const override { return false; }

	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
#ifdef DUCKDB_HAS_EXTENSION_CALLBACK_MANAGER
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
#else
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk,
	                         OperatorSourceInput &input) const override {
#endif
		auto &g = sink_state->Cast<SharePointWriteGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(g.affected_count)));
		return SourceResultType::FINISHED;
	}
	bool IsSource() const override { return true; }

private:
	std::shared_ptr<HttpAuthParams> auth_params_;
	std::string site_id_;
	std::string list_id_;
	idx_t num_table_cols_;
	std::vector<std::string> column_names_;
	std::vector<idx_t> update_col_physical_indices_;
};

// -------------------------------------------------------------------------------------------------
// SharePointSchemaEntry Implementation
// -------------------------------------------------------------------------------------------------

SharePointSchemaEntry::SharePointSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info)
    : duckdb::SchemaCatalogEntry(catalog, info), tables_loaded(false)
{
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateTable(duckdb::CatalogTransaction transaction, duckdb::BoundCreateTableInfo &info) {
	throw duckdb::NotImplementedException("Creating tables in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateFunction(duckdb::CatalogTransaction transaction, duckdb::CreateFunctionInfo &info) {
	throw duckdb::NotImplementedException("Creating functions in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateIndex(duckdb::CatalogTransaction transaction, duckdb::CreateIndexInfo &info, duckdb::TableCatalogEntry &table) {
	throw duckdb::NotImplementedException("Creating indexes in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateView(duckdb::CatalogTransaction transaction, duckdb::CreateViewInfo &info) {
	throw duckdb::NotImplementedException("Creating views in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateSequence(duckdb::CatalogTransaction transaction, duckdb::CreateSequenceInfo &info) {
	throw duckdb::NotImplementedException("Creating sequences in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateTableFunction(duckdb::CatalogTransaction transaction, duckdb::CreateTableFunctionInfo &info) {
	throw duckdb::NotImplementedException("Creating table functions in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateCopyFunction(duckdb::CatalogTransaction transaction, duckdb::CreateCopyFunctionInfo &info) {
	throw duckdb::NotImplementedException("Creating copy functions in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreatePragmaFunction(duckdb::CatalogTransaction transaction, duckdb::CreatePragmaFunctionInfo &info) {
	throw duckdb::NotImplementedException("Creating pragma functions in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateCollation(duckdb::CatalogTransaction transaction, duckdb::CreateCollationInfo &info) {
	throw duckdb::NotImplementedException("Creating collations in SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::CreateType(duckdb::CatalogTransaction transaction, duckdb::CreateTypeInfo &info) {
	throw duckdb::NotImplementedException("Creating types in SharePoint schemas is not supported");
}

void SharePointSchemaEntry::Alter(duckdb::CatalogTransaction transaction, duckdb::AlterInfo &info) {
	throw duckdb::NotImplementedException("Altering SharePoint schemas is not supported");
}

void SharePointSchemaEntry::Scan(duckdb::ClientContext &context, duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) {
	if (type == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex);
		if (!tables_loaded) {
			LoadTables();
			tables_loaded = true;
		}
		for (auto &table_pair : table_entries) {
			callback(*table_pair.second);
		}
	}
}

void SharePointSchemaEntry::Scan(duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) {
	if (type == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex);
		if (!tables_loaded) {
			LoadTables();
			tables_loaded = true;
		}
		for (auto &table_pair : table_entries) {
			callback(*table_pair.second);
		}
	}
}

void SharePointSchemaEntry::DropEntry(duckdb::ClientContext &context, duckdb::DropInfo &info) {
	throw duckdb::NotImplementedException("Dropping entries from SharePoint schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::GetEntry(duckdb::CatalogTransaction transaction, duckdb::CatalogType type, const std::string &name) {
	if (type == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex);
		if (!tables_loaded) {
			LoadTables();
			tables_loaded = true;
		}
		auto it = table_entries.find(name);
		if (it != table_entries.end()) {
			return it->second.get();
		}
	}
	return nullptr;
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointSchemaEntry::LookupEntry(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() == duckdb::CatalogType::TABLE_ENTRY) {
		std::lock_guard<std::mutex> lock(tables_mutex);
		if (!tables_loaded) {
			LoadTables();
			tables_loaded = true;
		}
		auto it = table_entries.find(lookup_info.GetEntryName());
		if (it != table_entries.end()) {
			return it->second.get();
		}
	}
	return nullptr;
}

void SharePointSchemaEntry::LoadTables() {
	// Must only be called while holding tables_mutex
	auto &sp_catalog = static_cast<SharePointCatalog &>(catalog);
	const std::string &site_id = sp_catalog.GetSiteId();
	auto auth_params = sp_catalog.GetAuthParams();
	const std::string &secret_name = sp_catalog.GetSecretName();

	try {
		GraphSharePointClient sp_client(auth_params);

		// Fetch all lists for the site
		const std::string lists_json = sp_client.ListLists(site_id);

		yyjson_doc *lists_doc = yyjson_read(lists_json.c_str(), lists_json.length(), 0);
		// Owns the documents across the throws below; a failed column request must not leak them.
		std::unique_ptr<yyjson_doc, void (*)(yyjson_doc *)> lists_guard(lists_doc, yyjson_doc_free);
		if (!lists_doc) {
			throw duckdb::IOException("Microsoft Graph returned an unparseable list collection for site '" + site_id + "'");
		}

		yyjson_val *lists_root = yyjson_doc_get_root(lists_doc);
		yyjson_val *lists_arr = yyjson_obj_get(lists_root, "value");

		if (!lists_arr || !yyjson_is_arr(lists_arr)) {
			throw duckdb::IOException("Microsoft Graph list collection for site '" + site_id + "' has no 'value' array");
		}

		table_entries.clear();

		size_t list_idx, list_max;
		yyjson_val *list_item;

		yyjson_arr_foreach(lists_arr, list_idx, list_max, list_item) {
			// Extract list id and displayName
			yyjson_val *list_id_val = yyjson_obj_get(list_item, "id");
			yyjson_val *display_name_val = yyjson_obj_get(list_item, "displayName");

			if (!list_id_val || !yyjson_is_str(list_id_val)) { continue; }

			std::string list_id = yyjson_get_str(list_id_val);
			std::string table_key = list_id;  // fall back to ID as key

			if (display_name_val && yyjson_is_str(display_name_val)) {
				std::string display_name = yyjson_get_str(display_name_val);
				if (!display_name.empty()) {
					table_key = display_name;
				}
			}

			// Fetch columns for this list to build the schema
			// A failed or unparseable column response aborts the whole load. Skipping the list would
			// cache a catalog silently missing it even after the service recovers (GitHub #253).
			const std::string cols_json = sp_client.GetListColumns(site_id, list_id);

			yyjson_doc *cols_doc = yyjson_read(cols_json.c_str(), cols_json.length(), 0);
			if (!cols_doc) {
				throw duckdb::IOException("Microsoft Graph returned unparseable columns for list '" + list_id + "'");
			}
			std::unique_ptr<yyjson_doc, void (*)(yyjson_doc *)> cols_guard(cols_doc, yyjson_doc_free);

			yyjson_val *cols_root = yyjson_doc_get_root(cols_doc);
			yyjson_val *cols_arr = yyjson_obj_get(cols_root, "value");

			duckdb::CreateTableInfo table_info;
			table_info.table = table_key;
			table_info.schema = name;

			// Always include id as first column
			table_info.columns.AddColumn(duckdb::ColumnDefinition("id", duckdb::LogicalType::VARCHAR));
			std::vector<std::string> column_names = {"id"};
			std::unordered_set<std::string> seen_names = {"id"};

			if (cols_arr && yyjson_is_arr(cols_arr)) {
				size_t col_idx, col_max;
				yyjson_val *col;

				yyjson_arr_foreach(cols_arr, col_idx, col_max, col) {
					yyjson_val *name_val = yyjson_obj_get(col, "name");
					if (!name_val || !yyjson_is_str(name_val)) { continue; }

					std::string col_name = yyjson_get_str(name_val);

					// Skip internal/system columns
					if (col_name.empty() || col_name[0] == '_') { continue; }
					if (col_name == "Edit" || col_name == "LinkTitle" ||
					    col_name == "LinkTitleNoMenu" || col_name == "DocIcon" ||
					    col_name == "ItemChildCount" || col_name == "FolderChildCount" ||
					    col_name == "AppAuthor" || col_name == "AppEditor") {
						continue;
					}

					// Skip case-insensitive duplicates (e.g. "ID" collides with "id")
					std::string lower_name = col_name;
					std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
					if (!seen_names.insert(lower_name).second) { continue; }

					const duckdb::LogicalType col_type = SharePointColumnToDuckDBType(col);
					table_info.columns.AddColumn(duckdb::ColumnDefinition(col_name, col_type));
					column_names.push_back(col_name);
				}
			}


			auto table_entry = duckdb::make_uniq<SharePointTableEntry>(
			    catalog, *this, table_info,
			    site_id, list_id, secret_name, auth_params);

			table_entries[table_key] = std::move(table_entry);
		}


	} catch (const std::exception &e) {
		// Propagate and leave tables_loaded unset: an expired token or a 403 must not read as a site
		// with no lists, nor be cached as one (GitHub #253).
		ERPL_TRACE_ERROR("SHAREPOINT_CATALOG", "Failed to load tables for site " + site_id + ": " + std::string(e.what()));
		throw;
	}
}

// -------------------------------------------------------------------------------------------------
// SharePointTableEntry Implementation
// -------------------------------------------------------------------------------------------------

SharePointTableEntry::SharePointTableEntry(duckdb::Catalog &catalog,
                                           duckdb::SchemaCatalogEntry &schema,
                                           duckdb::CreateTableInfo &info,
                                           const std::string &site_id,
                                           const std::string &list_id,
                                           const std::string &secret_name,
                                           std::shared_ptr<HttpAuthParams> auth_params)
    : duckdb::TableCatalogEntry(catalog, schema, info),
      site_id_(site_id),
      list_id_(list_id),
      secret_name_(secret_name),
      auth_params_(auth_params)
{
	// Populate column_names and column_types from the inherited columns member.
	// NOTE: TableCatalogEntry's constructor moves info.columns, so info.columns is
	// empty here; use this->columns (the base class member) instead.
	for (const auto &col : columns.Logical()) {
		column_names_.push_back(col.Name());
		column_types_.push_back(col.Type());
	}
}

duckdb::unique_ptr<duckdb::BaseStatistics> SharePointTableEntry::GetStatistics(duckdb::ClientContext &context, duckdb::column_t column_id) {
	return nullptr;
}

duckdb::TableFunction SharePointTableEntry::GetScanFunction(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::FunctionData> &bind_data) {
	auto scan_bind_data = duckdb::make_uniq<SharePointListScanBindData>();
	scan_bind_data->site_id = site_id_;
	scan_bind_data->list_id = list_id_;
	scan_bind_data->auth_params = auth_params_;
	scan_bind_data->column_names = column_names_;
	scan_bind_data->column_types = column_types_;
	scan_bind_data->table_entry = this;

	bind_data = std::move(scan_bind_data);

	duckdb::TableFunction table_function("sharepoint_list_scan", {}, SharePointListScan,
	                                     nullptr, GraphJsonArrayScanState::Init, SharePointListScanInitLocal);
	table_function.projection_pushdown = true;
	table_function.get_bind_info = SharePointListScanGetBindInfo;
	return table_function;
}

duckdb::TableStorageInfo SharePointTableEntry::GetStorageInfo(duckdb::ClientContext &context) {
	return duckdb::TableStorageInfo();
}

void SharePointTableEntry::BindUpdateConstraints(duckdb::Binder &binder, duckdb::LogicalGet &get,
                                                  duckdb::LogicalProjection &proj, duckdb::LogicalUpdate &update,
                                                  duckdb::ClientContext &context) {
	// External table — no CHECK constraints or indexes to bind; no-op.
}

// -------------------------------------------------------------------------------------------------
// SharePointCatalog Implementation
// -------------------------------------------------------------------------------------------------

SharePointCatalog::SharePointCatalog(duckdb::AttachedDatabase &db,
                                     const std::string &site_name_or_id,
                                     const std::string &secret_name,
                                     std::shared_ptr<HttpAuthParams> auth_params)
    : duckdb::Catalog(db),
      secret_name_(secret_name),
      auth_params_(auth_params),
      path_(site_name_or_id)
{
	// Resolve site name/ID to a canonical site ID via the Graph API
	GraphSharePointClient sp_client(auth_params_);
	site_id_ = sp_client.ResolveSiteId(site_name_or_id);

	duckdb::CreateSchemaInfo schema_info;
	schema_info.schema = "main";
	main_schema_ = duckdb::make_uniq<SharePointSchemaEntry>(*this, schema_info);
}

duckdb::string SharePointCatalog::GetCatalogType() {
	return "sharepoint";
}

void SharePointCatalog::Initialize(bool load_builtin) {
	// Nothing to initialize
}

void SharePointCatalog::Initialize(duckdb::optional_ptr<duckdb::ClientContext> context, bool load_builtin) {
	// Nothing to initialize
}

void SharePointCatalog::FinalizeLoad(duckdb::optional_ptr<duckdb::ClientContext> context) {
	// Nothing to finalize
}

duckdb::string SharePointCatalog::GetDBPath() {
	return path_;
}

bool SharePointCatalog::InMemory() {
	return true;
}

bool SharePointCatalog::SupportsTimeTravel() const {
	return false;
}

duckdb::string SharePointCatalog::GetDefaultSchema() const {
	return "main";
}

duckdb::DatabaseSize SharePointCatalog::GetDatabaseSize(duckdb::ClientContext &context) {
	return duckdb::DatabaseSize();
}

duckdb::vector<duckdb::MetadataBlockInfo> SharePointCatalog::GetMetadataInfo(duckdb::ClientContext &context) {
	return duckdb::vector<duckdb::MetadataBlockInfo>();
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> SharePointCatalog::LookupSchema(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info, duckdb::OnEntryNotFound if_not_found) {
	if (lookup_info.GetEntryName() == "main") {
		return main_schema_.get();
	}
	if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
		throw duckdb::CatalogException("Schema \"%s\" not found", lookup_info.GetEntryName());
	}
	return nullptr;
}

duckdb::optional_ptr<duckdb::CatalogEntry> SharePointCatalog::CreateSchema(duckdb::CatalogTransaction transaction, duckdb::CreateSchemaInfo &info) {
	throw duckdb::NotImplementedException("Creating schemas in SharePoint catalogs is not supported");
}

void SharePointCatalog::ScanSchemas(duckdb::ClientContext &context, std::function<void(duckdb::SchemaCatalogEntry &)> callback) {
	callback(*main_schema_);
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> SharePointCatalog::GetSchema(duckdb::CatalogTransaction transaction, const std::string &schema_name, duckdb::OnEntryNotFound if_not_found, duckdb::QueryErrorContext error_context) {
	if (schema_name == "main") {
		return main_schema_.get();
	}
	if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
		throw duckdb::CatalogException("Schema \"%s\" not found", schema_name);
	}
	return nullptr;
}

duckdb::PhysicalOperator &SharePointCatalog::PlanCreateTableAs(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalCreateTable &op, duckdb::PhysicalOperator &plan) {
	throw duckdb::NotImplementedException("CREATE TABLE AS is not supported on SharePoint catalogs");
}

duckdb::PhysicalOperator &SharePointCatalog::PlanInsert(duckdb::ClientContext &context,
                                                         duckdb::PhysicalPlanGenerator &planner,
                                                         duckdb::LogicalInsert &op,
                                                         duckdb::optional_ptr<duckdb::PhysicalOperator> plan) {
	if (!plan) {
		throw duckdb::InvalidInputException("INSERT requires a source (VALUES or SELECT)");
	}

	auto &sp_table = op.table.Cast<SharePointTableEntry>();

	// Resolve defaults so child outputs columns in table physical order
	duckdb::PhysicalOperator *child_plan = plan.get();
	if (!op.column_index_map.empty()) {
		child_plan = &planner.ResolveDefaultsProjection(op, *child_plan);
	}

	// Collect column names in physical order
	std::vector<std::string> col_names;
	for (const auto &col : op.table.GetColumns().Physical()) {
		col_names.push_back(col.Name());
	}

	auto &insert_op = planner.Make<PhysicalSharePointInsert>(
	    op.types, sp_table.GetAuthParams(), site_id_, sp_table.GetListId(),
	    std::move(col_names), op.estimated_cardinality);
	insert_op.children.push_back(*child_plan);
	return insert_op;
}

duckdb::PhysicalOperator &SharePointCatalog::PlanDelete(duckdb::ClientContext &context,
                                                         duckdb::PhysicalPlanGenerator &planner,
                                                         duckdb::LogicalDelete &op,
                                                         duckdb::PhysicalOperator &plan) {
	auto &sp_table = op.table.Cast<SharePointTableEntry>();

	// BindRowIdColumns injects the virtual "rowid" (COLUMN_IDENTIFIER_ROW_ID) into
	// the scan and records its resolved physical chunk index in op.expressions[0].
	// By the time PlanDelete is called, ColumnBindingResolver has already run, so
	// the expression is a BoundReferenceExpression (not a BoundColumnRefExpression).
	// Its index is the position of the BIGINT rowid in the child plan's output chunk.
	idx_t id_col_idx = 0;
	if (!op.expressions.empty()) {
		auto &id_expr = op.expressions[0]->Cast<duckdb::BoundReferenceExpression>();
		id_col_idx = id_expr.index;
	}

	auto &del_op = planner.Make<PhysicalSharePointDelete>(
	    op.types, sp_table.GetAuthParams(), site_id_, sp_table.GetListId(),
	    id_col_idx, op.estimated_cardinality);
	del_op.children.push_back(plan);
	return del_op;
}

duckdb::PhysicalOperator &SharePointCatalog::PlanUpdate(duckdb::ClientContext &context,
                                                         duckdb::PhysicalPlanGenerator &planner,
                                                         duckdb::LogicalUpdate &op,
                                                         duckdb::PhysicalOperator &plan) {
	auto &sp_table = op.table.Cast<SharePointTableEntry>();

	// Collect column names in physical order
	const idx_t num_table_cols = op.table.GetColumns().PhysicalColumnCount();
	std::vector<std::string> col_names;
	col_names.reserve(num_table_cols);
	for (const auto &col : op.table.GetColumns().Physical()) {
		col_names.push_back(col.Name());
	}

	// Collect physical indices of updated columns (op.columns is a vector<PhysicalIndex>)
	std::vector<idx_t> update_col_indices;
	update_col_indices.reserve(op.columns.size());
	for (const auto &phys_idx : op.columns) {
		update_col_indices.push_back(phys_idx.index);
	}

	auto &upd_op = planner.Make<PhysicalSharePointUpdate>(
	    op.types, sp_table.GetAuthParams(), site_id_, sp_table.GetListId(),
	    num_table_cols, std::move(col_names), std::move(update_col_indices), op.estimated_cardinality);
	upd_op.children.push_back(plan);
	return upd_op;
}

duckdb::unique_ptr<duckdb::LogicalOperator> SharePointCatalog::BindCreateIndex(duckdb::Binder &binder, duckdb::CreateStatement &stmt, duckdb::TableCatalogEntry &table, duckdb::unique_ptr<duckdb::LogicalOperator> plan) {
	throw duckdb::NotImplementedException("CREATE INDEX is not supported on SharePoint catalogs");
}

void SharePointCatalog::DropSchema(duckdb::ClientContext &context, duckdb::DropInfo &info) {
	throw duckdb::NotImplementedException("DROP SCHEMA is not supported on SharePoint catalogs");
}

SharePointSchemaEntry &SharePointCatalog::GetMainSchema() {
	return *main_schema_;
}

const std::string &SharePointCatalog::GetSiteId() const {
	return site_id_;
}

const std::string &SharePointCatalog::GetSecretName() const {
	return secret_name_;
}

std::shared_ptr<HttpAuthParams> SharePointCatalog::GetAuthParams() const {
	return auth_params_;
}

} // namespace erpl_web
