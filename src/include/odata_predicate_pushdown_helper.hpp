#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb_compat/filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

#include "datazoo/oauth2/http_client.hpp"
#include "odata_edm.hpp"
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <map>

namespace erpl_web {

class ODataPredicatePushdownHelper {
public:
    // Function type for resolving column names from activated indices
    using ColumnNameResolver = std::function<std::string(duckdb::column_t)>;
    
    explicit ODataPredicatePushdownHelper(const std::vector<std::string> &all_column_names);
    ODataPredicatePushdownHelper(const std::vector<std::string> &all_column_names,
                                 const std::vector<duckdb::LogicalType> &all_column_types);

    // Refresh the full-schema column names and types this helper reasons about.
    // The types are needed to decide whether $select is safe (see
    // IsNestedColumnType); they are optional and may be empty, in which case
    // $select is built without the nested-column guard.
    void SetColumnSchema(const std::vector<std::string> &names,
                         const std::vector<duckdb::LogicalType> &types);
    
    // Set OData version for proper syntax generation
    void SetODataVersion(ODataVersion version);
    ODataVersion GetODataVersion() const;
    
    // Column name resolution
    void SetColumnNameResolver(ColumnNameResolver resolver);
    
    // Consume DuckDB operations and convert to OData clauses
    void ConsumeColumnSelection(const std::vector<duckdb::column_t> &column_ids);
    void ConsumeFilters(duckdb::optional_ptr<duckdb::TableFilterSet> filters);
    // NOTE (see GitHub #90): a SQL `LIMIT` is NOT pushed down to `$top`.
    // DuckDB never hands a table function its result modifiers, so nothing in
    // this extension observes `LIMIT`/`OFFSET`. `$top`/`$skip` are produced
    // only from the explicit `top=`/`skip=` named parameters of odata_read()
    // (and the equivalent Datasphere/SAC parameters). A query such as
    // `SELECT * FROM odata_read(...) LIMIT 10` therefore still transfers every
    // page the service is willing to give.
    void ConsumeLimit(duckdb::idx_t limit);
    void ConsumeOffset(duckdb::idx_t offset);
    void ConsumeExpand(const std::string& expand_clause);
    
    // Get generated OData clauses
    std::string SelectClause() const;
    std::string FilterClause() const;
    std::string TopClause() const;
    std::string SkipClause() const;
    std::string ExpandClause() const;
    
    // Apply all clauses to a URL
    HttpUrl ApplyFiltersToUrl(const HttpUrl &base_url);
    
    // Inline count and skip token support
    void EnableInlineCount(bool enable);
    void SetSkipToken(const std::string& token);
    std::string InlineCountClause() const;
    std::string SkipTokenClause() const;

private:
    // OData version for proper syntax generation
    ODataVersion odata_version = ODataVersion::V4; // Default to V4
    
    // Column information (full schema order, as produced by
    // ODataReadBindData::GetResultNames(true) / GetResultTypes(true))
    std::vector<std::string> all_column_names;
    std::vector<duckdb::LogicalType> all_column_types;
    ColumnNameResolver column_name_resolver;
    
    // Generated OData clauses
    std::string select_clause;
    std::string filter_clause;
    std::string top_clause;
    std::string skip_clause;

    // Set when at least one filter could not be translated into $filter and was
    // therefore left for DuckDB to apply locally. $top and $skip must not be pushed
    // in that state: the server would apply them to the UNFILTERED result and return
    // a short page, so the query would silently produce fewer rows than asked for.
    bool has_untranslated_filter = false;
    std::string expand_clause;
    
    // Additional features
    bool inline_count_enabled = false;
    std::optional<std::string> skip_token;
    
    // Helper methods for building clauses
    std::string BuildSelectClause(const std::vector<duckdb::column_t> &column_ids) const;
    std::string BuildFilterClause(duckdb::optional_ptr<duckdb::TableFilterSet> filters);
    std::string BuildTopClause(duckdb::idx_t limit) const;
    std::string BuildSkipClause(duckdb::idx_t offset) const;
    
    // Filter translation methods
    std::string TranslateFilter(const duckdb::TableFilter &filter, const std::string &column_name) const;
    std::string TranslateConstantComparison(const duckdb::Value &constant, duckdb::ExpressionType comparison_type, const std::string &column_name) const;
    std::string TranslateInFilter(const duckdb_compat::InFilter &filter, const std::string &column_name) const;
    std::string TranslateConjunction(const duckdb_compat::ConjunctionAndFilter &filter, const std::string &column_name) const;
    std::string TranslateConjunction(const duckdb_compat::ConjunctionOrFilter &filter, const std::string &column_name) const;
    
    // True when the column at the given full-schema index maps to a nested
    // DuckDB type (LIST/STRUCT/MAP/ARRAY/UNION). Such columns come from OData
    // collection or complex properties, which many services reject inside
    // $select, so projection pushdown is skipped when one is requested.
    bool IsNestedColumnType(duckdb::column_t schema_index) const;

    // Refactored helper methods for ApplyFiltersToUrl
    void LogCurrentClauses() const;
    std::map<std::string, std::string> ParseExistingQueryParameters(const std::string& existing_query) const;
    void ApplyV2SpecificLogic(std::map<std::string, std::string>& existing_params);
    std::string GetEffectiveExpandList(const std::map<std::string, std::string>& existing_params) const;
    std::string ExtractSelectFields() const;
    std::set<std::string> ParseSelectedFields(const std::string& select_fields) const;
    std::vector<std::string> FindMissingNavigationProperties(
        const std::string& expand_list, 
        const std::set<std::string>& selected) const;
    void AugmentSelectClauseWithNavigationProperties(
        const std::string& select_fields, 
        const std::vector<std::string>& missing_nav_props);
    void MergeParametersIntoQuery(std::map<std::string, std::string>& existing_params);
    std::string RebuildQueryString(const std::map<std::string, std::string>& existing_params) const;
};

} // namespace erpl_web
