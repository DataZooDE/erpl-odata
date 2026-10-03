#include "duckdb_argument_helper.hpp"
#include "odata_read_functions.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "odata_client.hpp"
#include "odata_edm.hpp"

#include "tracing.hpp"
#include "telemetry.hpp"

#include <optional>
#include <set>
#include "erpl_odata_banner.hpp"
#include "scan_row_cursor.hpp"

namespace erpl_web {

static std::shared_ptr<HttpAuthParams>
AuthParamsFromDescribeInput(duckdb::ClientContext &context,
                            TableFunctionBindInput &input) {
  auto args = input.inputs;
  auto url = args[0].ToString();
  return ResolveHttpAuthFromSecrets(context, url);
}

// ============================================================================
// OData Describe Function Implementation
// ============================================================================

class ODataDescribeBindData : public TableFunctionData {
public:
    std::string url;
    std::string resource_type;  // "service" or "entity_set"
    std::string entity_set_name;
    std::shared_ptr<ODataEntitySetClient> entity_client;
    std::shared_ptr<ODataServiceClient> service_client;
    std::shared_ptr<HttpAuthParams> auth_params;
    bool data_loaded = false;
    std::vector<duckdb::Value> result_row;  // Single row result
};

static duckdb::LogicalType CreatePropertyStructType() {
    child_list_t<LogicalType> property_struct;
    property_struct.emplace_back("name", LogicalTypeId::VARCHAR);
    property_struct.emplace_back("duckdb_type", LogicalTypeId::VARCHAR);
    property_struct.emplace_back("edm_type", LogicalTypeId::VARCHAR);
    property_struct.emplace_back("is_nullable", LogicalTypeId::BOOLEAN);
    property_struct.emplace_back("is_key", LogicalTypeId::BOOLEAN);
    property_struct.emplace_back("max_length", LogicalTypeId::INTEGER);
    property_struct.emplace_back("precision", LogicalTypeId::INTEGER);
    property_struct.emplace_back("scale", LogicalTypeId::INTEGER);
    return LogicalType::STRUCT(property_struct);
}

static duckdb::LogicalType CreateNavigationStructType() {
    child_list_t<LogicalType> nav_struct;
    nav_struct.emplace_back("name", LogicalTypeId::VARCHAR);
    nav_struct.emplace_back("target_entity", LogicalTypeId::VARCHAR);
    
    child_list_t<LogicalType> target_type_struct;
    target_type_struct.emplace_back("name", LogicalTypeId::VARCHAR);
    target_type_struct.emplace_back("property_count", LogicalTypeId::BIGINT);
    target_type_struct.emplace_back("nav_property_count", LogicalTypeId::BIGINT);
    nav_struct.emplace_back("target_entity_type", LogicalType::STRUCT(target_type_struct));
    
    nav_struct.emplace_back("is_collection", LogicalTypeId::BOOLEAN);
    nav_struct.emplace_back("partner", LogicalTypeId::VARCHAR);
    
    child_list_t<LogicalType> constraint_struct;
    constraint_struct.emplace_back("property", LogicalTypeId::VARCHAR);
    constraint_struct.emplace_back("referenced_property", LogicalTypeId::VARCHAR);
    nav_struct.emplace_back("referential_constraints", LogicalType::LIST(LogicalType::STRUCT(constraint_struct)));
    
    return LogicalType::STRUCT(nav_struct);
}

static duckdb::LogicalType CreateEntitySetStructType() {
    child_list_t<LogicalType> entity_set_struct;
    entity_set_struct.emplace_back("name", LogicalTypeId::VARCHAR);
    entity_set_struct.emplace_back("entity_type", LogicalTypeId::VARCHAR);
    entity_set_struct.emplace_back("url", LogicalTypeId::VARCHAR);
    return LogicalType::STRUCT(entity_set_struct);
}

static duckdb::LogicalType CreateFunctionStructType() {
    child_list_t<LogicalType> function_struct;
    function_struct.emplace_back("name", LogicalTypeId::VARCHAR);
    function_struct.emplace_back("return_type", LogicalTypeId::VARCHAR);
    
    child_list_t<LogicalType> param_struct;
    param_struct.emplace_back("name", LogicalTypeId::VARCHAR);
    param_struct.emplace_back("type", LogicalTypeId::VARCHAR);
    param_struct.emplace_back("nullable", LogicalTypeId::BOOLEAN);
    function_struct.emplace_back("parameters", LogicalType::LIST(LogicalType::STRUCT(param_struct)));
    
    return LogicalType::STRUCT(function_struct);
}

// Helper to create empty property list with proper schema
static duckdb::Value CreateEmptyPropertyList() {
    return Value::LIST(CreatePropertyStructType(), vector<Value>());
}

// Helper to create empty navigation property list with proper schema
static duckdb::Value CreateEmptyNavigationList() {
    return Value::LIST(CreateNavigationStructType(), vector<Value>());
}

// Helper to create empty entity sets list with proper schema
static duckdb::Value CreateEmptyEntitySetsList() {
    return Value::LIST(CreateEntitySetStructType(), vector<Value>());
}

// Helper to create empty functions list with proper schema
static duckdb::Value CreateEmptyFunctionsList() {
    return Value::LIST(CreateFunctionStructType(), vector<Value>());
}

// A CSDL facet that the metadata never declared. Reporting it as 0 made it
// indistinguishable from a declared zero -- and Scale="0" versus an absent Scale is
// precisely what decides DECIMAL(p,0) against DOUBLE, so the one field a caller would
// consult to understand the type could not express the difference.
static duckdb::Value AbsentFacet() {
    return Value(LogicalType::INTEGER);
}

static duckdb::Value FacetValue(int facet) {
    return facet >= 0 ? Value::INTEGER(facet) : AbsentFacet();
}

// Helper to build property struct
//
// duckdb_type must be what the READER binds, which is why this goes through the
// property-aware mapping rather than the type-name-only one: the latter is blind to an
// Edm.Decimal's Precision/Scale facets, to Collection(...) and to complex types, so it
// answered "DECIMAL" for a DECIMAL(38,18) column and "VARCHAR" for both a VARCHAR[] and
// a STRUCT. A describe that disagrees with the scan is worse than no describe at all.
// (GitHub #254)
static duckdb::Value BuildPropertyStruct(const Property& prop, const Edmx& edmx, bool is_key = false) {
    auto duck_type_str = DuckTypeConverter::BuildLogicalTypeForProperty(prop, edmx).ToString();
    
    child_list_t<Value> property_struct;
    property_struct.emplace_back("name", Value(prop.name));
    property_struct.emplace_back("duckdb_type", Value(duck_type_str));
    property_struct.emplace_back("edm_type", Value(prop.type_name));
    property_struct.emplace_back("is_nullable", Value(prop.nullable));
    property_struct.emplace_back("is_key", Value(is_key));
    property_struct.emplace_back("max_length", FacetValue(prop.max_length));
    property_struct.emplace_back("precision", FacetValue(prop.precision));
    property_struct.emplace_back("scale", FacetValue(prop.scale));
    
    return Value::STRUCT(property_struct);
}

// Helper to recursively build navigation property info
static duckdb::Value BuildNavigationPropertyStruct(
    const NavigationProperty& nav_prop, 
    const Edmx& edmx,
    std::set<std::string>& visited_types  // Prevent infinite recursion
) {
    child_list_t<Value> nav_struct;
    nav_struct.emplace_back("name", Value(nav_prop.name));
    
    // Extract target entity type
    DuckTypeConverter converter(const_cast<Edmx&>(edmx));
    auto [is_collection, target_type] = converter.ExtractCollectionType(nav_prop.type);
    nav_struct.emplace_back("target_entity", Value(target_type));
    
    // Recursive: Get target entity type info (if not already visited)
    child_list_t<Value> type_info;
    if (visited_types.find(target_type) == visited_types.end()) {
        visited_types.insert(target_type);
        
        auto entity_type_variant = edmx.FindType(target_type);
        if (std::holds_alternative<EntityType>(entity_type_variant)) {
            // Build a simplified struct of the target entity
            auto& target_entity = std::get<EntityType>(entity_type_variant);
            
            type_info.emplace_back("name", Value(target_entity.name));
            type_info.emplace_back("property_count", Value((int64_t)target_entity.properties.size()));
            type_info.emplace_back("nav_property_count", Value((int64_t)target_entity.navigation_properties.size()));
        } else {
            // Provide empty values when target entity not found
            type_info.emplace_back("name", Value(""));
            type_info.emplace_back("property_count", Value((int64_t)0));
            type_info.emplace_back("nav_property_count", Value((int64_t)0));
        }
    } else {
        // Provide empty values when already visited (to avoid infinite recursion)
        type_info.emplace_back("name", Value(""));
        type_info.emplace_back("property_count", Value((int64_t)0));
        type_info.emplace_back("nav_property_count", Value((int64_t)0));
    }
    Value target_type_value = Value::STRUCT(type_info);
    nav_struct.emplace_back("target_entity_type", target_type_value);
    nav_struct.emplace_back("is_collection", Value(is_collection));
    nav_struct.emplace_back("partner", nav_prop.partner.empty() ? Value("") : Value(nav_prop.partner));
    
    // Add referential constraints
    vector<Value> constraints;
    for (const auto& constraint : nav_prop.referential_constraints) {
        child_list_t<Value> constraint_struct;
        constraint_struct.emplace_back("property", Value(constraint.property));
        constraint_struct.emplace_back("referenced_property", Value(constraint.referenced_property));
        constraints.push_back(Value::STRUCT(constraint_struct));
    }
    
    // Handle empty constraints list
    if (constraints.empty()) {
        child_list_t<LogicalType> constraint_struct;
        constraint_struct.emplace_back("property", LogicalTypeId::VARCHAR);
        constraint_struct.emplace_back("referenced_property", LogicalTypeId::VARCHAR);
        nav_struct.emplace_back("referential_constraints", Value::LIST(LogicalType::STRUCT(constraint_struct), vector<Value>()));
    } else {
        nav_struct.emplace_back("referential_constraints", Value::LIST(constraints));
    }
    
    return Value::STRUCT(nav_struct);
}

// Bind function for odata_describe
static unique_ptr<FunctionData> ODataDescribeBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names
) {
    PostHogTelemetry::Instance().RecordFunctionCall("odata_describe");
    ERPL_TRACE_INFO("ODATA_DESCRIBE_BIND", "Starting OData describe bind");

    // Reuse authentication handling from ODataReadBind
    auto auth_params = AuthParamsFromDescribeInput(context, input);
    
    // Get URL argument
    auto url = input.inputs[0].GetValue<string>();
    ERPL_TRACE_INFO("ODATA_DESCRIBE_BIND", "Describing URL: " + url);
    
    // Reuse probe logic from ODataClientFactory
    auto probe_result = ODataClientFactory::ProbeUrl(url, auth_params);
    
    auto bind_data = make_uniq<ODataDescribeBindData>();
    bind_data->url = url;
    bind_data->auth_params = auth_params;
    
    // Create appropriate client based on probe result
    if (probe_result.is_service_root) {
        bind_data->resource_type = "service";
        bind_data->service_client = ODataClientFactory::CreateServiceClient(probe_result);
        ERPL_TRACE_INFO("ODATA_DESCRIBE_BIND", "Detected service root");
    } else {
        bind_data->resource_type = "entity_set";
        bind_data->entity_client = ODataClientFactory::CreateEntitySetClient(probe_result);
        
        // Extract entity set name from URL
        HttpUrl parsed_url(url);
        auto path = parsed_url.Path();
        auto last_slash = path.find_last_of('/');
        if (last_slash != std::string::npos) {
            auto entity_set_part = path.substr(last_slash + 1);
            // Remove any query parameters
            auto query_pos = entity_set_part.find('?');
            if (query_pos != std::string::npos) {
                entity_set_part = entity_set_part.substr(0, query_pos);
            }
            // Remove any parentheses (for specific entity access)
            auto paren_pos = entity_set_part.find('(');
            if (paren_pos != std::string::npos) {
                entity_set_part = entity_set_part.substr(0, paren_pos);
            }
            bind_data->entity_set_name = entity_set_part;
        }
        ERPL_TRACE_INFO("ODATA_DESCRIBE_BIND", "Detected entity set: " + bind_data->entity_set_name);
    }
    
    // Define output schema
    names = {"url", "resource_type", "entity_set_name", "entity_type_name", 
             "properties", "navigation_properties", "entity_sets", "functions"};
    
    return_types = {
        LogicalTypeId::VARCHAR,                    // url
        LogicalTypeId::VARCHAR,                    // resource_type
        LogicalTypeId::VARCHAR,                    // entity_set_name
        LogicalTypeId::VARCHAR,                    // entity_type_name
        LogicalType::LIST(CreatePropertyStructType()),     // properties
        LogicalType::LIST(CreateNavigationStructType()),   // navigation_properties
        LogicalType::LIST(CreateEntitySetStructType()),    // entity_sets
        LogicalType::LIST(CreateFunctionStructType())      // functions
    };
    
    return bind_data;
}

