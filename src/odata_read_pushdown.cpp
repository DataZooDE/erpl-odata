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

// Projection and filter pushdown wiring for an OData scan: column activation
// and the activated -> original column mapping, DuckDB filter translation, the
// $select/$filter URL rebuild, and the lazily created
// ODataPredicatePushdownHelper together with its column-name resolver.
//
// Declarations live in src/include/odata_read_functions.hpp.

namespace erpl_web {

void ODataReadBindData::ActivateColumns(
    const std::vector<duckdb::column_t> &column_ids) {
    std::stringstream column_ids_str;
    for (auto &column_id : column_ids) {
        column_ids_str << column_id << ", ";
    }
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   duckdb::StringUtil::Format("Activating columns: %s",
                                              column_ids_str.str().c_str()));
    
    // Row ids are not data columns, so they take no part in $select - but they DO occupy a
    // slot in the output chunk, so which slots they are must be remembered. COUNT(*) asks
    // for nothing but a row id; forgetting that made the emit path fall back to "output
    // slot j is data column j" and write the first column's string into a BIGINT row-id
    // vector. See GitHub #132.
    std::vector<duckdb::column_t> visible_ids;
    visible_ids.reserve(column_ids.size());
    output_column_is_row_id.assign(column_ids.size(), false);
    for (size_t output_index = 0; output_index < column_ids.size(); ++output_index) {
        const auto column_id = column_ids[output_index];
        if (duckdb::IsRowIdColumnId(column_id)) {
            output_column_is_row_id[output_index] = true;
      ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                       "Skipping ROW_ID column from activation mapping");
            continue;
        }
        visible_ids.push_back(column_id);
    }

    active_column_ids = visible_ids;
    
    // Build the mapping from activated column index to original column name index
    activated_to_original_mapping.clear();
    activated_to_original_mapping.resize(visible_ids.size());
    
    for (size_t i = 0; i < visible_ids.size(); ++i) {
        activated_to_original_mapping[i] = visible_ids[i];
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     duckdb::StringUtil::Format(
                         "Mapping activated index %d to original index %d",
                         (int)i, (int)visible_ids[i]));
  }

  // Preserve existing predicate pushdown state (top/skip/expand/filter) across
  // activation Do NOT reset the helper; just update the column selection
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   std::string("ActivateColumns: service_root_mode_ = ") +
                       (service_root_mode_ ? "true" : "false"));
  if (!service_root_mode_) {
    // Refresh the helper's view of the schema before it builds $select: the
    // expand clause processed after helper creation can add columns, and the
    // types decide whether $select is safe at all (GitHub #86).
    PredicatePushdownHelper()->SetColumnSchema(GetResultNames(true),
                                               GetResultTypes(true));
    PredicatePushdownHelper()->ConsumeColumnSelection(visible_ids);
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     duckdb::StringUtil::Format(
                         "Select clause: %s",
                         PredicatePushdownHelper()->SelectClause().c_str()));
  } else {
    ERPL_TRACE_DEBUG(
        "ODATA_READ_BIND",
        "Service-root mode: skipping predicate pushdown helper creation");
  }
}

void ODataReadBindData::AddFilters(
    const duckdb::optional_ptr<duckdb::TableFilterSet> &filters) {
  if (service_root_mode_) {
    ERPL_TRACE_DEBUG(
        "ODATA_READ_BIND",
        "Service-root mode: ignoring filters for predicate pushdown");
    return;
  }
    if (filters && !filters->filters.empty()) {
        std::stringstream filters_str;
    for (auto &[projected_column_idx, filter] : filters->filters) {
      filters_str << "Column " << projected_column_idx << ": "
                  << filter->DebugToString() << std::endl;
    }
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     duckdb::StringUtil::Format("Adding %d filters: %s",
                                                filters->filters.size(),
                                                filters_str.str().c_str()));
        
        PredicatePushdownHelper()->ConsumeFilters(filters);
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     duckdb::StringUtil::Format(
                         "Filter clause: %s",
                         PredicatePushdownHelper()->FilterClause().c_str()));
    } else {
        ERPL_TRACE_DEBUG("ODATA_READ_BIND", "No filters to add");
    }
}

