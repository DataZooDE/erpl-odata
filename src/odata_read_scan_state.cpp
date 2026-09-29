#include "duckdb/function/table_function.hpp"

#include "datazoo/oauth2/http_client.hpp"
#include "odata_edm.hpp"
#include "odata_expand_parser.hpp"
#include "odata_read_functions.hpp"
#include "odata_url_helpers.hpp"
#include "yyjson.hpp"

#include <unordered_set>

#include "tracing.hpp"
#include "telemetry.hpp"
#include "erpl_odata_banner.hpp"

// Mutable scan state for one execution of an OData scan: the row buffer and
// progress tracker it owns, the page-fetch/row-emit loop, CloneForScan, the
// first-page prefetch, and the DuckDB scan callbacks driving them.
//
// Row buffering is kept here rather than in a file of its own: ODataRowBuffer
// and ODataProgressTracker are the scan's state containers and every caller of
// theirs is in this file, so separating them would only produce two 35-line
// translation units.
//
// Declarations live in src/include/odata_read_functions.hpp.

namespace erpl_web {

// ============================================================================
// ODataProgressTracker Implementation
// ============================================================================

void ODataProgressTracker::SetTotalCount(uint64_t total) {
    total_count_ = total;
    has_total_ = true;
}

void ODataProgressTracker::IncrementRowsFetched(uint64_t count) {
    rows_fetched_ += count;
}

// Despite the name, the value is a PERCENTAGE (0-100), not a fraction. That is what
// DuckDB's table-function contract wants: PhysicalTableScan::GetProgress assigns the
// returned value to ProgressData::done and sets ProgressData::total = 100.0
// (duckdb/src/execution/operator/scan/physical_table_scan.cpp). A negative value means
// "unknown" and makes DuckDB hide the progress bar.
double ODataProgressTracker::GetProgressFraction() const {
    if (!has_total_ || total_count_ == 0) {
        return -1.0; // unknown -> DuckDB will not show progress
    }
    double progress = 100.0 * (double)rows_fetched_ / (double)total_count_;
  if (progress < 0.0) {
    progress = 0.0;
  }
    return std::min(100.0, progress);
}

bool ODataProgressTracker::HasTotalCount() const { return has_total_; }

uint64_t ODataProgressTracker::GetTotalCount() const { return total_count_; }

uint64_t ODataProgressTracker::GetRowsFetched() const { return rows_fetched_; }

void ODataProgressTracker::Reset() {
    rows_fetched_ = 0;
    total_count_ = 0;
    has_total_ = false;
}

// ============================================================================
// ODataRowBuffer Implementation
// ============================================================================

void ODataRowBuffer::AddRows(std::vector<std::vector<duckdb::Value>> rows) {
    for (auto &row : rows) {
        row_buffer_.emplace_back(std::move(row));
    }
}

std::vector<duckdb::Value> ODataRowBuffer::GetNextRow() {
    if (row_buffer_.empty()) {
        return {};
    }
    auto row = std::move(row_buffer_.front());
    row_buffer_.pop_front();
    return row;
}

bool ODataRowBuffer::HasMoreRows() const { return !row_buffer_.empty(); }

size_t ODataRowBuffer::Size() const { return row_buffer_.size(); }

std::vector<std::vector<duckdb::Value>> ODataRowBuffer::CopyRows() const {
    return std::vector<std::vector<duckdb::Value>>(row_buffer_.begin(),
                                                   row_buffer_.end());
}

void ODataRowBuffer::Clear() { row_buffer_.clear(); }

void ODataRowBuffer::SetHasNextPage(bool has_next) {
    has_next_page_ = has_next;
}

bool ODataRowBuffer::HasNextPage() const { return has_next_page_; }

// ============================================================================
// FetchNextResult Helper Methods
// ============================================================================

void ODataReadBindData::EnsureInitialized() {
    // Ensure input parameters are set on client before any request
    if (!input_parameters.empty()) {
        odata_client->SetInputParameters(input_parameters);
    }

    // Make sure first page is prefetched once and buffered
    if (!first_page_cached_) {
        // PrefetchFirstPage buffers through BufferFirstPageFromResponse, which extracts
        // expanded data itself - the schema is settled by the time a scan runs.
        PrefetchFirstPage();
    } else {
        // The buffered page came from the bind-time probe, which ran before the expand
        // clause was parsed, so its expanded data was never extracted. Do it now, into
        // THIS scan's extractor. See GitHub #156.
        ExtractExpandedDataFromBufferedFirstPage();
    }
}

SchemaInfo ODataReadBindData::PrepareSchemaInfo() {
    SchemaInfo info;
    
    // Always use full schema for buffering; projection is applied at emission
    info.all_result_names = GetResultNames(true);
    info.all_result_types = GetResultTypes(true);
    info.has_expand = HasExpandedData();
    
    // Build name->index map for robust lookups
    info.name_to_index.reserve(info.all_result_names.size());
    for (duckdb::idx_t k = 0; k < (duckdb::idx_t)info.all_result_names.size(); ++k) {
        info.name_to_index.emplace(info.all_result_names[k], k);
    }
    
    return info;
}

void ODataReadBindData::FetchAdditionalPagesIfNeeded(
    duckdb::optional_ptr<duckdb::ClientContext> context,
    const SchemaInfo& schema_info) {
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t pages_fetched = 0;

    // Fetch additional pages until we have enough buffered rows to fill the
    // vector or no more pages
    while (row_buffer->Size() < target && row_buffer->HasNextPage()) {
        // Ctrl-C and statement timeouts both arrive as an interrupt flag on the
        // client context; a paging loop that never observes it is unstoppable.
        if (context != nullptr && context->interrupted) {
            ERPL_TRACE_INFO("ODATA_READ_BIND", "Interrupted while fetching additional pages");
            throw duckdb::InterruptException();
        }

        // Only yield once there is something to emit: DuckDB ends a table scan
        // on the first empty chunk, so breaking with an empty buffer would
        // silently truncate the result. A run of empty pages is bounded by the
        // interrupt check above and by the client's page ceiling instead.
        if (pages_fetched >= MAX_PAGES_PER_SCAN_CALL && row_buffer->Size() > 0) {
            ERPL_TRACE_DEBUG(
                "ODATA_READ_BIND",
                duckdb::StringUtil::Format(
                    "Reached the per-scan-call page cap of %llu; emitting a short vector and "
                    "resuming on the next call",
                    (unsigned long long)MAX_PAGES_PER_SCAN_CALL));
            break;
        }

        auto next_response = odata_client->Get(true);
        pages_fetched++;
        if (!next_response) {
            row_buffer->SetHasNextPage(false);
            break;
        }
        
        ProcessPageResponse(next_response, schema_info);
    }
}

void ODataReadBindData::ProcessPageResponse(
    std::shared_ptr<ODataEntitySetResponse> response, 
    const SchemaInfo& schema_info) {
    
    // Capture total count once for progress
    if (response->GetODataVersion() == ODataVersion::V4) {
        auto total = response->Content()->TotalCount();
        if (total.has_value() && (!progress_tracker->HasTotalCount() ||
                                  progress_tracker->GetTotalCount() == 0)) {
            progress_tracker->SetTotalCount(total.value());
        }
    }
    
    // Extract expanded data from subsequent pages as well
    if (schema_info.has_expand && !data_extractor->GetExpandedDataSchema().empty()) {
        try {
            ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                           "Extracting expanded data from subsequent page");
            data_extractor->ExtractExpandedDataFromResponse(response->RawContent());
        } catch (const StrictTypingViolation &) {
            // strict_typing must fail the query, not be downgraded to a warning.
            throw;
        } catch (const std::exception &e) {
            ERPL_TRACE_WARN(
                "ODATA_READ_BIND",
                "Failed to extract expanded data from subsequent page: " +
                    std::string(e.what()));
        }
    }