// Helper to build properties list for an entity type
static duckdb::Value BuildPropertiesList(const EntityType& entity_type, const std::set<std::string>& key_properties,
                                         const Edmx& edmx) {
    vector<Value> properties;
    for (const auto& prop : entity_type.properties) {
        bool is_key = key_properties.count(prop.name) > 0;
        properties.push_back(BuildPropertyStruct(prop, edmx, is_key));
    }
    return properties.empty() ? CreateEmptyPropertyList() : Value::LIST(properties);
}

// Helper to build navigation properties list for an entity type
static duckdb::Value BuildNavigationPropertiesList(
    const EntityType& entity_type, 
    const Edmx& metadata,
    const std::string& entity_type_name
) {
    vector<Value> nav_properties;
    std::set<std::string> visited_types;
    visited_types.insert(entity_type_name);
    
    for (const auto& nav_prop : entity_type.navigation_properties) {
        nav_properties.push_back(BuildNavigationPropertyStruct(nav_prop, metadata, visited_types));
    }
    
    return nav_properties.empty() ? CreateEmptyNavigationList() : Value::LIST(nav_properties);
}

// Helper to build entity sets list for service root
static duckdb::Value BuildEntitySetsList(const Edmx& metadata, const std::string& base_url) {
    vector<Value> entity_sets;
    
    for (const auto& schema : metadata.data_services.schemas) {
        for (const auto& container : schema.entity_containers) {
            for (const auto& entity_set : container.entity_sets) {
                child_list_t<Value> es_struct;
                es_struct.emplace_back("name", Value(entity_set.name));
                es_struct.emplace_back("entity_type", Value(entity_set.entity_type_name));
                
                // Build full URL for entity set
                std::string entity_url = base_url;
                if (!entity_url.empty() && entity_url.back() != '/') {
                    entity_url += "/";
                }
                entity_url += entity_set.name;
                
                es_struct.emplace_back("url", Value(entity_url));
                entity_sets.push_back(Value::STRUCT(es_struct));
            }
        }
    }
    
    return entity_sets.empty() ? CreateEmptyEntitySetsList() : Value::LIST(entity_sets);
}

