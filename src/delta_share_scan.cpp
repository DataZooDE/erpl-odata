#include "delta_share_scan.hpp"
#include "duckdb_compat.hpp"
#include "odata_url_helpers.hpp"

#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"

#include <map>
#include "tracing.hpp"
#include "yyjson.hpp"
#include "telemetry.hpp"

using namespace duckdb_yyjson;

namespace erpl_web {

using duckdb::PostHogTelemetry;

// =====================================================================
// Bind Phase
// =====================================================================

static unique_ptr<FunctionData> DeltaShareScanBind(ClientContext& context,
                                                   TableFunctionBindInput& input,
                                                   vector<LogicalType>& return_types,
                                                   duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("delta_share_scan");
    ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "Bind phase starting");

    auto bind_data = make_uniq<DeltaShareScanBindData>();

    // Parse parameters: profile_path, share, schema, table
    if (input.inputs.size() < 4) {
        throw InvalidInputException("delta_share_scan requires 4 parameters: "
                                   "(profile_path, share, schema, table)");
    }

    try {
        // Load profile from local or remote path (HTTP/S3 supported via DuckDB FileSystem API)
        bind_data->profile = DeltaShareProfile::FromFile(context, input.inputs[0].GetValue<string>());
    } catch (const std::exception& e) {
        throw InvalidInputException("Failed to load Delta Sharing profile: " + string(e.what()));
    }

    bind_data->share = input.inputs[1].GetValue<string>();
    bind_data->schema = input.inputs[2].GetValue<string>();
    bind_data->table = input.inputs[3].GetValue<string>();

    ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN",
                    "Fetching metadata for: " + bind_data->share + "." + bind_data->schema + "." +
                        bind_data->table);

    // Create client and fetch metadata
    try {
        auto client = duckdb::make_shared_ptr<DeltaShareClient>(context, bind_data->profile);
        bind_data->metadata = client->GetTableMetadata(bind_data->share, bind_data->schema, bind_data->table);
    } catch (const std::exception& e) {
        throw InvalidInputException("Failed to fetch Delta Sharing table metadata: " + string(e.what()));
    }

    // Extract column names and types from Delta schema JSON
    if (bind_data->metadata.schema_json.empty()) {
        // Fallback: if no schema extracted, use a simple schema
        return_types.push_back(LogicalType::VARCHAR);
        names.emplace_back("data");
        ERPL_TRACE_WARN("DELTA_SHARE_SCAN", "Using fallback schema - no schema JSON extracted from metadata");
    } else {
        // Parse schemaString to extract column names and types
        try {
            auto doc = std::shared_ptr<yyjson_doc>(
                yyjson_read(bind_data->metadata.schema_json.c_str(), bind_data->metadata.schema_json.size(), 0),
                yyjson_doc_free
            );

            if (!doc) {
                throw InvalidInputException("Failed to parse schema JSON");
            }

            auto root = yyjson_doc_get_root(doc.get());
            auto fields_arr = yyjson_obj_get(root, "fields");

            if (!fields_arr || !yyjson_is_arr(fields_arr)) {
                throw InvalidInputException("Schema has no 'fields' array");
            }

            // Iterate through fields and extract names and types
            size_t idx, max;
            yyjson_val* field_item;
            yyjson_arr_foreach(fields_arr, idx, max, field_item) {
                if (!yyjson_is_obj(field_item)) {
                    continue;
                }

                // Extract field name
                auto name_val = yyjson_obj_get(field_item, "name");
                if (!name_val || !yyjson_is_str(name_val)) {
                    continue;
                }
                string field_name = string(yyjson_get_str(name_val));

                // Extract field type
                auto type_val = yyjson_obj_get(field_item, "type");
                if (!type_val || !yyjson_is_str(type_val)) {
                    return_types.push_back(LogicalType::VARCHAR);
                    names.emplace_back(field_name);
                    continue;
                }

                string delta_type = string(yyjson_get_str(type_val));
                if (!IsKnownDeltaType(delta_type)) {
                    // Refuse rather than cast it to VARCHAR behind the caller's back: the
                    // file's real values would be stringified silently (GitHub #207).
                    throw duckdb::NotImplementedException(
                        "Delta Sharing column '" + field_name + "' has type '" + delta_type +
                        "', which this reader does not support yet.");
                }
                LogicalType duckdb_type = ConvertDeltaTypeToLogicalType(delta_type);
                return_types.push_back(duckdb_type);
                names.emplace_back(field_name);
            }

            if (names.empty()) {
                throw InvalidInputException("No fields extracted from schema");
            }

            ERPL_TRACE_INFO("DELTA_SHARE_SCAN", "Using " + std::to_string(names.size()) + " columns from metadata: " +
                           names[0] + (names.size() > 1 ? ", " + names[1] : "") + (names.size() > 2 ? ", ..." : ""));

        } catch (const duckdb::NotImplementedException&) {
            // An unsupported column type is a deliberate refusal, not a parse failure. The
            // fallback below would turn it into a single VARCHAR "data" column and report
            // success, which is exactly the silent stringification the check exists to
            // prevent, so it is rethrown rather than swallowed.
            throw;
        } catch (const std::exception& e) {
            ERPL_TRACE_ERROR("DELTA_SHARE_SCAN", "Error parsing schema: " + string(e.what()));
            // Fallback to simple schema
            return_types.push_back(LogicalType::VARCHAR);
            names.emplace_back("data");
            ERPL_TRACE_WARN("DELTA_SHARE_SCAN", "Using fallback schema after parsing error");
        }
    }

    bind_data->column_types = return_types;
    bind_data->column_names = duckdb_compat::Strs(names);

    ERPL_TRACE_INFO("DELTA_SHARE_SCAN", "Bind phase complete");

    return bind_data;
}