    // Buffer full rows using full schema to keep indices stable
    auto column_names = schema_info.all_result_names;
    auto column_types = schema_info.all_result_types;
    response->Content()->SetConversionFailureLog(conversion_failure_log);
    auto page_rows = response->ToRows(column_names, column_types);
    const auto row_count = page_rows.size();
    row_buffer->AddRows(std::move(page_rows));
    row_buffer->SetHasNextPage(response->NextUrl().has_value());
    // Deliberately no IncrementRowsFetched here. Progress counts rows HANDED TO DuckDB,
    // and UpdateProgressTracking already counts them as they are emitted. Counting them
    // again on arrival double-counted every page after the first - the first page is
    // buffered by BufferFirstPageFromResponse, which never incremented - so progress
    // saturated at a clamped 100% while barely two thirds of the rows had been delivered.
    // See GitHub #160.
    (void)row_count;
}

idx_t ODataReadBindData::EmitRowsToOutput(duckdb::DataChunk &output, const SchemaInfo& schema_info) {
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t to_emit = std::min<idx_t>(row_buffer->Size(), target);
    
    for (idx_t i = 0; i < to_emit; i++) {
        const auto &row = row_buffer->GetNextRow();
        EmitSingleRowToOutput(output, row, i, schema_info);
        emitted_row_index_++;
    }
    
    output.SetCardinality(to_emit);

    // Rows are emitted strictly in order, so everything before the current position can
    // never be read again. Releasing here bounds the expand cache to the rows still in
    // flight instead of the whole scan (GitHub #159).
    if (data_extractor && HasExpandedData()) {
        data_extractor->ReleaseExpandedDataBefore(static_cast<duckdb::idx_t>(emitted_row_index_));
    }

    return to_emit;
}