// Helper to build functions list
static duckdb::Value BuildFunctionsList(const Edmx& metadata) {
    vector<Value> functions;
    
    for (const auto& schema : metadata.data_services.schemas) {
        for (const auto& func : schema.functions) {
            child_list_t<Value> func_struct;
            func_struct.emplace_back("name", Value(func.name));
            func_struct.emplace_back("return_type", Value(func.return_type));
            
            vector<Value> params;
            for (const auto& param : func.parameters) {
                child_list_t<Value> param_struct;
                param_struct.emplace_back("name", Value(param.name));
                param_struct.emplace_back("type", Value(param.type));
                param_struct.emplace_back("nullable", Value(param.nullable));
                params.push_back(Value::STRUCT(param_struct));
            }
            
            // Handle empty params list
            if (params.empty()) {
                child_list_t<LogicalType> param_struct;
                param_struct.emplace_back("name", LogicalTypeId::VARCHAR);
                param_struct.emplace_back("type", LogicalTypeId::VARCHAR);
                param_struct.emplace_back("nullable", LogicalTypeId::BOOLEAN);
                func_struct.emplace_back("parameters", Value::LIST(LogicalType::STRUCT(param_struct), vector<Value>()));
            } else {
                func_struct.emplace_back("parameters", Value::LIST(params));
            }
            functions.push_back(Value::STRUCT(func_struct));
        }
    }
    
    return functions.empty() ? CreateEmptyFunctionsList() : Value::LIST(functions);
}

