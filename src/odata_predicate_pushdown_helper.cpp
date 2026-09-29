#include "odata_predicate_pushdown_helper.hpp"
#include "odata_url_helpers.hpp"
#include <set>
#include "duckdb/planner/filter/optional_filter.hpp"
#include "tracing.hpp"

namespace erpl_web {

namespace {

// Renders a DuckDB constant as an OData literal for the given protocol version,
// or nullopt when the type has no literal form we can emit safely.
//
// Returning nullopt means "this value has no OData spelling", not "drop the filter".
// DuckDB does NOT keep a pushed filter as a residual above the scan - once a predicate
// becomes a TableFilter and the function advertises filter_pushdown, the optimizer removes
// it from the plan (optimizer/pushdown/pushdown_get.cpp). So every caller must either fail
// the query or be advisory by construction (optional, bloom, dynamic filters); dropping
// the comparison returns rows that do not satisfy the WHERE clause. See GitHub #153.
// Emitting a wrongly-typed literal is not safe either, because a lenient server silently
// returns a different row set.
std::optional<std::string> FormatODataLiteral(const duckdb::Value &value, ODataVersion version)
{
    if (value.IsNull()) {
        return std::nullopt;
    }

    switch (value.type().id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
        return value.GetValue<bool>() ? std::string("true") : std::string("false");

    // Numeric literals are emitted bare in both versions. V2 strictly wants type
    // suffixes (L for Int64, M for Decimal, f for Single); SAP Gateway accepts
    // the bare form, so they are deliberately not emitted until validated
    // against a real Gateway system. See GitHub #64.
    case duckdb::LogicalTypeId::TINYINT:
    case duckdb::LogicalTypeId::SMALLINT:
    case duckdb::LogicalTypeId::INTEGER:
    case duckdb::LogicalTypeId::BIGINT:
    case duckdb::LogicalTypeId::HUGEINT:
    case duckdb::LogicalTypeId::UTINYINT:
    case duckdb::LogicalTypeId::USMALLINT:
    case duckdb::LogicalTypeId::UINTEGER:
    case duckdb::LogicalTypeId::UBIGINT:
    case duckdb::LogicalTypeId::UHUGEINT:
    case duckdb::LogicalTypeId::FLOAT:
    case duckdb::LogicalTypeId::DOUBLE:
    case duckdb::LogicalTypeId::DECIMAL:
        return value.ToString();

    case duckdb::LogicalTypeId::VARCHAR:
    case duckdb::LogicalTypeId::CHAR:
        return "'" + EscapeODataStringLiteral(value.ToString()) + "'";

    case duckdb::LogicalTypeId::DATE: {
        const auto rendered = value.ToString();
        if (version == ODataVersion::V2) {
            return "datetime'" + rendered + "T00:00:00'";
        }
        return rendered;
    }

    case duckdb::LogicalTypeId::TIMESTAMP: {
        // DuckDB renders "2020-01-02 03:04:05"; OData requires the ISO-8601 "T".
        auto rendered = value.ToString();
        const auto space_pos = rendered.find(' ');
        if (space_pos != std::string::npos) {
            rendered[space_pos] = 'T';
        }
        if (version == ODataVersion::V2) {
            return "datetime'" + rendered + "'";
        }
        return rendered + "Z";
    }

    case duckdb::LogicalTypeId::TIME: {
        if (version == ODataVersion::V2) {
            // V2 spells time as the duration form time'PT3H4M5S'; not emitted
            // until it can be validated against a real service.
            return std::nullopt;
        }
        return value.ToString();
    }

    case duckdb::LogicalTypeId::UUID: {
        const auto rendered = value.ToString();
        if (version == ODataVersion::V2) {
            return "guid'" + rendered + "'";
        }
        return rendered;
    }

    default:
        // TIMESTAMP_TZ, BLOB, INTERVAL, nested types: no literal form is emitted
        // rather than risking a wrongly-typed one. The filter stays local.
        return std::nullopt;
    }
}

} // namespace

// Forward declarations for local sanitization helpers
static void SanitizeFilterParam(std::map<std::string, std::string>& params);
static void SanitizeExpandParam(std::map<std::string, std::string>& params);
static void EnsureJsonFormat(std::map<std::string, std::string>& params);

ODataPredicatePushdownHelper::ODataPredicatePushdownHelper(const std::vector<std::string> &all_column_names)
    : ODataPredicatePushdownHelper(all_column_names, std::vector<duckdb::LogicalType>())
{ }

ODataPredicatePushdownHelper::ODataPredicatePushdownHelper(const std::vector<std::string> &all_column_names,
                                                           const std::vector<duckdb::LogicalType> &all_column_types)
    : all_column_names(all_column_names)
    , all_column_types(all_column_types)
{
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Created predicate pushdown helper with " + std::to_string(all_column_names.size()) + " columns");
    
    // Log all available column names for debugging
    for (size_t i = 0; i < all_column_names.size(); ++i) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Column " + std::to_string(i) + ": " + all_column_names[i]);
    }
}