void ODataReadBindData::EmitSingleRowToOutput(
    duckdb::DataChunk &output, 
    const std::vector<duckdb::Value> &row, 
    idx_t row_index,
    const SchemaInfo& schema_info) {
    
    auto null_value = duckdb::Value();
    
    idx_t data_slot = 0;
    for (idx_t j = 0; j < output.ColumnCount(); j++) {
        if (j < output_column_is_row_id.size() && output_column_is_row_id[j]) {
            // A synthetic row id: monotonically increasing over the scan, which is all a
            // row id has to be here. Emitting a data value would be a type mismatch.
            output.SetValue(j, row_index,
                            duckdb::Value::BIGINT(static_cast<int64_t>(emitted_row_index_)));
            continue;
        }

        duckdb::idx_t original_column_index = GetOriginalColumnIndex(data_slot);
        data_slot++;

        if (original_column_index < schema_info.all_result_names.size()) {
            auto value = GetColumnValue(original_column_index, row, schema_info);
            output.SetValue(j, row_index, value);
        } else {
            auto col_type = output.data[j].GetType();
            output.SetValue(j, row_index, null_value.DefaultCastAs(col_type));
        }
    }
}

duckdb::idx_t ODataReadBindData::GetOriginalColumnIndex(idx_t activated_column_index) const {
    return (activated_column_index < activated_to_original_mapping.size())
               ? activated_to_original_mapping[activated_column_index]
               : activated_column_index;
}

duckdb::Value ODataReadBindData::GetColumnValue(
    duckdb::idx_t original_column_index,
    const std::vector<duckdb::Value> &row,
    const SchemaInfo& schema_info) {
    
    auto null_value = duckdb::Value();
    
    // Check if this original index points to an expanded column
    if (IsExpandedColumn(original_column_index, schema_info)) {
        return GetExpandedColumnValue(original_column_index, schema_info);
    } else {
        return GetRegularColumnValue(original_column_index, row, schema_info);
    }
}

bool ODataReadBindData::IsExpandedColumn(duckdb::idx_t original_column_index, const SchemaInfo& schema_info) const {
    return schema_info.has_expand && 
           !data_extractor->GetExpandedDataSchema().empty() &&
           original_column_index >= (schema_info.all_result_names.size() - 
                                   data_extractor->GetExpandedDataSchema().size());
}

duckdb::Value ODataReadBindData::GetExpandedColumnValue(
    duckdb::idx_t original_column_index, 
    const SchemaInfo& schema_info) {
    
    auto null_value = duckdb::Value();
    
    size_t expand_index = original_column_index - 
                         (schema_info.all_result_names.size() - 
                          data_extractor->GetExpandedDataSchema().size());
    
    if (expand_index < data_extractor->GetExpandedDataSchema().size()) {
        std::string expand_path = data_extractor->GetExpandedDataSchema()[expand_index];
        
        // The cache is filled per input row, so the scan-global emitted index is the key.
        auto expand_data = data_extractor->ExtractExpandedDataForRow(
            static_cast<duckdb::idx_t>(emitted_row_index_), expand_path);
        
        if (!expand_data.IsNull()) {
            ERPL_TRACE_DEBUG("ODATA_SCAN", duckdb::StringUtil::Format("Setting expanded data for path '%s'", expand_path.c_str()));
            return expand_data;
        } else {
            ERPL_TRACE_DEBUG("ODATA_SCAN", duckdb::StringUtil::Format("Setting expanded data to NULL for path '%s'", expand_path.c_str()));
            return null_value;
        }
    }
    
    return null_value;
}