// Helper to find entity set in metadata
static std::optional<EntitySet> FindEntitySet(const Edmx& metadata, const std::string& entity_set_name) {
    // Search in all schemas and containers
    for (const auto& schema : metadata.data_services.schemas) {
        for (const auto& container : schema.entity_containers) {
            for (const auto& es : container.entity_sets) {
                if (es.name == entity_set_name) {
                    return es;
                }
            }
        }
    }
    
    // Try alternative search method
    auto all_entity_sets = metadata.FindEntitySets();
    for (const auto& es : all_entity_sets) {
        if (es.name == entity_set_name) {
            return es;
        }
    }
    
    return std::nullopt;
}

// Helper to process entity set description
static void ProcessEntitySetDescription(
    ODataDescribeBindData& bind_data,
    const Edmx& metadata,
    vector<Value>& row_values
) {
    row_values.push_back(Value(bind_data.entity_set_name));
    
    auto entity_set_opt = FindEntitySet(metadata, bind_data.entity_set_name);
    
    if (entity_set_opt.has_value()) {
        const auto& entity_set = entity_set_opt.value();
        row_values.push_back(Value(entity_set.entity_type_name));
        
        // Get entity type
        auto entity_type_variant = metadata.FindType(entity_set.entity_type_name);
        if (std::holds_alternative<EntityType>(entity_type_variant)) {
            const auto& entity_type = std::get<EntityType>(entity_type_variant);
            
            // Get key properties
            std::set<std::string> key_properties;
            for (const auto& key_ref : entity_type.key.property_refs) {
                key_properties.insert(key_ref.name);
            }
            
            // Build properties and navigation properties
            row_values.push_back(BuildPropertiesList(entity_type, key_properties, metadata));
            row_values.push_back(BuildNavigationPropertiesList(entity_type, metadata, entity_set.entity_type_name));
        } else {
            // Entity type not found
            row_values.push_back(CreateEmptyPropertyList());
            row_values.push_back(CreateEmptyNavigationList());
        }
    } else {
        // Entity set not found
        row_values.push_back(Value(""));  // entity_type_name
        row_values.push_back(CreateEmptyPropertyList());
        row_values.push_back(CreateEmptyNavigationList());
    }
    
    // Empty entity_sets and functions for entity set description
    row_values.push_back(CreateEmptyEntitySetsList());
    row_values.push_back(CreateEmptyFunctionsList());
}