void ODataPredicatePushdownHelper::SetColumnSchema(const std::vector<std::string> &names,
                                                   const std::vector<duckdb::LogicalType> &types)
{
    all_column_names = names;
    all_column_types = types;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                     "Column schema refreshed: " + std::to_string(all_column_names.size()) + " names, " +
                         std::to_string(all_column_types.size()) + " types");
}

bool ODataPredicatePushdownHelper::IsNestedColumnType(duckdb::column_t schema_index) const
{
    if (schema_index >= all_column_types.size()) {
        return false;
    }

    switch (all_column_types[schema_index].id()) {
    case duckdb::LogicalTypeId::LIST:
    case duckdb::LogicalTypeId::STRUCT:
    case duckdb::LogicalTypeId::MAP:
    case duckdb::LogicalTypeId::ARRAY:
    case duckdb::LogicalTypeId::UNION:
        return true;
    default:
        return false;
    }
}

void ODataPredicatePushdownHelper::SetColumnNameResolver(ColumnNameResolver resolver)
{
    column_name_resolver = resolver;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Column name resolver set");
}

void ODataPredicatePushdownHelper::ConsumeColumnSelection(const std::vector<duckdb::column_t> &column_ids) {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Consuming column selection: " + std::to_string(column_ids.size()) + " columns");
    this->select_clause = BuildSelectClause(column_ids);
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built select clause: " + this->select_clause);
}

void ODataPredicatePushdownHelper::ConsumeFilters(duckdb::optional_ptr<duckdb::TableFilterSet> filters) {
    // Recomputed from scratch for this filter set. Leaving it set would make $top/$skip
    // suppression sticky: one untranslatable filter would keep suppressing them for every
    // later use of the same helper, even after the filters were replaced.
    has_untranslated_filter = false;

    if (filters && !filters->filters.empty()) {
        std::stringstream filters_str;
        /*
        for (auto &[projected_column_idx, filter] : filters->filters) 
        {
            filters_str << "Column " << projected_column_idx << ": " << filter->DebugToString() << std::endl;
        }
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Consuming " + std::to_string(filters->filters.size()) + " filters: " + filters_str.str());
        */
        this->filter_clause = BuildFilterClause(filters);
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built filter clause: " + this->filter_clause);
    } else {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No filters to consume");
        this->filter_clause = "";
    }
}

void ODataPredicatePushdownHelper::ConsumeLimit(duckdb::idx_t limit) {
    if (limit > 0) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Consuming LIMIT: " + std::to_string(limit));
        this->top_clause = BuildTopClause(limit);
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built top clause: " + this->top_clause);
    } else {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No LIMIT to consume");
        this->top_clause = "";
    }
}

void ODataPredicatePushdownHelper::ConsumeOffset(duckdb::idx_t offset) {
    if (offset > 0) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Consuming OFFSET: " + std::to_string(offset));
        this->skip_clause = BuildSkipClause(offset);
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built skip clause: " + this->skip_clause);
    } else {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No OFFSET to consume");
        this->skip_clause = "";
    }
}

void ODataPredicatePushdownHelper::ConsumeExpand(const std::string& expand_clause) {
    if (!expand_clause.empty()) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Consuming expand clause: " + expand_clause);
        // Structure only -- the nested option keys get their '$', and nothing is encoded.
        // Encoding happens once, in EncodeQueryValueForOption, like every other value: a
        // clause sanitized here as well would be sanitized twice and reach the wire as
        // %2520, which is the same double-encoding #227 is about.
        auto normalized = ODataUrlCodec::normalizeExpand(expand_clause);
        if (normalized != expand_clause) {
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Normalized expand clause: " + normalized);
        }
        this->expand_clause = "$expand=" + normalized;
    } else {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No expand clause to consume");
        this->expand_clause = "";
    }
}

void ODataPredicatePushdownHelper::SetODataVersion(ODataVersion version) {
    odata_version = version;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Set OData version to: " + std::string(version == ODataVersion::V2 ? "V2" : "V4"));
}

ODataVersion ODataPredicatePushdownHelper::GetODataVersion() const {
    return odata_version;
}

void ODataPredicatePushdownHelper::EnableInlineCount(bool enable) {
    inline_count_enabled = enable;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Inline count " + std::string(enable ? "enabled" : "disabled"));
}

void ODataPredicatePushdownHelper::SetSkipToken(const std::string& token) {
    skip_token = token;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Set skip token to: " + token);
}



HttpUrl ODataPredicatePushdownHelper::ApplyFiltersToUrl(const HttpUrl &base_url) {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Applying filters to URL: " + base_url.ToString());
    
    HttpUrl result = base_url;
    std::string existing_query = result.Query();
    
    LogCurrentClauses();
    
    // Parse existing query parameters to avoid duplicates
    auto existing_params = ParseExistingQueryParameters(existing_query);
    
    // Sanitize existing params from URL first (normalize and encode once)
    SanitizeFilterParam(existing_params);
    SanitizeExpandParam(existing_params);
    EnsureJsonFormat(existing_params);

    // Apply version-specific logic
    if (odata_version == ODataVersion::V2) {
        ApplyV2SpecificLogic(existing_params);
    }
    
    // Merge all parameters and rebuild query
    MergeParametersIntoQuery(existing_params);
    
    // Rebuild final query from existing_params
    std::string new_query = RebuildQueryString(existing_params);
    
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Final query: '" + new_query + "'");
    result.Query(new_query);
    
    ERPL_TRACE_INFO("PREDICATE_PUSHDOWN", "Updated URL: " + result.ToString());
    return result;
}