duckdb::Value ODataReadBindData::GetRegularColumnValue(
    duckdb::idx_t original_column_index,
    const std::vector<duckdb::Value> &row,
    const SchemaInfo& schema_info) {
    
    auto null_value = duckdb::Value();
    
    // Regular base column from buffered full row: resolve by name to guard against index drift
    const std::string &col_name = schema_info.all_result_names[original_column_index];
    auto it = schema_info.name_to_index.find(col_name);
    
    if (it != schema_info.name_to_index.end() && it->second < row.size()) {
        return row[it->second];
    } else {
        return null_value;
    }
}

void ODataReadBindData::UpdateProgressTracking(idx_t rows_emitted) {
    progress_tracker->IncrementRowsFetched(rows_emitted);
    
    if (progress_tracker->HasTotalCount() && progress_tracker->GetTotalCount() > 0) {
        double pct = 100.0 * (double)progress_tracker->GetRowsFetched() /
                     (double)progress_tracker->GetTotalCount();
        if (pct > 100.0) {
            pct = 100.0;
        }
        
        ERPL_TRACE_INFO("ODATA_SCAN",
                        duckdb::StringUtil::Format(
                            "Progress: %.2f%% (%llu/%llu)", pct,
                            (unsigned long long)progress_tracker->GetRowsFetched(),
                            (unsigned long long)progress_tracker->GetTotalCount()));
    }
}

unsigned int ODataReadBindData::FetchNextResult(duckdb::DataChunk &output) {
    return FetchNextResultInternal(nullptr, output);
}

unsigned int ODataReadBindData::FetchNextResult(duckdb::ClientContext &context,
                                                duckdb::DataChunk &output) {
    return FetchNextResultInternal(&context, output);
}

unsigned int ODataReadBindData::FetchNextResultInternal(
    duckdb::optional_ptr<duckdb::ClientContext> context,
    duckdb::DataChunk &output) {
    EnsureInitialized();

    auto schema_info = PrepareSchemaInfo();
    FetchAdditionalPagesIfNeeded(context, schema_info);

    idx_t rows_emitted = EmitRowsToOutput(output, schema_info);
    UpdateProgressTracking(rows_emitted);
    
    return rows_emitted;
}

bool ODataReadBindData::HasMoreResults() {
    // If buffer still has rows, we have results to emit
    if (row_buffer->HasMoreRows()) {
        return true;
    }
    // If first page hasn't been cached yet, we need to deliver it
    if (!first_page_cached_) {
        return true;
    }
    // Otherwise, only if server indicated a next page
    return row_buffer->HasNextPage();
}

