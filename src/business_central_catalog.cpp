#include "business_central_catalog.hpp"
#include "odata_edm.hpp"
#include "odata_read_functions.hpp"
#include "odata_url_helpers.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include "erpl_odata_banner.hpp"

namespace erpl_web {

// -----------------------------------------------------------------------
// BcTableEntry
// -----------------------------------------------------------------------

BcTableEntry::BcTableEntry(duckdb::Catalog &catalog, duckdb::SchemaCatalogEntry &schema, duckdb::CreateTableInfo &info)
    : ODataTableEntry(catalog, schema, info) {
}

duckdb::TableFunction BcTableEntry::GetScanFunction(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::FunctionData> &bind_data) {
    auto &bc_catalog = catalog.Cast<BcCatalog>();

    auto entity_url = bc_catalog.CompanyUrl() + "/" + name;
    auto metadata_url = bc_catalog.MetadataContextUrl();
    auto auth_params = bc_catalog.GetServiceClient().AuthParams();

    ERPL_TRACE_DEBUG("BC_CATALOG", "BcTableEntry::GetScanFunction for entity: " + name);

    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    HttpUrl es_url(entity_url);
    ODataUrlCodec::ensureJsonFormat(es_url);
    auto odata_client = std::make_shared<ODataEntitySetClient>(http_client, es_url, auth_params);
    odata_client->SetMetadataContextUrl(metadata_url);
    odata_client->SetODataVersionDirectly(ODataVersion::V4);

    auto odata_bind = ODataReadBindData::FromEntitySetClient(odata_client);
    // Pre-warm column name/type cache: UpdateUrlFromPredicatePushdown() replaces
    // odata_client with a fresh instance that loses SetMetadataContextUrl, so
    // we fetch and cache names now (before any URL rewrite) to avoid a later
    // metadata fetch against the wrong companies({id})/$metadata URL.
    odata_bind->GetResultNames();
    odata_bind->GetResultTypes();
    bind_data = std::move(odata_bind);

    duckdb::TableFunction table_function("odata_table_scan", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataReadBind), ODataReadTableInitGlobalState);
    table_function.filter_pushdown = true;
    table_function.filter_prune = true;
    table_function.projection_pushdown = true;
    table_function.table_scan_progress = ODataReadTableProgress;

    return table_function;
}

// -----------------------------------------------------------------------
// BcSchemaEntry
// -----------------------------------------------------------------------

BcSchemaEntry::BcSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info)
    : duckdb::SchemaCatalogEntry(catalog, info) {
}

void BcSchemaEntry::LoadTables() {
    auto &bc_catalog = static_cast<BcCatalog&>(catalog);
    try {
        auto metadata = bc_catalog.GetServiceClient().GetMetadata();
        auto entity_sets = metadata.FindEntitySets();

        table_entries.clear();

        for (const auto &entity_set : entity_sets) {
            duckdb::CreateTableInfo table_info;
            table_info.table = entity_set.name;
            table_info.schema = name;

            try {
                auto type_variant = metadata.FindType(entity_set.entity_type_name);
                if (std::holds_alternative<EntityType>(type_variant)) {
                    auto entity_type = std::get<EntityType>(type_variant);
                    for (const auto &property : entity_type.properties) {
                        auto logical_type = DuckTypeConverter::BuildLogicalTypeForProperty(property, metadata);
                        table_info.columns.AddColumn(duckdb::ColumnDefinition(property.name, logical_type));
                    }
                } else {
                    table_info.columns.AddColumn(duckdb::ColumnDefinition("id", duckdb::LogicalType::VARCHAR));
                }
            } catch (const std::exception &) {
                table_info.columns.AddColumn(duckdb::ColumnDefinition("id", duckdb::LogicalType::VARCHAR));
            }

            auto entry = duckdb::make_uniq<BcTableEntry>(catalog, *this, table_info);
            table_entries[entity_set.name] = std::move(entry);
        }

        ERPL_TRACE_INFO("BC_CATALOG", "Loaded " + std::to_string(table_entries.size()) + " BC entity sets");
    } catch (const std::exception &e) {
        // Propagate: swallowing this made an expired token or a 403 indistinguishable from a company
        // with no entity sets, and the caller then cached the empty result (GitHub #253). The callers
        // only set tables_loaded after LoadTables returns, so a failure is retried next time.
        ERPL_TRACE_ERROR("BC_CATALOG", "Failed to load BC entity sets: " + std::string(e.what()));
        throw;
    }
}