void ODataPredicatePushdownHelper::LogCurrentClauses() const {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Select clause: '" + select_clause + "'");
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Filter clause: '" + filter_clause + "'");
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Top clause: '" + top_clause + "'");
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Skip clause: '" + skip_clause + "'");
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Expand clause: '" + expand_clause + "'");
}

std::map<std::string, std::string> ODataPredicatePushdownHelper::ParseExistingQueryParameters(const std::string& existing_query) const {
    std::map<std::string, std::string> existing_params;
    
    if (!existing_query.empty() && existing_query != "?") {
        std::string query_str = existing_query;
        if (query_str[0] == '?') {
            query_str = query_str.substr(1);
        }
        
        std::istringstream iss(query_str);
        std::string param;
        while (std::getline(iss, param, '&')) {
            size_t pos = param.find('=');
            if (pos != std::string::npos) {
                std::string raw_key = param.substr(0, pos);
                std::string raw_value = param.substr(pos + 1);
                // Decode key/value; canonicalize key (e.g., %24filter -> $filter)
                std::string decoded_key = ODataUrlCodec::decodeQueryValue(raw_key);
                if (!decoded_key.empty() && decoded_key[0] != '$' && decoded_key.rfind("%24", 0) == 0) {
                    decoded_key = std::string("$") + decoded_key.substr(3);
                }
                if (decoded_key.rfind("%24", 0) == 0) {
                    decoded_key = std::string("$") + decoded_key.substr(3);
                }
                // Normalize known aliases to canonical keys
                if (decoded_key == "filter") decoded_key = "$filter";
                if (decoded_key == "expand") decoded_key = "$expand";
                if (decoded_key == "select") decoded_key = "$select";
                if (decoded_key == "top") decoded_key = "$top";
                if (decoded_key == "skip") decoded_key = "$skip";
                if (decoded_key == "format") decoded_key = "$format";
                if (decoded_key == "count") decoded_key = "$count";

                std::string decoded_value = ODataUrlCodec::decodeQueryValue(raw_value);
                existing_params[decoded_key] = decoded_value;
            }
        }
    }
    
    return existing_params;
}

// Sanitize existing URL params: encode $filter exactly once, normalize $expand

static void SanitizeFilterParam(std::map<std::string, std::string>& params) {
    auto it = params.find("$filter");
    if (it == params.end()) return;
    const std::string &decoded = it->second; // already decoded by parser
    if (decoded.empty()) return;
    // Encode once using OData encoder
    std::string encoded = ODataUrlCodec::encodeFilterExpression(decoded);
    it->second = encoded;
}

static void SanitizeExpandParam(std::map<std::string, std::string>& params) {
    auto it = params.find("$expand");
    if (it == params.end()) return;
    const std::string &decoded = it->second; // already decoded
    if (decoded.empty()) return;
    std::string normalized = ODataUrlCodec::normalizeAndSanitizeExpand(decoded);
    it->second = normalized;
}

static void EnsureJsonFormat(std::map<std::string, std::string>& params) {
    auto it = params.find("$format");
    if (it == params.end()) {
        params["$format"] = "json";
    }
}

void ODataPredicatePushdownHelper::ApplyV2SpecificLogic(std::map<std::string, std::string>& existing_params) {
    // For OData V2: if a $select is present and we have an $expand, we must also include
    // the expanded navigation properties in $select or many services omit them from the payload.
    // This differs from V4 where $expand is sufficient.
    if (select_clause.empty()) {
        return;
    }
    
    std::string expand_list = GetEffectiveExpandList(existing_params);
    if (expand_list.empty()) {
        return;
    }
    
    std::string select_fields = ExtractSelectFields();
    std::set<std::string> selected = ParseSelectedFields(select_fields);
    std::vector<std::string> missing_nav_props = FindMissingNavigationProperties(expand_list, selected);
    
    if (!missing_nav_props.empty()) {
        AugmentSelectClauseWithNavigationProperties(select_fields, missing_nav_props);
    }
}

std::string ODataPredicatePushdownHelper::GetEffectiveExpandList(const std::map<std::string, std::string>& existing_params) const {
    std::string expand_list;
    
    if (!expand_clause.empty()) {
        // expand_clause is of the form "$expand=..."
        auto pos = expand_clause.find('=');
        if (pos != std::string::npos && pos + 1 < expand_clause.size()) {
            expand_list = expand_clause.substr(pos + 1);
        }
    } else {
        auto it = existing_params.find("$expand");
        if (it != existing_params.end()) {
            expand_list = it->second;
        }
    }
    
    return expand_list;
}

std::string ODataPredicatePushdownHelper::ExtractSelectFields() const {
    std::string select_fields;
    auto pos = select_clause.find('=');
    if (pos != std::string::npos && pos + 1 < select_clause.size()) {
        select_fields = select_clause.substr(pos + 1);
    }
    return select_fields;
}

