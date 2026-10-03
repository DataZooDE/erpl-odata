#include "duckdb_argument_helper.hpp"
#include <optional>
#include <limits>
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

// The odata_read() table function itself: its DuckDB registration, the bind
// entry point and the bind-time named-parameter / expand-clause helpers.
// The bind data it produces, the scan state that consumes it, pushdown wiring,
// type resolution and error mapping live in the sibling odata_read_*.cpp,
// odata_type_resolver.cpp and odata_error_mapping.cpp translation units.
//
// Declarations live in src/include/odata_read_functions.hpp.

namespace erpl_web {

using duckdb::PostHogTelemetry;

// -------------------------------------------------------------------------------------------------

static std::shared_ptr<HttpAuthParams>
AuthParamsFromInput(duckdb::ClientContext &context,
                    TableFunctionBindInput &input) {
    auto args = input.inputs;
    auto url = args[0].ToString();
    return ResolveHttpAuthFromSecrets(context, url);
}

// ============================================================================
// Helper Functions Implementation
// ============================================================================
namespace ODataReadBindHelpers {

void ProcessExpandClause(ODataReadBindData *bind_data,
                         const std::string &expand_clause) {
  if (expand_clause.empty()) {
    return;
  }

  ERPL_TRACE_DEBUG("ODATA_BIND",
                   std::string("Processing expand clause: ") + expand_clause);
  bind_data->PredicatePushdownHelper()->ConsumeExpand(expand_clause);

  // Parse the expand clause to set up expanded data schema
  auto expand_paths = ODataExpandParser::ParseExpandClause(expand_clause);
  std::vector<std::string> navigation_properties;
  std::vector<std::string> nested_full_paths;

  for (const auto &path : expand_paths) {
    navigation_properties.push_back(path.navigation_property);
    // Capture nested full expand paths for recursive inference
    for (const auto &sub : path.sub_expands) {
      nested_full_paths.push_back(path.navigation_property + "/" + sub);
    }
    // Fallback: parse nested expands from options if not captured
    if (path.sub_expands.empty()) {
      const std::string &raw = path.full_expand_path;
      size_t lp = raw.find('(');
      size_t rp = raw.rfind(')');
      if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
        std::string opts = raw.substr(lp + 1, rp - lp - 1);
        size_t k = opts.find("$expand=");
        if (k != std::string::npos) {
          std::string subs = opts.substr(k + 8);
          size_t semi = subs.find(';');
          if (semi != std::string::npos)
            subs = subs.substr(0, semi);
          std::stringstream ss(subs);
          std::string item;
          while (std::getline(ss, item, ',')) {
            // trim
            item.erase(0, item.find_first_not_of(" \t\r\n"));
            item.erase(item.find_last_not_of(" \t\r\n") + 1);
            if (!item.empty()) {
              nested_full_paths.push_back(path.navigation_property + "/" +
                                          item);
            }
          }
        }
      }
    }
  }