// Helper to process service root description
static void ProcessServiceRootDescription(
    ODataDescribeBindData& bind_data,
    const Edmx& metadata,
    vector<Value>& row_values
) {
    row_values.push_back(Value(""));  // entity_set_name
    row_values.push_back(Value(""));  // entity_type_name
    row_values.push_back(CreateEmptyPropertyList());
    row_values.push_back(CreateEmptyNavigationList());
    row_values.push_back(BuildEntitySetsList(metadata, bind_data.url));
    row_values.push_back(BuildFunctionsList(metadata));
}

// Scan function for odata_describe
static void ODataDescribeScan(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output
) {
    auto &bind_data = data_p.bind_data->CastNoConst<ODataDescribeBindData>();
    auto &state = data_p.global_state->Cast<ScanRowCursorState>();

    // Bind-time snapshot by design: the metadata is fetched on the first execution and
    // kept on the bind data, so re-executing a prepared statement replays it rather than
    // re-fetching. The cursor above is per-execution, so the row is emitted every time.
    // See the re-execution contract in graph_json_scan.hpp.
    if (!bind_data.data_loaded) {
        ERPL_TRACE_INFO("ODATA_DESCRIBE_SCAN", "Loading metadata for: " + bind_data.url);
        
        // Load metadata
        Edmx metadata;
        try {
            metadata = (bind_data.resource_type == "service") 
                ? bind_data.service_client->GetMetadata()
                : bind_data.entity_client->GetMetadata();
        } catch (const std::exception& e) {
            throw InvalidInputException("Failed to fetch metadata: " + std::string(e.what()));
        }
        
        // Build result row
        vector<Value> row_values;
        row_values.push_back(Value(bind_data.url));
        row_values.push_back(Value(bind_data.resource_type));
        
        if (bind_data.resource_type == "entity_set") {
            ProcessEntitySetDescription(bind_data, metadata, row_values);
        } else {
            ProcessServiceRootDescription(bind_data, metadata, row_values);
        }
        
        bind_data.result_row = row_values;
        bind_data.data_loaded = true;
        ERPL_TRACE_INFO("ODATA_DESCRIBE_SCAN", "Metadata loaded successfully");
    }
    
    // Output single row
    if (!state.finished && !bind_data.result_row.empty()) {
        for (idx_t i = 0; i < bind_data.result_row.size(); i++) {
            output.SetValue(i, 0, bind_data.result_row[i]);
        }
        output.SetCardinality(1);
        state.finished = true;  // The row is emitted once per EXECUTION, not once per bind
    } else {
        output.SetCardinality(0);
    }
}

// Create the odata_describe function
TableFunctionSet CreateODataDescribeFunction() {
    TableFunctionSet describe_func("odata_describe");
    
    TableFunction describe_function({LogicalTypeId::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataDescribeScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataDescribeBind));
    describe_function.init_global = ScanRowCursorState::Init;
    describe_function.named_parameters["secret"] = LogicalTypeId::VARCHAR;
    
    describe_func.AddFunction(describe_function);
    return describe_func;
}
} // namespace erpl_web