std::set<std::string> ODataPredicatePushdownHelper::ParseSelectedFields(const std::string& select_fields) const {
    std::set<std::string> selected;
    
    if (!select_fields.empty()) {
        std::istringstream ss(select_fields);
        std::string item;
        while (std::getline(ss, item, ',')) {
            // trim spaces
            size_t start = item.find_first_not_of(' ');
            size_t end = item.find_last_not_of(' ');
            if (start == std::string::npos) continue;
            selected.insert(item.substr(start, end - start + 1));
        }
    }
    
    return selected;
}

std::vector<std::string> ODataPredicatePushdownHelper::FindMissingNavigationProperties(
    const std::string& expand_list, 
    const std::set<std::string>& selected) const {
    
    std::vector<std::string> to_append;
    
    std::istringstream ss(expand_list);
    std::string exp;
    while (std::getline(ss, exp, ',')) {
        // trim
        size_t start = exp.find_first_not_of(' ');
        size_t end = exp.find_last_not_of(' ');
        if (start == std::string::npos) continue;
        
        std::string nav = exp.substr(start, end - start + 1);
        // Stop at '(' (options) and '/' (nested path)
        size_t paren = nav.find('(');
        if (paren != std::string::npos) nav = nav.substr(0, paren);
        size_t slash = nav.find('/');
        if (slash != std::string::npos) nav = nav.substr(0, slash);
        
        if (!nav.empty() && selected.find(nav) == selected.end()) {
            to_append.push_back(nav);
        }
    }
    
    return to_append;
}

void ODataPredicatePushdownHelper::AugmentSelectClauseWithNavigationProperties(
    const std::string& select_fields, 
    const std::vector<std::string>& missing_nav_props) {
    
    std::ostringstream rebuilt;
    rebuilt << "$select=" << select_fields;
    
    for (auto &nav : missing_nav_props) {
        if (!select_fields.empty()) {
            rebuilt << "," << nav;
        } else {
            // In case select_fields was empty after parsing, still handle correctly
            rebuilt << nav;
        }
    }
    
    select_clause = rebuilt.str();
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Augmented V2 $select with expanded nav props: " + select_clause);
}

void ODataPredicatePushdownHelper::MergeParametersIntoQuery(std::map<std::string, std::string>& existing_params) {
    // Merge/overwrite parameters from our generated clauses into existing_params,
    // then rebuild the query string. This avoids duplicates (e.g., $top twice).
    // For $filter: only set if not already present to avoid double-encoding after redirects.
    auto upsert_param = [&](const std::string &clause, bool overwrite_always) {
        if (clause.empty()) return;
        auto pos = clause.find('=');
        if (pos == std::string::npos || pos + 1 >= clause.size()) return;
        std::string key = clause.substr(0, pos);
        std::string value = clause.substr(pos + 1);
        if (overwrite_always || existing_params.find(key) == existing_params.end()) {
            existing_params[key] = value;
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", std::string("Set param '") + key + "' = '" + value + "'");
        } else {
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", std::string("Keeping existing param '") + key + "' = '" + existing_params[key] + "'");
        }
    };

    // $select: overwrite to latest selection
    upsert_param(select_clause, true);
    // $filter: do not overwrite an existing one (avoid double-encoding after redirects)
    upsert_param(filter_clause, false);
    // $top/$skip: overwrite to latest, but only when every filter reached the server.
    // Pushing a row limit over a result the server did not filter returns too few rows.
    if (has_untranslated_filter) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                         "Not pushing $top/$skip: a filter is being applied locally, so a "
                         "server-side row limit would truncate the unfiltered result");
    } else {
        upsert_param(top_clause, true);
        upsert_param(skip_clause, true);
    }
    // $expand: only set if not already present (respect explicit URL expand)
    upsert_param(expand_clause, false);
    // v2-specific inline count and skip token: overwrite
    {
        auto inline_count = InlineCountClause();
        upsert_param(inline_count, true);
        auto skip_token = SkipTokenClause();
        upsert_param(skip_token, true);
    }
}

std::string ODataPredicatePushdownHelper::RebuildQueryString(const std::map<std::string, std::string>& existing_params) const {
    if (existing_params.empty()) {
        return "";
    }
    
    std::ostringstream oss;
    oss << "?";
    bool first = true;
    for (const auto &kv : existing_params) {
        if (!first) oss << "&";
        first = false;
        oss << kv.first << "=" << ODataUrlCodec::encodeQueryValueForOption(kv.first, kv.second);
    }
    return oss.str();
}