  bind_data->SetExpandedDataSchema(navigation_properties);
  bind_data->SetNestedExpandPaths(nested_full_paths);
}

std::string ExtractExpandClauseFromUrl(const std::string &url) {
  ERPL_TRACE_DEBUG("ODATA_BIND",
                   std::string("Processing URL for expand clause: ") + url);

  // Try standard $expand= pattern first
    size_t expand_pos = url.find("$expand=");
    if (expand_pos != std::string::npos) {
        size_t start_pos = expand_pos + 8; // length of "$expand="
        size_t end_pos = url.find('&', start_pos);
        if (end_pos == std::string::npos) {
            end_pos = url.length();
        }
    std::string expand_clause = url.substr(start_pos, end_pos - start_pos);
    ERPL_TRACE_DEBUG("ODATA_BIND", std::string("Found expand clause in URL: ") +
                                       expand_clause);
    return expand_clause;
  }

  // Try alternative expand= pattern
        expand_pos = url.find("expand=");
        if (expand_pos != std::string::npos) {
            size_t start_pos = expand_pos + 7; // length of "expand="
            size_t end_pos = url.find('&', start_pos);
            if (end_pos == std::string::npos) {
                end_pos = url.length();
            }
    std::string expand_clause = url.substr(start_pos, end_pos - start_pos);
    ERPL_TRACE_DEBUG("ODATA_BIND",
                     std::string("Found expand clause in URL (alternative): ") +
                         expand_clause);
    return expand_clause;
  }

  return "";
}

void ProcessNamedParameters(ODataReadBindData *bind_data,
                            const TableFunctionBindInput &input) {
  // Handle TOP parameter
    if (input.named_parameters.find("top") != input.named_parameters.end()) {
        auto limit_value = input.named_parameters["top"].GetValue<duckdb::idx_t>();
    ERPL_TRACE_DEBUG("ODATA_BIND",
                     duckdb::StringUtil::Format(
                         "Named parameter 'top' set to: %d", limit_value));
        bind_data->PredicatePushdownHelper()->ConsumeLimit(limit_value);
    }
    
  // Handle SKIP parameter
    if (input.named_parameters.find("skip") != input.named_parameters.end()) {
    auto offset_value =
        input.named_parameters["skip"].GetValue<duckdb::idx_t>();
    ERPL_TRACE_DEBUG("ODATA_BIND",
                     duckdb::StringUtil::Format(
                         "Named parameter 'skip' set to: %d", offset_value));
        bind_data->PredicatePushdownHelper()->ConsumeOffset(offset_value);
    }
    
  // Handle EXPAND parameter
    if (input.named_parameters.find("expand") != input.named_parameters.end()) {
    auto expand_value =
        input.named_parameters["expand"].GetValue<std::string>();
    ERPL_TRACE_DEBUG("ODATA_BIND", duckdb::StringUtil::Format(
                                       "Named parameter 'expand' set to: %s",
                                       expand_value.c_str()));
    ProcessExpandClause(bind_data, expand_value);
  }

  // Handle STRICT_TYPING parameter
  if (input.named_parameters.find("strict_typing") !=
      input.named_parameters.end()) {
      auto strict_value =
          input.named_parameters["strict_typing"].GetValue<bool>();
      ERPL_TRACE_DEBUG("ODATA_BIND",
                       duckdb::StringUtil::Format(
                           "Named parameter 'strict_typing' set to: %s",
                           strict_value ? "true" : "false"));
      bind_data->SetStrictTyping(strict_value);
  }

  // Handle MAX_PAGE_SIZE parameter
  if (input.named_parameters.find("max_page_size") !=
      input.named_parameters.end()) {
    // Validation is shared with odp_odata_read so the same parameter cannot behave
    // differently depending on which reader is asked (GitHub #185).
    const auto requested =
        ValidateMaxPageSizeParameter(input.named_parameters["max_page_size"]);
    ERPL_TRACE_DEBUG("ODATA_BIND",
                     duckdb::StringUtil::Format(
                         "Named parameter 'max_page_size' set to: %d", requested));
    bind_data->GetODataClient()->SetMaxPageSize(requested);
  }

  // Handle COUNT parameter
  if (input.named_parameters.find("count") != input.named_parameters.end()) {
      auto count_value =
          input.named_parameters["count"].GetValue<bool>();
      ERPL_TRACE_DEBUG("ODATA_BIND",
                       duckdb::StringUtil::Format(
                           "Named parameter 'count' set to: %s",
                           count_value ? "true" : "false"));
      if (count_value) {
          bind_data->PredicatePushdownHelper()->EnableInlineCount(true);
      }
  }
}

void SetupSchemaFromProbeResult(
    const ODataClientFactory::ProbeResult &probe_result,
    ODataReadBindData *bind_data, vector<LogicalType> &return_types,
    vector<string> &names) {
  if (probe_result.is_service_root) {
    names = {"name", "kind", "url"};
    return_types = {duckdb::LogicalTypeId::VARCHAR, 
                    duckdb::LogicalTypeId::VARCHAR,
                    duckdb::LogicalTypeId::VARCHAR};
  } else {
    // Use existing entity-set schema logic
    names = bind_data->GetResultNames();
    return_types = bind_data->GetResultTypes();
  }
}

} // namespace ODataReadBindHelpers

