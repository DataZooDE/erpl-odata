#include "delta_share_catalog.hpp"
#include "delta_share_client.hpp"
#include "delta_share_types.hpp"
#include "tracing.hpp"
#include "telemetry.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector.hpp"

#include <memory>
#include <vector>
#include <string>
#include "erpl_odata_banner.hpp"

using duckdb::ClientContext;
using duckdb::DataChunk;
using duckdb::ExecutionContext;
using duckdb::FunctionData;
using duckdb::InvalidInputException;
using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInput;
using duckdb::TableFunctionSet;
using duckdb::Value;

namespace erpl_web {

using duckdb::PostHogTelemetry;

// ============================================================================
// Helper Bind Data Structures
// ============================================================================

// Per-execution scan cursor shared by the delta-share catalog functions. The
// position must not live on the bind data: a bound plan that is EXECUTEd twice
// would otherwise return zero rows the second time (GitHub #202).
struct DeltaShareRowCursorState : public duckdb::GlobalTableFunctionState {
	size_t current_index = 0;
	bool finished = false;

	static duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Init(duckdb::ClientContext &,
	                                                                duckdb::TableFunctionInitInput &) {
		return duckdb::make_uniq<DeltaShareRowCursorState>();
	}
};

struct DeltaShareShowSharesBindData : public TableFunctionData {
public:
	std::vector<DeltaShareInfo> shares;

	DeltaShareShowSharesBindData() = default;

	duckdb::unique_ptr<FunctionData> Copy() const override {
		auto result = duckdb::make_uniq<DeltaShareShowSharesBindData>();
		result->shares = shares;
		return std::move(result);
	}
};

struct DeltaShareShowSchemasBindData : public TableFunctionData {
public:
	std::vector<DeltaSchemaInfo> schemas;

	DeltaShareShowSchemasBindData() = default;

	duckdb::unique_ptr<FunctionData> Copy() const override {
		auto result = duckdb::make_uniq<DeltaShareShowSchemasBindData>();
		result->schemas = schemas;
		return std::move(result);
	}
};

struct DeltaShareShowTablesBindData : public TableFunctionData {
public:
	std::vector<DeltaTableInfo> tables;

	DeltaShareShowTablesBindData() = default;