// =====================================================================
// Init Global Phase (fetches metadata and file list once)
// =====================================================================

static unique_ptr<GlobalTableFunctionState> DeltaShareScanInitGlobal(ClientContext& context,
                                                                     TableFunctionInitInput& input) {
    ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "InitGlobal phase starting");

    auto& bind_data = input.bind_data->Cast<DeltaShareScanBindData>();
    auto global_state = make_uniq<DeltaShareGlobalState>();

    // Create shared client for metadata fetching
    global_state->client = duckdb::make_shared_ptr<DeltaShareClient>(context, bind_data.profile);

    // Store metadata from bind phase
    global_state->metadata = bind_data.metadata;

    // Fetch complete file list with pre-signed URLs from Delta Sharing server
    // This is done once in InitGlobal and shared read-only across all threads
    try {
        global_state->files = global_state->client->QueryTable(bind_data.share, bind_data.schema, bind_data.table);
        ERPL_TRACE_INFO("DELTA_SHARE_SCAN", "Fetched " + std::to_string(global_state->files.size()) + " files from Delta Sharing");

        // Every one of these URLs was chosen by the SHARE SERVER and is about to be handed
        // to parquet_scan, which resolves whatever it is given - including file:// and bare
        // filesystem paths, and globs them. A hostile or compromised share could therefore
        // read arbitrary local files and return their contents through the query. The Delta
        // Sharing protocol issues pre-signed https URLs, so anything else is refused here,
        // at the point the list arrives, rather than at the first read (GitHub #207).
        //
        // Requiring an http(s) scheme also disposes of the glob concern: globbing is a
        // filesystem operation and does not apply to a remote URL. Note that '?' cannot be
        // rejected - every pre-signed URL carries a query string.
        for (const auto& file_ref : global_state->files) {
            // Absolute http(s) FIRST. RequireSecureOrLoopbackUrl alone is not enough here:
            // it deliberately returns for input carrying no scheme at all, because its other
            // callers accept a bare name and resolve it themselves. For this caller a bare
            // name IS the attack - "/etc/passwd", "../../secrets.parquet", "*.parquet" would
            // sail through and be resolved against the local filesystem by parquet_scan.
            if (!LooksLikeAbsoluteHttpUrl(file_ref.url)) {
                throw duckdb::InvalidInputException(
                    "A Delta Sharing data file URL must be an absolute http(s) URL; the share "
                    "server returned '%s', which would be resolved against the local "
                    "filesystem.", file_ref.url.c_str());
            }
            RequireSecureOrLoopbackUrl(file_ref.url, "A Delta Sharing data file URL");
        }

        if (global_state->files.empty()) {
            ERPL_TRACE_WARN("DELTA_SHARE_SCAN", "No files found for table");
        } else {
            // Log first few file URLs for debugging
            for (size_t i = 0; i < global_state->files.size() && i < 3; ++i) {
                ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "File " + std::to_string(i) + ": " +
                    global_state->files[i].url.substr(0, 80) + "... (size: " +
                    std::to_string(global_state->files[i].size) + " bytes)");
            }
        }
    } catch (const std::exception& e) {
        throw InvalidInputException("Failed to query Delta Sharing table: " + string(e.what()));
    }

    ERPL_TRACE_INFO("DELTA_SHARE_SCAN", "InitGlobal phase complete");

    return global_state;
}