duckdb::unique_ptr<FunctionData>
ODataReadBind(ClientContext &context, TableFunctionBindInput &input,
              vector<LogicalType> &return_types, vector<string> &names) {
  PostHogTelemetry::Instance().RecordFunctionCall("odata_read");
  auto auth_params = AuthParamsFromInput(context, input);
  auto url = input.inputs[0].GetValue<std::string>();

  ERPL_TRACE_INFO("ODATA_BIND",
                  duckdb::StringUtil::Format(
                      "Binding OData read function for URL: %s", url.c_str()));

  try {
    // Read and validate max_page_size BEFORE probing. The probe response is page one -
    // FromProbeResult buffers it - and for an unprojected read the URL never changes, so
    // that buffer is never refetched. Applying the preference only afterwards left the
    // first page at the service's default size (GitHub #185).
    std::optional<uint32_t> max_page_size;
    if (input.named_parameters.find("max_page_size") != input.named_parameters.end()) {
      max_page_size = ValidateMaxPageSizeParameter(input.named_parameters["max_page_size"]);
    }

    // Single probe to determine content type and version
    auto probe_result = ODataClientFactory::ProbeUrl(url, auth_params, max_page_size);

  // Create appropriate bind data based on probe result with fallback heuristic
  duckdb::unique_ptr<ODataReadBindData> bind_data;
  if (ODataReadBindData::LooksLikeServiceRootUrl(url)) {
    ERPL_TRACE_INFO("ODATA_BIND", "Routing via service-root path based on URL heuristic");
    bind_data = ODataReadBindData::FromServiceRoot(url, auth_params);
  } else {
    bind_data = ODataReadBindData::FromProbeResult(probe_result);
  }

  // Set return types and names based on content type
  ODataReadBindHelpers::SetupSchemaFromProbeResult(
      probe_result, bind_data.get(), return_types, names);

  // A service root lists entity sets rather than reading one, and the block below - which
  // is where max_page_size is applied to the client - is skipped for it. The preference
  // still rides the probe request (it is passed to ProbeUrl above), but nothing would
  // carry it onto any later request, so apply it here too rather than accepting the
  // parameter and quietly ignoring it (GitHub #185).
  if (probe_result.is_service_root && max_page_size.has_value() &&
      bind_data->GetODataClient() != nullptr) {
    bind_data->GetODataClient()->SetMaxPageSize(max_page_size.value());
  }

  // Handle named parameters and URL expand clause (only for entity-set mode)
  if (!probe_result.is_service_root) {
    // Process named parameters (top, skip, expand)
    ODataReadBindHelpers::ProcessNamedParameters(bind_data.get(), input);

    // Process expand clause from URL if present
    auto url_expand_clause =
        ODataReadBindHelpers::ExtractExpandClauseFromUrl(url);
    if (!url_expand_clause.empty()) {
      ERPL_TRACE_DEBUG("ODATA_BIND",
                       std::string("Processing expand clause from URL: ") +
                           url_expand_clause);
      ODataReadBindHelpers::ProcessExpandClause(bind_data.get(),
                                                url_expand_clause);
    }

    // Update names and types after processing expand clauses
    names = bind_data->GetResultNames();
    return_types = bind_data->GetResultTypes();
  }

  ERPL_TRACE_INFO("ODATA_BIND",
                  duckdb::StringUtil::Format("Bound function with %d columns",
                                             return_types.size()));
    
    // More efficient string concatenation for debug logging
    if (!names.empty()) {
        std::string column_names;
    column_names.reserve(names.size() *
                         20); // Estimate average column name length
        for (size_t i = 0; i < names.size(); ++i) {
      if (i > 0)
        column_names += ", ";
            column_names += names[i];
        }
    ERPL_TRACE_DEBUG(
        "ODATA_BIND",
        duckdb::StringUtil::Format("Column names: %s", column_names.c_str()));
    }
    
    return std::move(bind_data);
    
  } catch (const duckdb::InvalidInputException& e) {
    // Re-throw DuckDB exceptions as-is (these are already well-formatted)
    throw;
  } catch (const std::runtime_error& e) {
    // Use shared error handling utility
    throw ODataErrorHandling::ConvertHttpErrorToUserFriendly(e, url, "OData", "sap_odata_show()");
  } catch (const std::exception& e) {
    // Handle other exceptions with context
    throw duckdb::InvalidInputException("Failed to bind OData function for URL: " + url + 
      ". Error: " + std::string(e.what()));
  }
}

TableFunctionSet CreateODataReadFunction() {
    TableFunctionSet function_set("odata_read");
    
    TableFunction read_entity_set({LogicalTypeId::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadBind), ODataReadTableInitGlobalState);
    read_entity_set.filter_pushdown = true;
    read_entity_set.projection_pushdown = true;
    read_entity_set.table_scan_progress = ODataReadTableProgress;
    
    // Add named parameters for TOP, SKIP, EXPAND, and COUNT
    read_entity_set.named_parameters["top"] = LogicalTypeId::UBIGINT;
    read_entity_set.named_parameters["skip"] = LogicalTypeId::UBIGINT;
    read_entity_set.named_parameters["expand"] = LogicalTypeId::VARCHAR;
    read_entity_set.named_parameters["count"] = LogicalTypeId::BOOLEAN;
    read_entity_set.named_parameters["max_page_size"] = LogicalTypeId::UBIGINT;
    // Off by default: turning today's silently-wrong queries into hard
    // failures would be its own regression. On, a value we cannot convert
    // fails the query instead of arriving as an indistinguishable NULL.
    read_entity_set.named_parameters["strict_typing"] = LogicalTypeId::BOOLEAN;

    function_set.AddFunction(read_entity_set);
    return function_set;
}

} // namespace erpl_web