void ODataReadBindData::AddResultModifiers(
    const std::vector<duckdb::unique_ptr<duckdb::BoundResultModifier>>
        &modifiers) {
  // Deliberately does nothing (GitHub #90). DuckDB does not hand table
  // functions their bound result modifiers, so a SQL `LIMIT`/`OFFSET` never
  // reaches this extension and is NEVER translated into `$top`/`$skip`:
  // `SELECT * FROM odata_read(...) LIMIT 10` still pulls every page the
  // service offers. `$top`/`$skip` are produced only from the explicit
  // `top=`/`skip=` named parameters (see ODataReadBindHelpers::
  // ProcessNamedParameters) and their Datasphere/SAC equivalents.
  //
  // The code that used to walk the modifiers and synthesise those clauses has
  // been deleted because it made the read path look as though LIMIT pushdown
  // existed. This stub survives only so the (equally uncalled)
  // OdpODataReadBindData::AddResultModifiers keeps linking; delete both
  // together.
  (void)modifiers;
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   "AddResultModifiers is a no-op: LIMIT is not pushed to $top");
}

void ODataReadBindData::UpdateUrlFromPredicatePushdown() {
  if (service_root_mode_) {
    ERPL_TRACE_DEBUG(
        "ODATA_READ_BIND",
        "Service-root mode: skipping URL update from predicate pushdown");
    return;
  }
    ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Updating URL from predicate pushdown");
    ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Original URL: " + odata_client->Url());
    
    auto http_client = odata_client->GetHttpClient();
    auto auth_params = odata_client->AuthParams();
    auto prev_url_str = odata_client->Url();
  auto updated_url =
      PredicatePushdownHelper()->ApplyFiltersToUrl(odata_client->Url());
    
    ERPL_TRACE_DEBUG("ODATA_READ_BIND", "Updated URL: " + updated_url.ToString());

  // Nothing to apply: keep the client we already have. Replacing it with an
  // identical one throws away its current_response, and the buffered first page
  // below is only discarded when the URL changed -- so the scan would keep page
  // one and then ask a client that has never issued a request to advance to page
  // two, which it cannot. Server-driven paging stopped dead after the first page
  // and the rest of the entity set was dropped silently. This is the ordinary
  // `SELECT *` case: with every column projected and no filter there is no
  // $select and no $filter to add, so the URL is necessarily unchanged.
  // See GitHub #149.
  if (prev_url_str == updated_url.ToString()) {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Predicate pushdown left the URL unchanged; keeping the "
                     "existing client so server-driven paging can continue");
    return;
  }

    // Store the current OData version before creating new client
    auto current_version = odata_client->GetODataVersion();
  const auto current_max_page_size = odata_client->GetMaxPageSize();
  // Datasphere's dual-URL state. CloneForScan carries these, and this rebuild used to
  // throw them away one statement later on every projecting or filtered query - which is
  // why a SELECT * test could not reproduce the loss: SELECT * takes the early return
  // above and never reaches here. business_central_catalog.cpp pre-warms caches
  // specifically to work around this same loss (GitHub #186).
  const auto current_metadata_context = odata_client->StoredMetadataContextUrl();
  const auto current_entity_set_name = odata_client->GetEntitySetName();
  const auto current_input_parameters = odata_client->GetInputParameters();

  // Same as CloneForScan: the origin is carried, never re-derived from the cursor
  // (GitHub #189).
  const auto service_origin = odata_client->ServiceOriginUrl();
  odata_client = std::make_shared<ODataEntitySetClient>(
      http_client, updated_url, service_origin, auth_params);

  if (current_max_page_size.has_value()) {
    odata_client->SetMaxPageSize(current_max_page_size.value());
  }
  if (!current_metadata_context.empty()) {
    odata_client->SetMetadataContextUrl(current_metadata_context);
  }
  if (!current_entity_set_name.empty()) {
    odata_client->SetEntitySetName(current_entity_set_name);
  }
  if (!current_input_parameters.empty()) {
    odata_client->SetInputParameters(current_input_parameters);
  }
    
    // Preserve the OData version to avoid metadata fetching
    if (current_version != ODataVersion::UNKNOWN) {
        odata_client->SetODataVersionDirectly(current_version);
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     duckdb::StringUtil::Format(
                         "Preserved OData version %s on new client",
                         current_version == ODataVersion::V2 ? "V2" : "V4"));
    }

  // If the finalized URL changed compared to the prefetched one, discard
  // buffered data so we don't emit unfiltered/unprojected rows. The scan init
  // will prefetch again.
    if (first_page_cached_) {
    ERPL_TRACE_INFO("ODATA_READ_BIND",
                    "Final URL changed after predicate pushdown; discarding "
                    "prefetched buffer and caches");
        if (row_buffer) {
            row_buffer->Clear();
            row_buffer->SetHasNextPage(false);
        }
        if (progress_tracker) {
            progress_tracker->Reset();
        }
        if (data_extractor) {
            data_extractor->ClearCache();
        }
        emitted_row_index_ = 0;
        first_page_cached_ = false;
        // The stored probe page belongs to the OLD url. Keeping it here is how a scan ends
        // up adopting a page that does not match the request it is about to make.
        first_page_response_.reset();
        first_page_expand_extracted_ = false;
    }
}