std::string ODataPredicatePushdownHelper::BuildSelectClause(const std::vector<duckdb::column_t> &column_ids) const {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Building select clause for " + std::to_string(column_ids.size()) + " columns");
    
    if (column_ids.empty()) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No columns selected, returning empty select clause");
        return "";
    }

    // Count non-rowid columns
    size_t non_rowid_count = 0;
    for (size_t i = 0; i < column_ids.size(); ++i) {
        if (!duckdb::IsRowIdColumnId(column_ids[i])) {
            non_rowid_count++;
        }
    }

    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Non-rowid columns: " + std::to_string(non_rowid_count) + " out of " + std::to_string(all_column_names.size()));

    // If all available columns are selected, skip the $select parameter
    // This allows the OData service to return all data without column restrictions
    if (non_rowid_count == all_column_names.size()) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "All columns selected, skipping $select parameter");
        return "";
    }

    // Skip $select entirely when any requested column is a nested (collection or
    // complex) property. Many services reject such properties inside $select, and
    // OData v2 in particular has no portable syntax for projecting into them.
    //
    // This used to be a hard-coded list of TripPin demo property names
    // ("Emails", "AddressInfo", "HomeAddress", "Features") matched by PREFIX,
    // which silently disabled projection pushdown for any real service with a
    // scalar column merely starting with one of those words (GitHub #86).
    // The DuckDB LogicalType is the accurate signal the name match approximated.
    for (size_t i = 0; i < column_ids.size(); ++i) {
        if (duckdb::IsRowIdColumnId(column_ids[i])) {
            continue;
        }

        if (IsNestedColumnType(column_ids[i])) {
            const std::string field_name = (column_ids[i] < all_column_names.size())
                                               ? all_column_names[column_ids[i]]
                                               : std::string("<unknown>");
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                             "Nested column type detected: " + field_name + " (" +
                                 all_column_types[column_ids[i]].ToString() +
                                 "), skipping $select to avoid OData errors");
            return "";
        }
    }

    std::stringstream select_clause;
    std::set<std::string> unique_fields; // Use set to avoid duplicates

    size_t output_pos = 0;
    for (size_t i = 0; i < column_ids.size(); ++i) {
        if (duckdb::IsRowIdColumnId(column_ids[i])) {
            continue;
        }

        // Extract base field name from complex expressions.
        // Pass output_pos (not schema index) so the resolver maps correctly.
        std::string field_name;
        if (column_name_resolver) {
            field_name = column_name_resolver(output_pos);
            if (field_name.empty()) {
                ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN", "Column name resolver returned empty string for output pos " + std::to_string(output_pos));
                output_pos++;
                continue;
            }
        } else {
            field_name = all_column_names[column_ids[i]];
        }
        
        // For OData $select, we need to use the base field name (before any path expressions)
        // Complex expressions like AddressInfo[1].City."Name" should use AddressInfo in $select
        // but the full expression will be evaluated by the JSON path evaluator
        size_t pos = field_name.find_first_of(".[\"");
        if (pos != std::string::npos) {
            field_name = field_name.substr(0, pos);
        }
        
        // Only add if not already present
        if (unique_fields.find(field_name) == unique_fields.end()) {
            if (!unique_fields.empty()) {
                select_clause << ",";
            }
            select_clause << field_name;
            unique_fields.insert(field_name);
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Added field to select: " + field_name);
        }
        output_pos++;
    }

    if (select_clause.str().empty()) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No valid fields for select, returning empty clause");
        return "";
    }

    std::string result = "$select=" + select_clause.str();
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built select clause: " + result);
    return result;
}

std::string ODataPredicatePushdownHelper::BuildFilterClause(duckdb::optional_ptr<duckdb::TableFilterSet> filters) {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Building filter clause");
    
    if (!filters || filters->filters.empty()) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No filters provided, returning empty filter clause");
        return "";
    }

    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Processing " + std::to_string(filters->filters.size()) + " filters");
    
    std::stringstream filter_clause;
    std::vector<std::string> valid_filters;
    
    // First pass: collect all valid filters
    for (const auto &filter_entry : filters->filters) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", duckdb::StringUtil::Format("Processing filter for DuckDB column index: %d", filter_entry.first));
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", duckdb::StringUtil::Format("Total columns available: %d", all_column_names.size()));
        
        // Use column name resolver if available, otherwise fall back to direct indexing
        std::string column_name;
        if (column_name_resolver) {
            column_name = column_name_resolver(filter_entry.first);
            if (column_name.empty()) {
                // Skipping loses the predicate entirely: DuckDB pushed it here and removed
                // it from the plan, so nothing re-applies it and the scan returns rows that
                // do not satisfy the WHERE clause. See GitHub #153.
                ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN", "Column name resolver returned empty string for index " + std::to_string(filter_entry.first));
                // NotImplementedException, not InternalException: DuckDB treats
                // ExceptionType::INTERNAL as fatal and calls ValidChecker::Invalidate on the
                // whole database instance, so a single query with an unresolvable filter
                // column would take down every later statement on that connection. Failing
                // the statement is right; failing the instance is not.
                throw duckdb::NotImplementedException(
                    "OData pushdown could not resolve a column name for filter index " +
                    std::to_string(filter_entry.first) +
                    "; refusing to drop the filter, which would return rows that do not match "
                    "the query");
            }
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Column name resolver mapped index " + std::to_string(filter_entry.first) + " to: " + column_name);
        } else {
            // Fallback to direct indexing (for backward compatibility)
            duckdb::idx_t column_index = filter_entry.first;
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No column name resolver, using direct index: " + std::to_string(column_index));
            
            if (column_index >= all_column_names.size()) {
                // As above: dropping the filter silently widens the result.
                ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN", "Column index " + std::to_string(column_index) + " is out of bounds for column names array");
                // See the note above on why this is not an InternalException.
                throw duckdb::NotImplementedException(
                    "OData pushdown got a filter for column index " + std::to_string(column_index) +
                    ", but the scan has only " + std::to_string(all_column_names.size()) +
                    " columns; refusing to drop the filter, which would return rows that do not "
                    "match the query");
            }
            
            column_name = all_column_names[column_index];
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Direct mapping: column index " + std::to_string(column_index) + " maps to column name: " + column_name);
        }
        
        std::string translated_filter = TranslateFilter(*filter_entry.second, column_name);
        if (!translated_filter.empty()) {
            valid_filters.push_back(translated_filter);
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Valid filter: " + translated_filter);
        } else {
            // The filter stays with DuckDB. Record that, so $top/$skip are not pushed
            // on top of a result the server has not filtered. See the member comment.
            has_untranslated_filter = true;
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Filter translation resulted in empty string for column: " + column_name);
        }
    }
    
    // If no valid filters, return empty string
    if (valid_filters.empty()) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "No valid filters found, returning empty filter clause");
        return "";
    }
    
    // Build the filter clause with valid filters
    filter_clause << "$filter=";
    for (size_t i = 0; i < valid_filters.size(); ++i) {
        if (i > 0) {
            filter_clause << " and ";
        }
        filter_clause << valid_filters[i];
    }
    
    std::string result = filter_clause.str();

    // Deliberately NOT encoded here. Every value in the parameter map is decoded
    // plaintext, and RebuildQueryString encodes each one exactly once on the way out.
    // Encoding in both places is what produced %2520 after a redirect, and the
    // workarounds for that ("keep the existing $filter") then dropped pushed-down
    // filters instead. See GitHub #227.
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built filter clause: " + result);
    return result;
}