	duckdb::unique_ptr<FunctionData> Copy() const override {
		auto result = duckdb::make_uniq<DeltaShareShowTablesBindData>();
		result->tables = tables;
		return std::move(result);
	}
};

// ============================================================================
// delta_share_list_shares() - Lists all shares
// ============================================================================

static void DeltaShareShowSharesScan(ClientContext &context, TableFunctionInput &data_p,
									   DataChunk &output) {
	auto &state = data_p.global_state->Cast<DeltaShareRowCursorState>();
	auto &bind_data = data_p.bind_data->Cast<DeltaShareShowSharesBindData>();

	if (state.finished) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = 0;
	while (state.current_index < bind_data.shares.size() && count < output.GetCapacity()) {
		const auto &share = bind_data.shares[state.current_index];

		output.SetValue(0, count, Value(share.name));
		output.SetValue(1, count, Value(share.id));

		state.current_index++;
		count++;
	}

	state.finished = (state.current_index >= bind_data.shares.size());
	output.SetCardinality(count);
}

static duckdb::unique_ptr<FunctionData>
DeltaShareShowSharesBind(ClientContext &context, TableFunctionBindInput &input,
						vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("delta_share_show_shares");
	// Validate input
	if (input.inputs.empty()) {
		throw InvalidInputException("delta_share_list_shares requires profile_path parameter");
	}

	auto profile_path = input.inputs[0].GetValue<std::string>();

	ERPL_TRACE_INFO("DELTA_SHARE_CATALOG", "Listing shares from: " + profile_path);

	try {
		// Load profile
		auto profile = DeltaShareProfile::FromFile(context, profile_path);

		// Create client
		auto client = duckdb::make_shared_ptr<DeltaShareClient>(context, profile);

		// Fetch shares
		auto shares = client->ListShares();

		ERPL_TRACE_DEBUG("DELTA_SHARE_CATALOG",
						 "Found " + std::to_string(shares.size()) + " shares");

		// Define output schema
		names = {"share_name", "share_id"};
		return_types = {
			LogicalType(LogicalTypeId::VARCHAR),  // share_name
			LogicalType(LogicalTypeId::VARCHAR)   // share_id
		};

		// Create and populate bind data
		auto bind = duckdb::make_uniq<DeltaShareShowSharesBindData>();
		bind->shares = shares;

		return std::move(bind);
	} catch (const std::exception &e) {
		ERPL_TRACE_ERROR("DELTA_SHARE_CATALOG", "Failed to list shares: " + std::string(e.what()));
		throw;
	}
}

// ============================================================================
// delta_share_list_schemas() - Lists schemas in a share
// ============================================================================

static void DeltaShareShowSchemasScan(ClientContext &context, TableFunctionInput &data_p,
									    DataChunk &output) {
	auto &state = data_p.global_state->Cast<DeltaShareRowCursorState>();
	auto &bind_data = data_p.bind_data->Cast<DeltaShareShowSchemasBindData>();

	if (state.finished) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = 0;
	while (state.current_index < bind_data.schemas.size() && count < output.GetCapacity()) {
		const auto &schema = bind_data.schemas[state.current_index];

		output.SetValue(0, count, Value(schema.name));
		output.SetValue(1, count, Value(schema.share));

		state.current_index++;
		count++;
	}

	state.finished = (state.current_index >= bind_data.schemas.size());
	output.SetCardinality(count);
}

static duckdb::unique_ptr<FunctionData>
DeltaShareShowSchemasBind(ClientContext &context, TableFunctionBindInput &input,
						vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("delta_share_show_schemas");
	// Validate input
	if (input.inputs.size() < 2) {
		throw InvalidInputException(
			"delta_share_list_schemas requires profile_path and share parameters");
	}

	auto profile_path = input.inputs[0].GetValue<std::string>();
	auto share = input.inputs[1].GetValue<std::string>();

	ERPL_TRACE_INFO("DELTA_SHARE_CATALOG",
					"Listing schemas in share: " + share + " from profile: " + profile_path);

	try {
		// Load profile
		auto profile = DeltaShareProfile::FromFile(context, profile_path);

		// Create client
		auto client = duckdb::make_shared_ptr<DeltaShareClient>(context, profile);

		// Fetch schemas
		auto schemas = client->ListSchemas(share);

		ERPL_TRACE_DEBUG("DELTA_SHARE_CATALOG",
						 "Found " + std::to_string(schemas.size()) + " schemas");

		// Define output schema
		names = {"schema_name", "share"};
		return_types = {
			LogicalType(LogicalTypeId::VARCHAR),  // schema_name
			LogicalType(LogicalTypeId::VARCHAR)   // share
		};

		// Create and populate bind data
		auto bind = duckdb::make_uniq<DeltaShareShowSchemasBindData>();
		bind->schemas = schemas;

		return std::move(bind);
	} catch (const std::exception &e) {
		ERPL_TRACE_ERROR("DELTA_SHARE_CATALOG",
						 "Failed to list schemas: " + std::string(e.what()));
		throw;
	}
}

// ============================================================================
// delta_share_list_tables() - Lists tables in a schema
// ============================================================================

static void DeltaShareShowTablesScan(ClientContext &context, TableFunctionInput &data_p,
									   DataChunk &output) {
	auto &state = data_p.global_state->Cast<DeltaShareRowCursorState>();
	auto &bind_data = data_p.bind_data->Cast<DeltaShareShowTablesBindData>();

	if (state.finished) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = 0;
	while (state.current_index < bind_data.tables.size() && count < output.GetCapacity()) {
		const auto &table = bind_data.tables[state.current_index];

		output.SetValue(0, count, Value(table.name));
		output.SetValue(1, count, Value(table.schema));
		output.SetValue(2, count, Value(table.share));
		output.SetValue(3, count, Value(table.id));
		if (table.description.has_value()) {
			output.SetValue(4, count, Value(table.description.value()));
		} else {
			output.SetValue(4, count, Value());
		}

		state.current_index++;
		count++;
	}

	state.finished = (state.current_index >= bind_data.tables.size());
	output.SetCardinality(count);
}

static duckdb::unique_ptr<FunctionData>
DeltaShareShowTablesBind(ClientContext &context, TableFunctionBindInput &input,
						vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("delta_share_show_tables");
	// Validate input
	if (input.inputs.size() < 3) {
		throw InvalidInputException("delta_share_list_tables requires profile_path, share, and "
									 "schema parameters");
	}

	auto profile_path = input.inputs[0].GetValue<std::string>();
	auto share = input.inputs[1].GetValue<std::string>();
	auto schema = input.inputs[2].GetValue<std::string>();

	ERPL_TRACE_INFO("DELTA_SHARE_CATALOG",
					"Listing tables in schema: " + schema + ", share: " + share +
						", profile: " + profile_path);

	try {
		// Load profile
		auto profile = DeltaShareProfile::FromFile(context, profile_path);

		// Create client
		auto client = duckdb::make_shared_ptr<DeltaShareClient>(context, profile);

		// Fetch tables
		auto tables = client->ListTables(share, schema);

		ERPL_TRACE_DEBUG("DELTA_SHARE_CATALOG",
						 "Found " + std::to_string(tables.size()) + " tables");

		// Define output schema
		names = {"table_name", "schema", "share", "table_id", "description"};
		return_types = {
			LogicalType(LogicalTypeId::VARCHAR),  // table_name
			LogicalType(LogicalTypeId::VARCHAR),  // schema
			LogicalType(LogicalTypeId::VARCHAR),  // share
			LogicalType(LogicalTypeId::VARCHAR),  // table_id
			LogicalType(LogicalTypeId::VARCHAR)   // description (nullable)
		};

		// Create and populate bind data
		auto bind = duckdb::make_uniq<DeltaShareShowTablesBindData>();
		bind->tables = tables;

		return std::move(bind);
	} catch (const std::exception &e) {
		ERPL_TRACE_ERROR("DELTA_SHARE_CATALOG",
						 "Failed to list tables: " + std::string(e.what()));
		throw;
	}
}

// ============================================================================
// Function Factory Methods (internal)
// ============================================================================

static TableFunctionSet CreateDeltaShareShowSharesFunctionInternal() {
	TableFunctionSet function_set("delta_share_show_shares");

	duckdb::TableFunction func({LogicalTypeId::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, DeltaShareShowSharesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DeltaShareShowSharesBind));
	func.init_global = DeltaShareRowCursorState::Init;

	function_set.AddFunction(func);
	return function_set;
}

static TableFunctionSet CreateDeltaShareShowSchemasFunctionInternal() {
	TableFunctionSet function_set("delta_share_show_schemas");

	duckdb::TableFunction func({LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
							   DATAZOO_GUARD(ERPL_ODATA_BANNER, DeltaShareShowSchemasScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DeltaShareShowSchemasBind));
	func.init_global = DeltaShareRowCursorState::Init;

	function_set.AddFunction(func);
	return function_set;
}

static TableFunctionSet CreateDeltaShareShowTablesFunctionInternal() {
	TableFunctionSet function_set("delta_share_show_tables");

	duckdb::TableFunction func({LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
							   DATAZOO_GUARD(ERPL_ODATA_BANNER, DeltaShareShowTablesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, DeltaShareShowTablesBind));
	func.init_global = DeltaShareRowCursorState::Init;

	function_set.AddFunction(func);
	return function_set;
}

// ============================================================================
// Public Function Registration
// ============================================================================

duckdb::TableFunctionSet CreateDeltaShareShowSharesFunction() {
	return CreateDeltaShareShowSharesFunctionInternal();
}

duckdb::TableFunctionSet CreateDeltaShareShowSchemasFunction() {
	return CreateDeltaShareShowSchemasFunctionInternal();
}

duckdb::TableFunctionSet CreateDeltaShareShowTablesFunction() {
	return CreateDeltaShareShowTablesFunctionInternal();
}

} // namespace erpl_web