duckdb::unique_ptr<ODataReadBindData> ODataReadBindData::CloneForScan() const {
  // The clone owns every piece of mutable scan state; the source keeps only
  // the schema and configuration settled during bind (GitHub #75).
  //
  // That includes the OData client: it carries the pagination cursor (url,
  // current_response, page_requests), so sharing one between executions makes the second
  // EXECUTE of a bound plan resume where the first stopped. A projecting plan used to get
  // a private client by accident, because applying $select rebuilt it; an unprojected
  // `SELECT *` changes no URL and rebuilds nothing, so the client is minted here instead
  // and the accident is no longer load-bearing. Service-root mode issues no requests and
  // keeps its stub.
  auto scan_client = odata_client;
  if (!service_root_mode_ && odata_client != nullptr) {
    // Carry the trusted origin explicitly: Url() is the pagination cursor, so deriving the
    // origin from it would make a followed next link the credentialed origin (GitHub #189).
    scan_client = std::make_shared<ODataEntitySetClient>(
        odata_client->GetHttpClient(), HttpUrl(odata_client->Url()),
        odata_client->ServiceOriginUrl(), odata_client->AuthParams());
    const auto bound_version = odata_client->GetODataVersion();
    if (bound_version != ODataVersion::UNKNOWN) {
      scan_client->SetODataVersionDirectly(bound_version);
    }
    // The bind-time probe already paid for page one; adopting it here lets this execution
    // follow that page's next link without re-fetching it, while keeping the cursor
    // private. Deliberately NOT odata_client->current_response: after one execution that
    // is the LAST page, and resuming from it would return nothing.
    if (const auto page_size = odata_client->GetMaxPageSize()) {
      scan_client->SetMaxPageSize(page_size.value());
    }
    // Datasphere's dual-URL shape: FromEntitySetRoot stores the @odata.context metadata
    // URL and the entity-set name from its fragment at bind time, gated on
    // IsDatasphereUrl. A clone that adopts page one never runs the code that derives them
    // (it lives in ODataEntitySetClient::Get()), so without carrying them the clone
    // resolves $metadata from the data URL and works from an empty entity-set name
    // (GitHub #186).
    if (!odata_client->StoredMetadataContextUrl().empty()) {
      scan_client->SetMetadataContextUrl(odata_client->StoredMetadataContextUrl());
    }
    if (!odata_client->GetEntitySetName().empty()) {
      scan_client->SetEntitySetName(odata_client->GetEntitySetName());
    }
    if (first_page_response_ != nullptr) {
      scan_client->AdoptResponse(first_page_response_);
    }
  }

  auto clone = duckdb::make_uniq<ODataReadBindData>(scan_client, true);
  clone->first_page_response_ = first_page_response_;
  // Carry the row count the service reported at bind time. The clone gets a fresh tracker,
  // so without this the scan has no denominator and DuckDB shows no progress at all.
  if (progress_tracker && clone->progress_tracker && progress_tracker->HasTotalCount()) {
    clone->progress_tracker->SetTotalCount(progress_tracker->GetTotalCount());
  }
  // first_page_expand_extracted_ is deliberately NOT copied. The clone gets a fresh
  // extractor with an empty expand cache, so this execution must extract page one's
  // expanded data into it again; copying the flag would leave the first page's expanded
  // columns NULL. See GitHub #156.

  clone->service_root_mode_ = service_root_mode_;
  clone->InitializeComponents(service_root_mode_);

  // InitializeComponents mints a fresh ConversionFailureLog for this execution, which is
  // what gives the log the right lifetime - but it also resets strict typing, so the
  // bind-time choice has to be re-applied or strict_typing=true silently does nothing.
  clone->SetStrictTyping(strict_typing_);

  // Schema and configuration: pure values, safe to copy.
  clone->all_result_names = all_result_names;
  clone->all_result_types = all_result_types;
  clone->extracted_column_names = extracted_column_names;
  clone->base_result_names = base_result_names;
  clone->base_result_types = base_result_types;
  clone->base_schema_resolved_ = base_schema_resolved_;
  clone->input_parameters = input_parameters;
  clone->expand_clause = expand_clause;
  clone->has_expanded_data = has_expanded_data;
  clone->active_column_ids = active_column_ids;
  clone->activated_to_original_mapping = activated_to_original_mapping;

  // The clone gets its own extractor so its per-row expand cache starts empty,
  // but it must carry the expand schema and the types inferred during bind.
  if (data_extractor && clone->data_extractor) {
    clone->data_extractor->SetExpandedDataSchema(
        data_extractor->GetExpandedDataSchema());
    clone->data_extractor->SetNestedExpandPaths(
        data_extractor->GetNestedExpandPaths());
    const auto &expanded_types = data_extractor->GetExpandedDataTypes();
    for (size_t i = 0; i < expanded_types.size(); ++i) {
      clone->data_extractor->UpdateExpandedColumnType(i, expanded_types[i]);
    }
  }

  // Carry the clauses generated at bind time (top/skip/expand/count from named
  // parameters) but re-point the column resolver at the clone.
  if (predicate_pushdown_helper) {
    clone->predicate_pushdown_helper =
        std::make_shared<ODataPredicatePushdownHelper>(*predicate_pushdown_helper);
    clone->BindPredicateColumnResolver();
  }

  // Carry rows buffered during bind. The ODP path hands a pre-fetched first
  // page in via FromEntitySetClient and the service-root path pre-buffers its
  // synthetic rows; dropping those here would make every execution re-issue a
  // bare GET (which SAP ODP answers with the entire dataset).
  if (row_buffer && clone->row_buffer) {
    clone->row_buffer->AddRows(row_buffer->CopyRows());
    clone->row_buffer->SetHasNextPage(row_buffer->HasNextPage());
  }
  clone->first_page_cached_ = first_page_cached_;

  ERPL_TRACE_DEBUG(
      "ODATA_READ_BIND",
      duckdb::StringUtil::Format(
          "Cloned bind data for scan: %d buffered rows, first_page_cached=%s",
          (int)(row_buffer ? row_buffer->Size() : 0),
          first_page_cached_ ? "true" : "false"));

  return clone;
}