std::string ODataPredicatePushdownHelper::BuildTopClause(duckdb::idx_t limit) const {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Building top clause for limit: " + std::to_string(limit));
    
    if (limit <= 0) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Invalid limit, returning empty top clause");
        return "";
    }
    
    std::string result = "$top=" + std::to_string(limit);
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built top clause: " + result);
    return result;
}

std::string ODataPredicatePushdownHelper::BuildSkipClause(duckdb::idx_t offset) const {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Building skip clause for offset: " + std::to_string(offset));
    
    if (offset <= 0) {
        ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Invalid offset, returning empty skip clause");
        return "";
    }
    
    std::string result = "$skip=" + std::to_string(offset);
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Built skip clause: " + result);
    return result;
}

std::string ODataPredicatePushdownHelper::InlineCountClause() const {
    if (!inline_count_enabled) {
        return "";
    }
    
    if (odata_version == ODataVersion::V2) {
        // OData v2: $inlinecount=allpages
        return "$inlinecount=allpages";
    } else {
        // OData v4: $count=true
        return "$count=true";
    }
}

std::string ODataPredicatePushdownHelper::SkipTokenClause() const {
    if (!skip_token.has_value()) {
        return "";
    }
    
    if (odata_version == ODataVersion::V2) {
        // OData v2: $skiptoken=value
        return "$skiptoken=" + skip_token.value();
    } else {
        // OData v4: $skip=value (or use @odata.nextLink)
        return "$skip=" + skip_token.value();
    }
}

namespace {

// DuckDB 1.5 appended TableFilterType::BLOOM_FILTER (= 10). The 1.4 LTS line, which this
// extension still builds against, stops at EXPRESSION_FILTER (= 9) and has no such
// enumerator, so naming it directly fails to compile there. The numeric value is stable
// and unused on 1.4, which makes this case label simply unreachable on the LTS build.
constexpr auto BLOOM_FILTER_TABLE_FILTER_TYPE = static_cast<duckdb::TableFilterType>(10);

// Where the 1.5 header exists, hold the numeric value to the real enumerator: a future
// renumbering would otherwise silently point this case label at a different filter kind.
#if __has_include("duckdb/planner/filter/bloom_filter.hpp")
static_assert(BLOOM_FILTER_TABLE_FILTER_TYPE == duckdb::TableFilterType::BLOOM_FILTER,
              "TableFilterType::BLOOM_FILTER is no longer 10; update the LTS fallback value");
#endif

}  // namespace