std::shared_ptr<ODataPredicatePushdownHelper>
ODataReadBindData::PredicatePushdownHelper() {
    if (predicate_pushdown_helper == nullptr) {
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     "Creating new predicate pushdown helper");
        
    // Feed the helper the full resolved schema (metadata order, plus expanded
    // columns) together with its types. The types let it decide whether
    // $select is safe rather than guessing from column names (GitHub #86),
    // and the order matches what DuckDB's column_ids index into.
    auto column_names = GetResultNames(true);
    auto column_types = GetResultTypes(true);
    ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                     duckdb::StringUtil::Format(
                         "Creating predicate pushdown helper with %d columns",
                         (int)column_names.size()));
    predicate_pushdown_helper =
        std::make_shared<ODataPredicatePushdownHelper>(column_names, column_types);
        
        // Set the OData version for proper filter syntax generation
        auto odata_version = odata_client->GetODataVersion();
        predicate_pushdown_helper->SetODataVersion(odata_version);
    ERPL_TRACE_DEBUG(
        "ODATA_READ_BIND",
        "Set OData version " +
            std::string(odata_version == ODataVersion::V2 ? "V2" : "V4") +
            " on predicate pushdown helper");

    BindPredicateColumnResolver();
    }

    return predicate_pushdown_helper;
}

void ODataReadBindData::BindPredicateColumnResolver() {
  if (!predicate_pushdown_helper) {
    return;
  }

  // DuckDB passes the OUTPUT position (index within the projected column list
  // from InitGlobal). Translate that through activated_to_original_mapping to
  // the full-schema index before looking up the column name, because the
  // activated column order may differ from the schema order (e.g.
  // column_ids=[8,0] puts Country at output pos 0).
  predicate_pushdown_helper->SetColumnNameResolver(
      [this](duckdb::column_t output_index) -> std::string {
        size_t schema_index = output_index;
        if (output_index < this->activated_to_original_mapping.size()) {
          schema_index = this->activated_to_original_mapping[output_index];
        }

        this->EnsureBaseSchemaResolved();
        if (schema_index < this->base_result_names.size()) {
          return this->base_result_names[schema_index];
        }
        ERPL_TRACE_ERROR(
            "ODATA_READ_BIND",
            duckdb::StringUtil::Format(
                "Schema column index %d (from output index %d) is out of "
                "bounds for result names",
                (int)schema_index, (int)output_index));
        return std::string();
      });
}

std::string ODataReadBindData::GetOriginalColumnName(
    duckdb::column_t activated_column_index) const {
    if (activated_column_index >= activated_to_original_mapping.size()) {
    // This is not an error - it's a column that wasn't activated (not selected
    // by user) Just return empty string for columns we don't have mapping for
        return "";
    }

    auto original_index = activated_to_original_mapping[activated_column_index];

  // base_result_names is the reconciled schema: EDMX order first, JSON-only
  // columns appended. That is exactly the order DuckDB's column indices refer
  // to, so no other list may be consulted here (GitHub #88).
  if (original_index >= base_result_names.size()) {
    ERPL_TRACE_ERROR(
        "ODATA_READ_BIND",
        duckdb::StringUtil::Format(
            "Original column index %d is out of bounds for result names (%d total)",
            original_index, (int)base_result_names.size()));
    return "";
  }

  const auto column_name = base_result_names[original_index];
  ERPL_TRACE_DEBUG("ODATA_READ_BIND",
                   duckdb::StringUtil::Format(
                       "Activated index %d maps to original index %d which "
                       "is column '%s'",
                       (int)activated_column_index, (int)original_index,
                       column_name.c_str()));
  return column_name;
}

} // namespace erpl_web