optional_ptr<CatalogEntry> BcSchemaEntry::GetEntry(CatalogTransaction transaction, CatalogType type, const string &entry_name) {
    if (type != CatalogType::TABLE_ENTRY) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(tables_mutex);
    if (!tables_loaded) {
        LoadTables();
        tables_loaded = true;
    }
    auto it = table_entries.find(entry_name);
    if (it != table_entries.end()) {
        return it->second.get();
    }
    return nullptr;
}

optional_ptr<CatalogEntry> BcSchemaEntry::LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) {
    if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
        return nullptr;
    }
    return GetEntry(transaction, CatalogType::TABLE_ENTRY, lookup_info.GetEntryName());
}

void BcSchemaEntry::Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
    if (type != CatalogType::TABLE_ENTRY) {
        return;
    }
    std::lock_guard<std::mutex> lock(tables_mutex);
    if (!tables_loaded) {
        LoadTables();
        tables_loaded = true;
    }
    for (auto &entry : table_entries) {
        callback(*entry.second);
    }
}

void BcSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
    if (type != CatalogType::TABLE_ENTRY) {
        return;
    }
    std::lock_guard<std::mutex> lock(tables_mutex);
    if (!tables_loaded) {
        LoadTables();
        tables_loaded = true;
    }
    for (auto &entry : table_entries) {
        callback(*entry.second);
    }
}

void BcSchemaEntry::GetTableInfo(const std::string &table_name,
                                  duckdb::ColumnList &columns,
                                  std::vector<duckdb::unique_ptr<duckdb::Constraint>> &constraints) {
    std::lock_guard<std::mutex> lock(tables_mutex);
    if (!tables_loaded) {
        LoadTables();
        tables_loaded = true;
    }
    auto it = table_entries.find(table_name);
    if (it != table_entries.end()) {
        auto &entry = *it->second;
        for (auto &col : entry.GetColumns().Logical()) {
            columns.AddColumn(duckdb::ColumnDefinition(col.Name(), col.Type()));
        }
    }
}

optional_ptr<CatalogEntry> BcSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
optional_ptr<CatalogEntry> BcSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
void BcSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}
void BcSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}

// -----------------------------------------------------------------------
// BcCatalog
// -----------------------------------------------------------------------

BcCatalog::BcCatalog(duckdb::AttachedDatabase &db,
                     const std::string &base_url,
                     const std::string &company_url,
                     std::shared_ptr<HttpAuthParams> auth_params)
    : duckdb::Catalog(db),
      service_client_(std::make_shared<HttpClient>(), HttpUrl(base_url), auth_params),
      company_url_(company_url),
      metadata_url_(base_url + "/$metadata"),
      path_(base_url) {

    duckdb::CreateSchemaInfo schema_info;
    schema_info.schema = "main";
    main_schema_ = duckdb::make_uniq<BcSchemaEntry>(*this, schema_info);

    ERPL_TRACE_INFO("BC_CATALOG", "BcCatalog created: company_url=" + company_url + " metadata_url=" + metadata_url_);
}

duckdb::string BcCatalog::GetCatalogType() {
    return "business_central";
}

void BcCatalog::Initialize(bool) { }
void BcCatalog::Initialize(duckdb::optional_ptr<duckdb::ClientContext>, bool) { }
void BcCatalog::FinalizeLoad(duckdb::optional_ptr<duckdb::ClientContext>) { }

duckdb::string BcCatalog::GetDBPath() {
    return path_;
}

bool BcCatalog::InMemory() {
    return true;
}

bool BcCatalog::SupportsTimeTravel() const {
    return false;
}

duckdb::string BcCatalog::GetDefaultSchema() const {
    return "main";
}