double ODataReadBindData::GetProgressFraction() const {
    return progress_tracker->GetProgressFraction();
}

void ODataReadBindData::PrefetchFirstPage() {
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   std::string("PrefetchFirstPage: service_root_mode_ = ") +
                       (service_root_mode_ ? "true" : "false"));
    if (first_page_cached_) {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "PrefetchFirstPage: first page already cached, skipping");
        return;
    }
  if (service_root_mode_) {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Service-root mode: prefetch skipped (rows prebuffered)");
    first_page_cached_ = true;
    row_buffer->SetHasNextPage(false);
    return;
  }
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   "PrefetchFirstPage: starting first page fetch");

    // Ensure input parameters are set on client before any request
    if (!input_parameters.empty()) {
        odata_client->SetInputParameters(input_parameters);
    }

    auto response = odata_client->Get();
    BufferFirstPageFromResponse(response);

  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   duckdb::StringUtil::Format(
        "PrefetchFirstPage: buffered %d rows, has_next_page=%s",
                       (int)row_buffer->Size(),
                       row_buffer->HasNextPage() ? "true" : "false"));
}

void ODataReadBindData::ExtractExpandedDataFromBufferedFirstPage() {
  if (service_root_mode_ || first_page_response_ == nullptr || first_page_expand_extracted_) {
    return;
  }
  if (!HasExpandedData() || data_extractor->GetExpandedDataSchema().empty()) {
    return;
  }

  try {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Extracting expanded data from the buffered probe page now that the "
                     "expand schema is known");
    data_extractor->ExtractExpandedDataFromResponse(first_page_response_->RawContent());
    first_page_expand_extracted_ = true;
  } catch (const StrictTypingViolation &) {
    throw;
  } catch (const std::exception &e) {
    ERPL_TRACE_WARN("ODATA_READ_BIND",
                    std::string("Failed to extract expanded data from the buffered probe "
                                "page: ") +
                        e.what());
  }
}

void ODataReadBindData::BufferFirstPageFromResponse(
    std::shared_ptr<ODataEntitySetResponse> response) {
    if (!response) {
        ERPL_TRACE_WARN("ODATA_READ_BIND",
                        "BufferFirstPageFromResponse: no response received");
        first_page_cached_ = true;
        row_buffer->SetHasNextPage(false);
        return;
    }

    // Extract expanded data if we have expand clauses
    if (HasExpandedData() && !data_extractor->GetExpandedDataSchema().empty()) {
        try {
            ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                             "Extracting expanded data from buffered first page");
            data_extractor->ExtractExpandedDataFromResponse(response->RawContent());
        } catch (const StrictTypingViolation &) {
            // strict_typing must fail the query, not be downgraded to a warning.
            throw;
        } catch (const std::exception &e) {
            ERPL_TRACE_WARN("ODATA_READ_BIND",
                            std::string("Failed to extract expanded data from "
                                        "buffered first page: ") +
                                e.what());
        }
    }

    // Capture total count once for progress (v4 only, when available)
    if (response->GetODataVersion() == ODataVersion::V4) {
        auto total = response->Content()->TotalCount();
        if (total.has_value()) {
            progress_tracker->SetTotalCount(total.value());
            ERPL_TRACE_INFO(
                "ODATA_READ_BIND",
                duckdb::StringUtil::Format(
                    "Service reported total row count: %llu",
                    (unsigned long long)progress_tracker->GetTotalCount()));
        }
    }

    // Buffer full schema rows to keep indices stable across projections
    auto all_result_names_local = GetResultNames(true);
    auto all_result_types_local = GetResultTypes(true);

    response->Content()->SetConversionFailureLog(conversion_failure_log);
    auto page_rows = response->ToRows(all_result_names_local, all_result_types_local);
    row_buffer->AddRows(std::move(page_rows));
    row_buffer->SetHasNextPage(response->NextUrl().has_value());
    first_page_cached_ = true;
    // Whatever this page's expanded data was, it has been handled here; the re-extraction
    // hook in EnsureInitialized must not append it a second time.
    first_page_expand_extracted_ = true;
}