std::string ODataPredicatePushdownHelper::TranslateFilter(const duckdb::TableFilter &filter, const std::string &column_name) const {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Translating filter for column '" + column_name + "' with filter type: " + std::to_string(static_cast<int>(filter.filter_type)));
    
    std::string result;
    switch (filter.filter_type) {
        case duckdb::TableFilterType::CONSTANT_COMPARISON:
            result = TranslateConstantComparison(filter.Cast<duckdb::ConstantFilter>(), column_name);
            break;
        case duckdb::TableFilterType::IS_NULL:
            result = column_name + " eq null";
            break;
        case duckdb::TableFilterType::IS_NOT_NULL:
            result = column_name + " ne null";
            break;
        case duckdb::TableFilterType::CONJUNCTION_AND:
            result = TranslateConjunction(filter.Cast<duckdb::ConjunctionAndFilter>(), column_name);
            break;
        case duckdb::TableFilterType::CONJUNCTION_OR:
            result = TranslateConjunction(filter.Cast<duckdb::ConjunctionOrFilter>(), column_name);
            break;
        case duckdb::TableFilterType::OPTIONAL_FILTER:
            // DuckDB documents this kind as "executing filter is not required for query
            // correctness" (duckdb/planner/table_filter.hpp), so it is always safe to skip
            // and must never fail the query. A hash join pushes an optional filter wrapping
            // a bloom filter into this slot; translating the child eagerly would reach the
            // default: arm below and throw on an ordinary join.
            try {
                result = TranslateFilter(*filter.Cast<duckdb::OptionalFilter>().child_filter, column_name);
            } catch (const duckdb::NotImplementedException &) {
                ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                                 "Optional filter on column '" + column_name +
                                 "' has no OData translation; skipping it (it is not required for correctness)");
                result = "";
            }
            break;
        case BLOOM_FILTER_TABLE_FILTER_TYPE:
            // A probabilistic join pre-filter: dropping it costs only the rows it would
            // have skipped early, never correctness.
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                             "Bloom filter on column '" + column_name + "' is not pushed down");
            result = "";
            break;
        case duckdb::TableFilterType::DYNAMIC_FILTER: {
            // A dynamic filter is filled in by the Top-N optimizer only once the
            // first rows have been seen. Until then filter_data->filter holds a
            // placeholder sentinel (INT32_MIN and friends) that must never reach
            // the server: pushing it returns zero rows, and because the filter
            // was pushed there is no residual filter left to restore them.
            // See GitHub #59.
            auto &filter_data = *filter.Cast<duckdb::DynamicFilter>().filter_data;
            duckdb::lock_guard<duckdb::mutex> filter_guard(filter_data.lock);
            if (!filter_data.initialized || !filter_data.filter) {
                ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                                 "Dynamic filter for column '" + column_name +
                                 "' is not initialized yet; not pushing it down");
                result = "";
                break;
            }
            try {
                result = TranslateConstantComparison(*filter_data.filter, column_name);
            } catch (const duckdb::NotImplementedException &) {
                // A dynamic filter is advisory by construction: the Top-N operator that
                // produced it enforces the bound again above the scan. Failing here would
                // abort a query that works today - ORDER BY <timestamptz col> LIMIT n - for
                // a filter we are never required to push.
                ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                                 "Dynamic filter on column '" + column_name +
                                 "' has no OData translation; skipping it (it is not required "
                                 "for correctness)");
                result = "";
            }
            break;
        }
        case duckdb::TableFilterType::IN_FILTER:
            result = TranslateInFilter(filter.Cast<duckdb::InFilter>(), column_name);
            break;
        default:
            std::stringstream error;
            auto filter_str = const_cast<duckdb::TableFilter&>(filter).ToString(column_name);
            auto filter_type_name = typeid(filter).name();
            error << "Unsupported filter type for OData translation: '" << filter_str << "'"
                  << " (" << filter_type_name << ")";
            ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN", "Unsupported filter type: " + std::string(filter_type_name));
            // Deliberately loud: DuckDB prunes some pushed filters from the
            // residual plan, so silently dropping one can lose rows. Failing the
            // query is preferable to returning a wrong answer.
            throw duckdb::NotImplementedException(error.str());
    }
    
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Translated filter result: '" + result + "'");
    return result;
}

std::string ODataPredicatePushdownHelper::TranslateConstantComparison(const duckdb::ConstantFilter &filter, const std::string &column_name) const {
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Translating constant comparison for column '" + column_name + "'");
    
    // Validate the filter value to ensure it makes sense for OData
    std::string constant_value = filter.constant.ToString();
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Constant value: '" + constant_value + "' (type: " + filter.constant.type().ToString() + ")");
    
    // Nothing is skipped here on the grounds of the value alone. DuckDB removes a
    // predicate from the plan once it becomes a TableFilter and the function advertises
    // filter_pushdown, so no residual filter re-applies what we drop: skipping means
    // returning rows that do not satisfy the query's WHERE clause. An empty string is a
    // perfectly good OData literal ("Col eq ''", which SAP Gateway answers correctly),
    // and an over-long literal is the service's business - a rejection from the server is
    // loud, unlike a silently wrong answer. See GitHub #153.
    
    std::stringstream result;
    result << column_name << " ";

    std::string comparison_operator;
    switch (filter.comparison_type) {
        case duckdb::ExpressionType::COMPARE_EQUAL:
            comparison_operator = "eq";
            break;
        case duckdb::ExpressionType::COMPARE_NOTEQUAL:
            comparison_operator = "ne";
            break;
        case duckdb::ExpressionType::COMPARE_LESSTHAN:
            comparison_operator = "lt";
            break;
        case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
            comparison_operator = "le";
            break;
        case duckdb::ExpressionType::COMPARE_GREATERTHAN:
            comparison_operator = "gt";
            break;
        case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            comparison_operator = "ge";
            break;
        default:
            // Skipping would silently widen the result set (see above), so fail instead.
            ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN", "Unsupported comparison type for column: " + column_name);
            throw duckdb::NotImplementedException(
                "OData pushdown cannot express the comparison used on column '" + column_name +
                "'. Please open an issue at https://github.com/DataZooDE/erpl-odata/issues");
    }
    
    result << comparison_operator;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Using comparison operator: " + comparison_operator);

    // Render the constant as a version-appropriate OData literal. A type with no safe
    // literal form cannot be pushed and cannot be left to DuckDB either, because the
    // predicate is no longer in the plan - so the query fails rather than answering with
    // rows that do not match it.
    const auto literal = FormatODataLiteral(filter.constant, odata_version);
    if (!literal.has_value()) {
        ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN",
                         "No OData literal form for type " + filter.constant.type().ToString() +
                         " on column " + column_name);
        throw duckdb::NotImplementedException(
            "OData pushdown has no literal form for type " + filter.constant.type().ToString() +
            " used in a comparison on column '" + column_name +
            "'. Cast the value to a type the service understands, or filter after reading.");
    }
    result << " " << *literal;
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Added literal: " + *literal);

    std::string final_result = result.str();
    ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN", "Final constant comparison: '" + final_result + "'");
    return final_result;
}