duckdb::DatabaseSize BcCatalog::GetDatabaseSize(duckdb::ClientContext &) {
    return duckdb::DatabaseSize();
}

duckdb::vector<duckdb::MetadataBlockInfo> BcCatalog::GetMetadataInfo(duckdb::ClientContext &) {
    return duckdb::vector<duckdb::MetadataBlockInfo>();
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> BcCatalog::LookupSchema(duckdb::CatalogTransaction transaction, const duckdb::EntryLookupInfo &lookup_info, duckdb::OnEntryNotFound if_not_found) {
    if (lookup_info.GetEntryName() == "main") {
        return main_schema_.get();
    }
    if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
        throw duckdb::CatalogException("Schema \"%s\" not found", lookup_info.GetEntryName());
    }
    return nullptr;
}

duckdb::optional_ptr<duckdb::CatalogEntry> BcCatalog::CreateSchema(duckdb::CatalogTransaction, duckdb::CreateSchemaInfo &) {
    throw duckdb::NotImplementedException("Business Central catalogs are read-only");
}

void BcCatalog::ScanSchemas(duckdb::ClientContext &, std::function<void(duckdb::SchemaCatalogEntry &)> callback) {
    callback(*main_schema_);
}

duckdb::optional_ptr<duckdb::SchemaCatalogEntry> BcCatalog::GetSchema(duckdb::CatalogTransaction transaction, const std::string &schema_name, duckdb::OnEntryNotFound if_not_found, duckdb::QueryErrorContext) {
    if (schema_name == "main") {
        return main_schema_.get();
    }
    if (if_not_found == duckdb::OnEntryNotFound::THROW_EXCEPTION) {
        throw duckdb::CatalogException("Schema \"%s\" not found", schema_name);
    }
    return nullptr;
}

duckdb::PhysicalOperator &BcCatalog::PlanCreateTableAs(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &, duckdb::LogicalCreateTable &, duckdb::PhysicalOperator &) {
    throw duckdb::NotImplementedException("CREATE TABLE AS is not supported on Business Central catalogs");
}
duckdb::PhysicalOperator &BcCatalog::PlanInsert(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &, duckdb::LogicalInsert &, duckdb::optional_ptr<duckdb::PhysicalOperator>) {
    throw duckdb::NotImplementedException("INSERT is not supported on Business Central catalogs");
}
duckdb::PhysicalOperator &BcCatalog::PlanDelete(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &, duckdb::LogicalDelete &, duckdb::PhysicalOperator &) {
    throw duckdb::NotImplementedException("DELETE is not supported on Business Central catalogs");
}
duckdb::PhysicalOperator &BcCatalog::PlanUpdate(duckdb::ClientContext &, duckdb::PhysicalPlanGenerator &, duckdb::LogicalUpdate &, duckdb::PhysicalOperator &) {
    throw duckdb::NotImplementedException("UPDATE is not supported on Business Central catalogs");
}
duckdb::unique_ptr<duckdb::LogicalOperator> BcCatalog::BindCreateIndex(duckdb::Binder &, duckdb::CreateStatement &, duckdb::TableCatalogEntry &, duckdb::unique_ptr<duckdb::LogicalOperator>) {
    throw duckdb::NotImplementedException("CREATE INDEX is not supported on Business Central catalogs");
}
void BcCatalog::DropSchema(duckdb::ClientContext &, duckdb::DropInfo &) {
    throw duckdb::NotImplementedException("DROP SCHEMA is not supported on Business Central catalogs");
}

std::string BcCatalog::CompanyUrl() const {
    return company_url_;
}

std::string BcCatalog::MetadataContextUrl() const {
    return metadata_url_;
}

ODataServiceClient &BcCatalog::GetServiceClient() {
    return service_client_;
}

BcSchemaEntry &BcCatalog::GetMainSchema() {
    return *main_schema_;
}

void BcCatalog::GetTableInfo(const std::string &table_name,
                              duckdb::ColumnList &columns,
                              std::vector<duckdb::unique_ptr<duckdb::Constraint>> &constraints) {
    main_schema_->GetTableInfo(table_name, columns, constraints);
}

} // namespace erpl_web