ODataReadGlobalState::ODataReadGlobalState(
    duckdb::unique_ptr<ODataReadBindData> scan_state)
    : scan_state(std::move(scan_state)) {
}

namespace {

// Returns the object that owns the scan state for this execution.
//
// odata_read(), the ATTACHed odata_table_scan, the SAC readers and both
// Datasphere readers all return ODataReadGlobalState, so each execution of a
// bound plan gets its own state.
//
// A caller that supplies anything else has no per-execution state, which used to fall
// back to scanning straight out of the shared bind data - the historical behaviour that
// made the second EXECUTE of a bound plan return nothing, silently. Failing here instead
// turns the next mis-wired reader into an immediate, diagnosable error rather than a
// wrong answer. NOT InternalException: DuckDB treats that as fatal to the whole database
// instance (ValidChecker::Invalidate), which is far too much for one mis-registered
// function - the query should fail, not the connection.
ODataReadBindData &ResolveScanState(const duckdb::FunctionData *bind_data,
                                    const GlobalTableFunctionState *global_state) {
  auto *odata_global = dynamic_cast<const ODataReadGlobalState *>(global_state);
  if (odata_global == nullptr) {
    throw duckdb::NotImplementedException(
        "OData scan has no per-execution state: the table function must register "
        "ODataReadTableInitGlobalState as its init_global");
  }
  return const_cast<ODataReadGlobalState *>(odata_global)->Scan();
}

} // namespace

unique_ptr<GlobalTableFunctionState>
ODataReadTableInitGlobalState(ClientContext &context,
                              TableFunctionInitInput &input) {
  auto &bind_data = input.bind_data->CastNoConst<ODataReadBindData>();

  // Projection, filter pushdown and the first-page prefetch all mutate scan
  // state, so they run against a private clone. The bind data stays exactly as
  // bind left it, which is what makes a second EXECUTE of the same prepared
  // statement (and a self-join of one odata_read call) work (GitHub #75).
  auto scan_state = bind_data.CloneForScan();

  scan_state->ActivateColumns(input.column_ids);
  scan_state->AddFilters(input.filters);
  scan_state->UpdateUrlFromPredicatePushdown();
  // Prefetch first page after URL is finalized so progress can show early and
  // tiny scans return immediately
  scan_state->PrefetchFirstPage();

  return duckdb::make_uniq<ODataReadGlobalState>(std::move(scan_state));
}

double ODataReadTableProgress(ClientContext &, const FunctionData *func_data,
                              const GlobalTableFunctionState *global_state) {
  return ResolveScanState(func_data, global_state).GetProgressFraction();
}

void ODataReadScan(ClientContext &context, TableFunctionInput &data,
                   DataChunk &output) {
  // Scan state lives on the per-execution clone owned by the global state, so a
  // bound plan can be executed more than once (GitHub #75). ResolveScanState
  // THROWS for a caller that supplies a bare GlobalTableFunctionState - the old
  // fallback to the bind data was removed, because falling back silently restored
  // the very defect this state exists to prevent. This comment still described the
  // fallback long after it was gone.
  auto &scan_state =
      ResolveScanState(data.bind_data.get(), data.global_state.get());

  ERPL_TRACE_DEBUG("ODATA_SCAN", "Starting OData scan operation");

  if (!scan_state.HasMoreResults()) {
    ERPL_TRACE_DEBUG("ODATA_SCAN", "No more results available");
    // End of scan: surface the per-column conversion failures once, against the
    // instance that actually accumulated them (GitHub #74).
    scan_state.ReportConversionFailures();
    return;
  }

  ERPL_TRACE_DEBUG("ODATA_SCAN", "Fetching next result set");
  // The context reaches the paging loop so a long scan stays interruptible and
  // yields between pages (GitHub #78).
  auto rows_fetched = scan_state.FetchNextResult(context, output);
  ERPL_TRACE_INFO("ODATA_SCAN",
                  duckdb::StringUtil::Format("Fetched %d rows", rows_fetched));

  if (!scan_state.HasMoreResults()) {
    scan_state.ReportConversionFailures();
  }
}

} // namespace erpl_web