// =====================================================================
// Init Local Phase (creates per-thread HTTP client)
// =====================================================================

static unique_ptr<LocalTableFunctionState> DeltaShareScanInitLocal(ExecutionContext& context,
                                                                   TableFunctionInitInput& input,
                                                                   GlobalTableFunctionState* gstate) {
    ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "InitLocal phase starting for thread");

    auto& global_state = *static_cast<DeltaShareGlobalState*>(gstate);
    auto local_state = make_uniq<DeltaShareLocalState>();

    // Create per-thread HTTP client
    // Each thread gets its own client instance for connection reuse via keep-alive
    // No global synchronization needed—lock-free design
    try {
        local_state->http_client = global_state.client;  // Reuse shared client, it's thread-safe
        ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "Per-thread HTTP client reference initialized");
    } catch (const std::exception& e) {
        ERPL_TRACE_ERROR("DELTA_SHARE_SCAN", "Failed to create per-thread HTTP client: " + string(e.what()));
        throw InvalidInputException("Failed to create per-thread HTTP client: " + string(e.what()));
    }

    ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "InitLocal phase complete");

    return local_state;
}

// =====================================================================
// Scan Phase (with atomic lock-free work distribution)
// =====================================================================

// Opens the next file this thread claims, or returns false when the table is exhausted.
// The reader is left on the local state so the caller can drain it across scan calls.
// Builds the projection that aligns one parquet file to the bound schema.
//
// BY NAME, not by position. The share server supplies both the declared schema and the
// files, and Delta tables legitimately evolve, so the file's column order need not match
// the schema's. A positional mapping silently returns one column's values under another
// column's name - wrong data with no error, which is worse than the INTERNAL crash this
// projection was added to prevent. A bound column the file does not carry is filled with
// NULL, which is what schema evolution means for a column added after a file was written.
//
// Built as parsed expressions rather than SQL text: the file's column names come from the
// share server, and nothing share-supplied should ever be parsed as SQL.
// Partition names are matched the same way file column names are - exact first, then
// case-insensitively. Having the two lookups disagree beside each other would mean a file
// whose casing differs lines up while its partition columns silently do not.
static map<string, string>::const_iterator FindPartitionValue(
    const map<string, string>& partition_values, const string& wanted) {
    const auto exact = partition_values.find(wanted);
    if (exact != partition_values.end()) {
        return exact;
    }
    const auto lowered_wanted = duckdb::StringUtil::Lower(wanted);
    for (auto it = partition_values.begin(); it != partition_values.end(); ++it) {
        if (duckdb::StringUtil::Lower(it->first) == lowered_wanted) {
            return it;
        }
    }
    return partition_values.end();
}