std::string ODataPredicatePushdownHelper::TranslateInFilter(const duckdb::InFilter &filter, const std::string &column_name) const {
    // OData has no IN operator, so an IN list becomes a parenthesised or-chain.
    // Every value must translate: dropping one would narrow the disjunction and
    // silently lose rows, and DuckDB prunes a pushed IN filter from the residual
    // plan, so nothing downstream would bring them back.
    if (filter.values.empty()) {
        return "";
    }

    // There is no local fallback to fall back TO: DuckDB removed this filter from the plan
    // when it pushed it here, so returning "" hands back the whole entity set. The old
    // 100-value ceiling did exactly that. A long URL is the service's business - a 414 is
    // loud, where a dropped filter is silently wrong. See GitHub #153.

    std::stringstream result;
    result << "(";
    for (idx_t i = 0; i < filter.values.size(); ++i) {
        const auto literal = FormatODataLiteral(filter.values[i], odata_version);
        if (!literal.has_value()) {
            ERPL_TRACE_ERROR("PREDICATE_PUSHDOWN",
                             "IN list on column '" + column_name +
                             "' holds a value with no OData literal form");
            throw duckdb::NotImplementedException(
                "OData pushdown has no literal form for type " + filter.values[i].type().ToString() +
                " used in an IN list on column '" + column_name +
                "'. Materialise the read first, for example: CREATE TEMP TABLE t AS SELECT * FROM "
                "odata_read(...); SELECT * FROM t WHERE ...");
        }
        if (i > 0) {
            result << " or ";
        }
        result << column_name << " eq " << *literal;
    }
    result << ")";

    return result.str();
}

std::string ODataPredicatePushdownHelper::TranslateConjunction(const duckdb::ConjunctionAndFilter &filter, const std::string &column_name) const {
    // A child that cannot be translated throws, and the exception propagates: dropping
    // it would widen the result set with nothing downstream to narrow it again. An empty
    // translation now means only "advisory filter" (optional, bloom, or an uninitialised
    // dynamic filter), which is safe to leave out. See GitHub #153.
    std::vector<std::string> translated;
    translated.reserve(filter.child_filters.size());
    for (const auto &child : filter.child_filters) {
        auto child_filter = TranslateFilter(*child, column_name);
        if (!child_filter.empty()) {
            translated.push_back(std::move(child_filter));
        }
    }

    if (translated.empty()) {
        return "";
    }

    std::stringstream result;
    result << "(";
    for (idx_t i = 0; i < translated.size(); ++i) {
        if (i > 0) {
            result << " and ";
        }
        result << translated[i];
    }
    result << ")";
    return result.str();
}

std::string ODataPredicatePushdownHelper::TranslateConjunction(const duckdb::ConjunctionOrFilter &filter, const std::string &column_name) const {
    // An OR is all-or-nothing. A branch that cannot be translated throws from the child
    // translation, like everywhere else (GitHub #153). A branch that translates to "" is
    // merely advisory and carries no predicate of its own, so pushing a partial form would
    // wrongly NARROW the disjunction; that case abandons the whole thing, which is safe
    // because whatever produced the advisory filter re-applies it above the scan.
    std::vector<std::string> translated;
    translated.reserve(filter.child_filters.size());
    for (const auto &child : filter.child_filters) {
        auto child_filter = TranslateFilter(*child, column_name);
        if (child_filter.empty()) {
            ERPL_TRACE_DEBUG("PREDICATE_PUSHDOWN",
                             "Disjunction on column '" + column_name +
                             "' has an untranslatable branch; not pushing any of it down");
            return "";
        }
        translated.push_back(std::move(child_filter));
    }

    if (translated.empty()) {
        return "";
    }

    std::stringstream result;
    result << "(";
    for (idx_t i = 0; i < translated.size(); ++i) {
        if (i > 0) {
            result << " or ";
        }
        result << translated[i];
    }
    result << ")";
    return result.str();
}

std::string ODataPredicatePushdownHelper::SelectClause() const {
    return select_clause;
}

std::string ODataPredicatePushdownHelper::FilterClause() const {
    return filter_clause;
}

std::string ODataPredicatePushdownHelper::TopClause() const {
    return top_clause;
}

std::string ODataPredicatePushdownHelper::SkipClause() const {
    return skip_clause;
}

std::string ODataPredicatePushdownHelper::ExpandClause() const {
    return expand_clause;
}



} // namespace erpl_web
