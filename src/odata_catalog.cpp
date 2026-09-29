#include "odata_catalog.hpp"
#include "odata_read_functions.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "odata_attach_functions.hpp"
#include "erpl_odata_banner.hpp"

namespace erpl_web {

// Deprecated legacy helper removed in favor of DuckTypeConverter central API

// -------------------------------------------------------------------------------------------------
// ODataSchemaEntry Implementation
// -------------------------------------------------------------------------------------------------

ODataSchemaEntry::ODataSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info)
    : duckdb::SchemaCatalogEntry(catalog, info), tables_loaded(false) {
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateTable(duckdb::CatalogTransaction transaction, duckdb::BoundCreateTableInfo &info) {
    throw duckdb::NotImplementedException("Creating tables in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateFunction(duckdb::CatalogTransaction transaction, duckdb::CreateFunctionInfo &info) {
    throw duckdb::NotImplementedException("Creating functions in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateIndex(duckdb::CatalogTransaction transaction, duckdb::CreateIndexInfo &info, duckdb::TableCatalogEntry &table) {
    throw duckdb::NotImplementedException("Creating indexes in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateView(duckdb::CatalogTransaction transaction, duckdb::CreateViewInfo &info) {
    throw duckdb::NotImplementedException("Creating views in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateSequence(duckdb::CatalogTransaction transaction, duckdb::CreateSequenceInfo &info) {
    throw duckdb::NotImplementedException("Creating sequences in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateTableFunction(duckdb::CatalogTransaction transaction, duckdb::CreateTableFunctionInfo &info) {
    throw duckdb::NotImplementedException("Creating table functions in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateCopyFunction(duckdb::CatalogTransaction transaction, duckdb::CreateCopyFunctionInfo &info) {
    throw duckdb::NotImplementedException("Creating copy functions in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreatePragmaFunction(duckdb::CatalogTransaction transaction, duckdb::CreatePragmaFunctionInfo &info) {
    throw duckdb::NotImplementedException("Creating pragma functions in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateCollation(duckdb::CatalogTransaction transaction, duckdb::CreateCollationInfo &info) {
    throw duckdb::NotImplementedException("Creating collations in OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::CreateType(duckdb::CatalogTransaction transaction, duckdb::CreateTypeInfo &info) {
    throw duckdb::NotImplementedException("Creating types in OData schemas is not supported");
}

void ODataSchemaEntry::Alter(duckdb::CatalogTransaction transaction, duckdb::AlterInfo &info) {
    throw duckdb::NotImplementedException("Altering OData schemas is not supported");
}

void ODataSchemaEntry::Scan(duckdb::ClientContext &context, duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) {
    if (type == duckdb::CatalogType::TABLE_ENTRY) {
        std::lock_guard<std::mutex> lock(tables_mutex);
        if (!tables_loaded) {
            LoadTables();
            tables_loaded = true;
        }
        for (auto& table_pair : table_entries) {
            callback(*table_pair.second);
        }
    }
}

void ODataSchemaEntry::Scan(duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) {
    if (type == duckdb::CatalogType::TABLE_ENTRY) {
        std::lock_guard<std::mutex> lock(tables_mutex);
        if (!tables_loaded) {
            LoadTables();
            tables_loaded = true;
        }
        for (auto& table_pair : table_entries) {
            callback(*table_pair.second);
        }
    }
}

void ODataSchemaEntry::DropEntry(duckdb::ClientContext &context, duckdb::DropInfo &info) {
    throw duckdb::NotImplementedException("Dropping entries from OData schemas is not supported");
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::GetEntry(duckdb::CatalogTransaction transaction, duckdb::CatalogType type, const std::string &name) {
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

duckdb::optional_ptr<duckdb::CatalogEntry> ODataSchemaEntry::LookupEntry(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info) {
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

void ODataSchemaEntry::LoadTables() {
    // This method should only be called while holding the tables_mutex
    auto &odata_catalog = static_cast<ODataCatalog&>(catalog);

    // Metadata failures are deliberately not swallowed here: a 401, a TLS handshake failure or an
    // unreachable host must not be reported as "that table does not exist". LoadTables() is only
    // reached lazily from Scan()/GetEntry()/LookupEntry(), so ATTACH itself still succeeds and the
    // real cause surfaces on the first statement that needs the schema. tables_loaded is set by the
    // callers only after this method returns, hence a transient failure is retried on the next use.
    auto &metadata = odata_catalog.GetCachedMetadata();
    auto entity_sets = metadata.FindEntitySets();

    // Build into a local map so a failure never leaves a half-populated catalog behind
    std::unordered_map<std::string, duckdb::unique_ptr<ODataTableEntry>> loaded_entries;
    loaded_entries.reserve(entity_sets.size());

    for (const auto& entity_set : entity_sets) {
        if (odata_catalog.IsIgnored(entity_set.name)) {
            continue;
        }

        duckdb::CreateTableInfo table_info;
        table_info.table = entity_set.name;
        table_info.schema = name;

        try {
            auto type_variant = metadata.FindType(entity_set.entity_type_name);
            if (std::holds_alternative<EntityType>(type_variant)) {
                auto entity_type = std::get<EntityType>(type_variant);
                for (const auto& property : entity_type.properties) {
                    // Prefer property-aware central mapping (precision/scale + collection)
                    auto logical_type = DuckTypeConverter::BuildLogicalTypeForProperty(property, metadata);
                    table_info.columns.AddColumn(duckdb::ColumnDefinition(property.name, logical_type));
                }
            } else {
                // Fallback for entity type not found
                table_info.columns.AddColumn(duckdb::ColumnDefinition("id", duckdb::LogicalType::VARCHAR));
            }
        } catch (const std::exception& e) {
            // Fallback for entity type not found
            table_info.columns.AddColumn(duckdb::ColumnDefinition("id", duckdb::LogicalType::VARCHAR));
        }

        auto table_entry = duckdb::make_uniq<ODataTableEntry>(catalog, *this, table_info);
        loaded_entries[entity_set.name] = std::move(table_entry);
    }

    table_entries = std::move(loaded_entries);
}

// -------------------------------------------------------------------------------------------------
// ODataTableEntry Implementation
// -------------------------------------------------------------------------------------------------

ODataTableEntry::ODataTableEntry(duckdb::Catalog &catalog, duckdb::SchemaCatalogEntry &schema, duckdb::CreateTableInfo &info)
    : duckdb::TableCatalogEntry(catalog, schema, info) {
}

duckdb::unique_ptr<duckdb::BaseStatistics> ODataTableEntry::GetStatistics(duckdb::ClientContext &context, duckdb::column_t column_id) {
    return nullptr;
}

duckdb::TableFunction ODataTableEntry::GetScanFunction(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::FunctionData> &bind_data) {
    // Create a custom TableFunction for this OData table
    auto &odata_catalog = static_cast<ODataCatalog&>(catalog);
    
    // Set up the URL for this specific entity set
    auto service_url = odata_catalog.ServiceUrl().ToString();
    if (!service_url.empty() && service_url.back() != '/') {
        service_url += "/";
    }
    service_url += name; // Add the entity set name
    
    // Get auth params from the service client
    auto auth_params = odata_catalog.GetServiceClient().AuthParams();
    
    // Create bind data using the existing factory method
    auto odata_bind_data = ODataReadBindData::FromEntitySetRoot(service_url, auth_params);
    
    // Set the bind data
    bind_data = std::move(odata_bind_data);
    
    // Create and return a TableFunction with OData scan capabilities
    duckdb::TableFunction table_function("odata_table_scan", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadBind), ODataReadTableInitGlobalState);
    table_function.filter_pushdown = true;
    table_function.projection_pushdown = true;
    table_function.table_scan_progress = ODataReadTableProgress;
    
    return table_function;
}

duckdb::TableStorageInfo ODataTableEntry::GetStorageInfo(duckdb::ClientContext &context) {
    return duckdb::TableStorageInfo();  // Use default constructor
}

void ODataTableEntry::BindUpdateConstraints(duckdb::Binder &binder, duckdb::LogicalGet &get, duckdb::LogicalProjection &proj, duckdb::LogicalUpdate &update, duckdb::ClientContext &context) {
    throw duckdb::NotImplementedException("Updates are not supported on OData tables");
}

// -------------------------------------------------------------------------------------------------
// ODataCatalog Implementation
// -------------------------------------------------------------------------------------------------

ODataCatalog::ODataCatalog(duckdb::AttachedDatabase &db, 
                           const std::string &url, 
                           std::shared_ptr<HttpAuthParams> auth_params, 
                           const std::string &ignore_pattern)
    : duckdb::Catalog(db), 
      service_client(std::make_shared<HttpClient>(), HttpUrl(url), auth_params),
      ignore_pattern(ignore_pattern),
      path_(url) {
    
    // Create the main schema
    duckdb::CreateSchemaInfo schema_info;
    schema_info.schema = "main";
    main_schema = duckdb::make_uniq<ODataSchemaEntry>(*this, schema_info);
}

duckdb::string ODataCatalog::GetCatalogType() {
    return "odata";
}

void ODataCatalog::Initialize(bool load_builtin) {
    // Nothing to initialize
}

void ODataCatalog::Initialize(duckdb::optional_ptr<duckdb::ClientContext> context, bool load_builtin) {
    // Nothing to initialize
}

void ODataCatalog::FinalizeLoad(duckdb::optional_ptr<duckdb::ClientContext> context) {
    // Nothing to finalize
}

duckdb::string ODataCatalog::GetDBPath() {
    return path_;
}

bool ODataCatalog::InMemory() {
    return true;
}

bool ODataCatalog::SupportsTimeTravel() const {
    return false;
}

duckdb::string ODataCatalog::GetDefaultSchema() const {
    return "main";
}

duckdb::DatabaseSize ODataCatalog::GetDatabaseSize(duckdb::ClientContext &context) {
    return duckdb::DatabaseSize();
}

duckdb::vector<duckdb::MetadataBlockInfo> ODataCatalog::GetMetadataInfo(duckdb::ClientContext &context) {
    return duckdb::vector<duckdb::MetadataBlockInfo>();
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> ODataCatalog::LookupSchema(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info, duckdb::OnEntryNotFound if_not_found) {
    if (lookup_info.GetEntryName() == "main") {
        return main_schema.get();
    }
    if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
        throw duckdb::CatalogException("Schema \"%s\" not found", lookup_info.GetEntryName());
    }
    return nullptr;
}

duckdb::optional_ptr<duckdb::CatalogEntry> ODataCatalog::CreateSchema(duckdb::CatalogTransaction transaction, duckdb::CreateSchemaInfo &info) {
    throw duckdb::NotImplementedException("Creating schemas in OData catalogs is not supported");
}

void ODataCatalog::ScanSchemas(duckdb::ClientContext &context, std::function<void(duckdb::SchemaCatalogEntry &)> callback) {
    callback(*main_schema);
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> ODataCatalog::GetSchema(duckdb::CatalogTransaction transaction, const std::string &schema_name, duckdb::OnEntryNotFound if_not_found, duckdb::QueryErrorContext error_context) {
    if (schema_name == "main") {
        return main_schema.get();
    }
    if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
        throw duckdb::CatalogException("Schema \"%s\" not found", schema_name);
    }
    return nullptr;
}

duckdb::PhysicalOperator &ODataCatalog::PlanCreateTableAs(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalCreateTable &op, duckdb::PhysicalOperator &plan) {
    throw duckdb::NotImplementedException("CREATE TABLE AS is not supported on OData catalogs");
}

duckdb::PhysicalOperator &ODataCatalog::PlanInsert(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalInsert &op, duckdb::optional_ptr<duckdb::PhysicalOperator> plan) {
    throw duckdb::NotImplementedException("INSERT is not supported on OData catalogs");
}

duckdb::PhysicalOperator &ODataCatalog::PlanDelete(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalDelete &op, duckdb::PhysicalOperator &plan) {
    throw duckdb::NotImplementedException("DELETE is not supported on OData catalogs");
}

duckdb::PhysicalOperator &ODataCatalog::PlanUpdate(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner, duckdb::LogicalUpdate &op, duckdb::PhysicalOperator &plan) {
    throw duckdb::NotImplementedException("UPDATE is not supported on OData catalogs");
}

duckdb::unique_ptr<duckdb::LogicalOperator> ODataCatalog::BindCreateIndex(duckdb::Binder &binder, duckdb::CreateStatement &stmt, duckdb::TableCatalogEntry &table, duckdb::unique_ptr<duckdb::LogicalOperator> plan) {
    throw duckdb::NotImplementedException("CREATE INDEX is not supported on OData catalogs");
}

void ODataCatalog::DropSchema(duckdb::ClientContext &context, duckdb::DropInfo &info) {
    throw duckdb::NotImplementedException("DROP SCHEMA is not supported on OData catalogs");
}

HttpUrl ODataCatalog::ServiceUrl() {
    return service_client.Url();
}

std::vector<std::string> ODataCatalog::GetTableNames() {
    auto &metadata = GetCachedMetadata();
    auto entity_sets = metadata.FindEntitySets();

    std::vector<std::string> table_names;
    table_names.reserve(entity_sets.size());
    for (const auto& entity_set : entity_sets) {
        if (!IsIgnored(entity_set.name)) {
            table_names.push_back(entity_set.name);
        }
    }
    return table_names;
}

ODataSchemaEntry& ODataCatalog::GetMainSchema() {
    return *main_schema;
}

void ODataCatalog::GetTableInfo(const std::string &table_name, 
                                 duckdb::ColumnList &columns, 
                                 std::vector<duckdb::unique_ptr<duckdb::Constraint>> &constraints) {
    auto &metadata = GetCachedMetadata();
    auto entity_sets = metadata.FindEntitySets();

    for (const auto& entity_set : entity_sets) {
        if (entity_set.name != table_name) {
            continue;
        }

        try {
            auto type_variant = metadata.FindType(entity_set.entity_type_name);
            if (std::holds_alternative<EntityType>(type_variant)) {
                auto entity_type = std::get<EntityType>(type_variant);
                for (const auto& property : entity_type.properties) {
                    auto logical_type = DuckTypeConverter::BuildLogicalTypeForProperty(property, metadata);
                    columns.AddColumn(duckdb::ColumnDefinition(property.name, logical_type));
                }
                return;
            }
        } catch (const std::exception& e) {
            // Fallback for entity type not found
            columns.AddColumn(duckdb::ColumnDefinition("id", duckdb::LogicalType::VARCHAR));
            return;
        }
    }
}

std::optional<ODataEntitySetReference> ODataCatalog::GetEntitySetReference(const std::string &table_name) {
    try {
        auto &metadata = GetCachedMetadata();
        auto entity_sets = metadata.FindEntitySets();
        
        for (const auto& entity_set : entity_sets) {
            if (entity_set.name == table_name) {
                ODataEntitySetReference ref;
                ref.name = entity_set.name;
                ref.url = entity_set.name; // Use entity set name as URL for now
                return ref;
            }
        }
    } catch (const std::exception& e) {
        // If metadata fetch fails, return nullopt
    }
    return std::nullopt;
}

ODataServiceClient& ODataCatalog::GetServiceClient() {
    return service_client;
}

const Edmx &ODataCatalog::GetCachedMetadata() {
    std::lock_guard<std::mutex> lock(metadata_mutex);
    if (!cached_metadata) {
        try {
            // GetMetadata() populates the process-global EdmCache as a side effect. Taking the
            // snapshot back out of the cache - rather than storing the returned value - is what
            // keeps the catalog from holding a second full EDMX for the whole life of the ATTACH.
            // See GitHub #106.
            auto edmx = service_client.GetMetadata();
            const auto metadata_url = service_client.GetMetadataContextUrl();
            cached_metadata = EdmCache::GetInstance().Get(metadata_url);
            if (!cached_metadata) {
                // The client did not cache it (or the entry expired in between); adopt ours.
                cached_metadata = EdmCache::GetInstance().Set(metadata_url, std::move(edmx));
            }
        } catch (const std::exception &e) {
            // Preserve the underlying cause (401, TLS failure, DNS failure, malformed EDMX, ...)
            // together with the service URL instead of degrading it into an empty schema.
            throw duckdb::IOException(duckdb::StringUtil::Format(
                "Failed to load OData metadata from '%s': %s", path_, std::string(e.what())));
        }
    }
    return *cached_metadata;
}

void ODataCatalog::InvalidateCachedMetadata() {
    std::lock_guard<std::mutex> lock(metadata_mutex);
    if (cached_metadata) {
        EdmCache::GetInstance().Invalidate(service_client.GetMetadataContextUrl());
    }
    // Any caller still reading the previous snapshot keeps it alive through its own
    // shared_ptr; resetting ours only detaches this catalog from it.
    cached_metadata.reset();
}

bool ODataCatalog::IsIgnored(const std::string &entity_set_name) const {
    return MatchesIgnorePattern(entity_set_name, ignore_pattern);
}

bool ODataCatalog::MatchesIgnorePattern(const std::string &entity_set_name, const std::string &ignore_pattern) {
    if (ignore_pattern.empty()) {
        return false;
    }
    return ODataAttachBindData::MatchPattern(entity_set_name, ignore_pattern);
}

} // namespace erpl_web