static duckdb::vector<duckdb::unique_ptr<duckdb::ParsedExpression>> BuildAlignmentProjection(
    const vector<string>& column_names, const vector<LogicalType>& column_types,
    const duckdb::vector<duckdb::ColumnDefinition>& file_columns,
    const map<string, string>& partition_values) {

    // Case-sensitive match first, then a case-insensitive fallback, so a file that differs
    // only in capitalisation still lines up rather than silently becoming all NULLs.
    std::map<string, string> by_exact_name;
    std::map<string, string> by_lowered_name;
    for (const auto& column : file_columns) {
        by_exact_name.emplace(duckdb_compat::Str(column.Name()), duckdb_compat::Str(column.Name()));
        by_lowered_name.emplace(duckdb::StringUtil::Lower(duckdb_compat::Str(column.Name())), duckdb_compat::Str(column.Name()));
    }

    duckdb::vector<duckdb::unique_ptr<duckdb::ParsedExpression>> expressions;
    expressions.reserve(column_types.size());


    for (idx_t i = 0; i < column_types.size(); i++) {
        const auto& wanted = column_names[i];
        const auto& wanted_type = column_types[i];

        const auto exact = by_exact_name.find(wanted);
        const auto lowered = by_lowered_name.find(duckdb::StringUtil::Lower(wanted));

        duckdb::unique_ptr<duckdb::ParsedExpression> source;
        if (exact != by_exact_name.end()) {
            source = duckdb::make_uniq<duckdb::ColumnRefExpression>(duckdb_compat::Ident(exact->second));
        } else if (lowered != by_lowered_name.end()) {
            source = duckdb::make_uniq<duckdb::ColumnRefExpression>(duckdb_compat::Ident(lowered->second));
        } else if (const auto partition = FindPartitionValue(partition_values, wanted);
                   partition != partition_values.end()) {
            // A partition column: its value is not in the file, it is carried per file in
            // the protocol. Emitted as a constant and cast like any other column, so a
            // partitioned table groups by real values instead of a single NULL group.
            //
            // The empty string is NULL, for every type. Delta Sharing PROTOCOL.md, under
            // Partition Value Serialization: "An empty string for any type translates to a
            // null partition value." Emitting it literally made '' fail the cast on a DATE
            // column - taking the whole scan with it - and produced '' instead of NULL on a
            // VARCHAR one.
            if (partition->second.empty()) {
                ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "Partition column '" + wanted +
                                                         "' has an empty value, which is NULL");
                source = duckdb_compat::ParsedConstant(duckdb::Value(wanted_type));
            } else {
                ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "Column '" + wanted +
                                                         "' comes from the file's partition values");
                source =
                    duckdb_compat::ParsedConstant(duckdb::Value(partition->second));
            }
        } else {
            ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN",
                             "Column '" + wanted + "' is absent from this file; filling NULL");
            source = duckdb_compat::ParsedConstant(duckdb::Value(wanted_type));
        }

        expressions.push_back(
            duckdb::make_uniq<duckdb::CastExpression>(wanted_type, std::move(source)));
    }

    return expressions;
}

void DeltaShareFileReader::Open(duckdb::ClientContext& context, const std::string& file_url,
                                const vector<string>& column_names,
                                const vector<LogicalType>& column_types,
                                const map<string, string>& partition_values) {
    Close();

    // The URL is chosen by the SHARE SERVER, so it is passed as a bound VALUE and never
    // concatenated into SQL text. Building "SELECT * FROM parquet_scan('" + url + "')" let
    // a URL containing a quote close the literal, and Connection::Query accepts several
    // statements - so a hostile share server could run arbitrary SQL in the caller's
    // session, with whatever attachments and secrets it holds (GitHub #207).
    connection = duckdb::make_uniq<duckdb::Connection>(*context.db);

    // Binding the relation is itself a read - a missing or unreadable file raises here,
    // before any row is fetched - so the whole open is wrapped and reported as one thing.
    // Otherwise the caller gets a bare parquet error that never mentions Delta Sharing or
    // which file of the table failed.
    std::string failure;
    try {
        auto relation = connection->TableFunction("parquet_scan", {duckdb::Value(file_url)});

        // Align the file to the schema the plan was bound to. Without this a file whose
        // types merely differ from the declared schema - which the share server also
        // supplies - is referenced into an output vector of another type and raises an
        // INTERNAL error.
        if (!column_types.empty()) {
            // The relation is already bound by the TableFunction call above; Columns() just
            // reads the schema that bind produced. (An earlier comment here credited
            // Columns() with doing the binding, which is why a missing file raises before
            // this line, not at it.)
            const auto& file_columns = relation->Columns();
            relation = relation->Project(
                BuildAlignmentProjection(column_names, column_types, file_columns, partition_values),
            column_names);
        }

        result = relation->Execute();

        // A file that cannot be read fails the QUERY. Swallowing it into an empty chunk
        // ended the scan silently, so one bad file truncated the whole table and reported
        // success (GitHub #207).
        if (result->HasError()) {
            failure = result->GetError();
        }
    } catch (const std::exception& e) {
        failure = e.what();
    }

    if (!failure.empty()) {
        Close();
        throw duckdb::IOException("Failed to read Delta Sharing file (" +
                                  file_url.substr(0, 80) + "): " + failure);
    }
}

