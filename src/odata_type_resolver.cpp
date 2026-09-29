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

// EDM -> DuckDB type resolution for navigation, entity and complex types.
// Companion to src/odata_data_extractor.cpp, which consumes it.
//
// Declarations live in src/include/odata_read_functions.hpp.

namespace erpl_web {

// ============================================================================
// ODataTypeResolver Implementation
// ============================================================================

ODataTypeResolver::ODataTypeResolver(
    std::shared_ptr<ODataEntitySetClient> odata_client)
    : odata_client(odata_client) {}

duckdb::LogicalType ODataTypeResolver::ResolveNavigationPropertyType(
    const std::string &property_name) const {
    try {
        auto entity_type = odata_client->GetCurrentEntityType();
        
    for (const auto &nav_prop : entity_type.navigation_properties) {
            if (nav_prop.name == property_name) {
                auto [is_collection, type_name] = ExtractCollectionType(nav_prop.type);
                
                duckdb::LogicalType base_type;
                if (type_name.find("Edm.") == 0) {
                    base_type = ConvertPrimitiveTypeString(type_name);
                } else {
                    base_type = ResolveEntityType(type_name);
                }
                
                if (is_collection) {
                    return duckdb::LogicalType::LIST(base_type);
                }
                
                return base_type;
            }
        }
  } catch (const std::exception &e) {
    ERPL_TRACE_WARN("TYPE_RESOLVER",
                    "Failed to resolve type for navigation property '" +
                        property_name + "': " + e.what());
    }
    
    return duckdb::LogicalTypeId::VARCHAR; // Default fallback
}

std::pair<bool, std::string> ODataTypeResolver::GetNavTargetFromCurrentEntity(
    const std::string &nav_prop) const {
  try {
    auto entity_type = odata_client->GetCurrentEntityType();
    for (const auto &np : entity_type.navigation_properties) {
      if (np.name == nav_prop) {
        auto [is_collection, type_name] = ExtractCollectionType(np.type);
        return {is_collection, type_name};
      }
    }
  } catch (...) {
  }
  return {false, std::string()};
}

std::pair<bool, std::string>
ODataTypeResolver::GetNavTargetOnEntity(const std::string &entity_type_name,
                                        const std::string &nav_prop) const {
  try {
    auto edmx = odata_client->GetMetadata();
    auto type_variant = edmx.FindType(entity_type_name);
    EntityType entity;
    if (std::holds_alternative<EntityType>(type_variant)) {
      entity = std::get<EntityType>(type_variant);
    } else {
      return {false, std::string()};
    }
    for (const auto &np : entity.navigation_properties) {
      if (np.name == nav_prop) {
        auto [is_collection, type_name] = ExtractCollectionType(np.type);
        return {is_collection, type_name};
      }
    }
  } catch (...) {
  }
  return {false, std::string()};
}

duckdb::LogicalType ODataTypeResolver::ResolveNavigationOnEntity(
    const std::string &entity_type_name, const std::string &nav_prop) const {
  auto [is_collection, type_name] =
      GetNavTargetOnEntity(entity_type_name, nav_prop);
  if (type_name.empty())
    return duckdb::LogicalTypeId::VARCHAR;
  duckdb::LogicalType base_type;
  if (type_name.find("Edm.") == 0) {
    base_type = ConvertPrimitiveTypeString(type_name);
  } else {
    base_type = ResolveEntityType(type_name);
  }
  if (is_collection) {
    return duckdb::LogicalType::LIST(base_type);
  }
  return base_type;
}

duckdb::LogicalType ODataTypeResolver::ConvertPrimitiveTypeString(
    const std::string &type_name) const {
    return DuckTypeConverter::ConvertEdmPrimitiveStringToLogicalType(type_name);
}

std::tuple<bool, std::string>
ODataTypeResolver::ExtractCollectionType(const std::string &type_name) const {
    std::regex collection_regex("Collection\\(([^\\)]+)\\)");
    std::smatch match;

    if (std::regex_search(type_name, match, collection_regex)) {
        return std::make_tuple(true, match[1]);
    } else {
        return std::make_tuple(false, type_name);
    }
}

duckdb::LogicalType
ODataTypeResolver::ResolveEntityType(const std::string &type_name) const {
    try {
        auto edmx = odata_client->GetMetadata();
        auto target_type = edmx.FindType(type_name);

        // Use DuckTypeConverter to get proper DuckDB type
        auto type_conv = DuckTypeConverter(edmx);
        return std::visit(type_conv, target_type);
    } catch (const std::exception &e) {
        ERPL_TRACE_WARN("TYPE_RESOLVER", "Failed to resolve entity type '" +
                                             type_name + "': " + e.what());
        return duckdb::LogicalTypeId::VARCHAR;
    }
}

duckdb::LogicalType
ODataTypeResolver::ResolveComplexType(const std::string &type_name) const {
    try {
        auto edmx = odata_client->GetMetadata();
        auto target_type = edmx.FindType(type_name);

        // Use DuckTypeConverter to get proper DuckDB type
        auto type_conv = DuckTypeConverter(edmx);
        return std::visit(type_conv, target_type);
    } catch (const std::exception &e) {
        ERPL_TRACE_WARN("TYPE_RESOLVER", "Failed to resolve complex type '" +
                                         type_name + "': " + e.what());
        return duckdb::LogicalTypeId::VARCHAR;
    }
}

} // namespace erpl_web