duckdb::DataChunk *DeltaShareFileReader::NextChunk() {
    if (!result) {
        return nullptr;
    }
    current_chunk = result->Fetch();
    if (current_chunk && current_chunk->size() > 0) {
        return current_chunk.get();
    }
    current_chunk.reset();
    return nullptr;
}

void DeltaShareFileReader::Close() {
    current_chunk.reset();
    result.reset();
    connection.reset();
}

// Opens the next file this thread claims, or returns false when the table is exhausted.
static bool ClaimNextFile(ClientContext& context, const DeltaShareScanBindData& bind_data,
                          DeltaShareGlobalState& global_state,
                          DeltaShareLocalState& local_state) {
    const idx_t file_idx = global_state.current_file_index.fetch_add(1);
    if (file_idx >= global_state.files.size()) {
        return false;
    }

    const auto& file_ref = global_state.files[file_idx];
    ERPL_TRACE_INFO("DELTA_SHARE_SCAN",
                    "Reading Parquet file " + std::to_string(file_idx) + "/" +
                        std::to_string(global_state.files.size()) + ": " +
                        file_ref.url.substr(0, 80) + "...");

    local_state.reader.Open(context, file_ref.url, bind_data.column_names,
                            bind_data.column_types, file_ref.partition_values);
    return true;
}

static void DeltaShareScan(ClientContext& context, TableFunctionInput& input, DataChunk& output) {
    auto& bind_data = input.bind_data->Cast<DeltaShareScanBindData>();
    auto& global_state = input.global_state->Cast<DeltaShareGlobalState>();
    auto& local_state = input.local_state->Cast<DeltaShareLocalState>();

    // Drain the current file completely before claiming the next one. Each iteration either
    // emits one chunk and returns, or exhausts a file and moves on.
    while (true) {
        if (local_state.reader.IsOpen()) {
            if (auto *chunk = local_state.reader.NextChunk()) {
                // The chunk is owned by the reader, which outlives this call.
                output.Reference(*chunk);
                return;
            }
            local_state.reader.Close();  // file exhausted
        }

        if (!ClaimNextFile(context, bind_data, global_state, local_state)) {
            ERPL_TRACE_DEBUG("DELTA_SHARE_SCAN", "All files processed, thread returning empty result");
            duckdb_compat::SetChildCardinality(output, 0);
            return;
        }
    }
}

// =====================================================================
// Table Function Registration
// =====================================================================

TableFunctionSet CreateDeltaShareScanFunction() {
    ERPL_TRACE_DEBUG("DELTA_SHARE", "Registering delta_share_scan table function");

    TableFunctionSet function_set("delta_share_scan");

    // Create table function with parallel execution support
    // - Bind phase: parse parameters and fetch metadata
    // - InitGlobal phase: fetch complete file list (once, shared across threads)
    // - InitLocal phase: create per-thread HTTP client for connection reuse
    // - Scan phase: atomic lock-free work distribution via file index
    TableFunction scan_function(
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        DeltaShareScan,
        DeltaShareScanBind,
        DeltaShareScanInitGlobal,  // InitGlobal: metadata + file list
        DeltaShareScanInitLocal);  // InitLocal: per-thread HTTP client

    function_set.AddFunction(scan_function);

    return function_set;
}

} // namespace erpl_web
