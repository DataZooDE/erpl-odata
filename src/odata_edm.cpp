#include "odata_edm.hpp"
#include "duckdb_compat.hpp"
#include "datazoo/oauth2/http_client.hpp"

namespace erpl_web {

EdmCache& EdmCache::GetInstance() {
    static EdmCache instance;
    return instance;
}

bool EdmCache::IsExpired(const Entry& entry, std::chrono::steady_clock::time_point now) const {
    if (entry_lifetime < std::chrono::seconds::zero()) {
        return false;
    }
    return (now - entry.stored_at) >= entry_lifetime;
}

void EdmCache::EvictExpired(std::chrono::steady_clock::time_point now) {
    if (entry_lifetime < std::chrono::seconds::zero()) {
        return;
    }
    for (auto it = cache.begin(); it != cache.end();) {
        it = IsExpired(it->second, now) ? cache.erase(it) : std::next(it);
    }
}

std::shared_ptr<const Edmx> EdmCache::Get(const std::string& metadata_url) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(cache_lock);

    // Sweeping on every lookup keeps the map from growing without bound in a
    // long-lived process that attaches many services. See GitHub #106.
    EvictExpired(now);

    const auto url_without_fragment = UrlWithoutFragment(metadata_url);
    const auto it = cache.find(url_without_fragment);
    if (it == cache.end()) {
        return nullptr;
    }
    // The shared_ptr copy leaves the lock with the caller; whatever happens to the map
    // slot afterwards, the document the caller reads stays alive and unchanged.
    return it->second.edmx;
}

std::shared_ptr<const Edmx> EdmCache::Set(const std::string& metadata_url, Edmx edmx) {
    auto snapshot = std::make_shared<const Edmx>(std::move(edmx));
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(cache_lock);
    EvictExpired(now);
    cache[UrlWithoutFragment(metadata_url)] = Entry{snapshot, now};
    return snapshot;
}

void EdmCache::Invalidate(const std::string& metadata_url) {
    std::lock_guard<std::mutex> lock(cache_lock);
    cache.erase(UrlWithoutFragment(metadata_url));
}

void EdmCache::Clear() {
    std::lock_guard<std::mutex> lock(cache_lock);
    cache.clear();
}

size_t EdmCache::Size() const {
    std::lock_guard<std::mutex> lock(cache_lock);
    return cache.size();
}

void EdmCache::SetEntryLifetime(std::chrono::seconds lifetime) {
    std::lock_guard<std::mutex> lock(cache_lock);
    entry_lifetime = lifetime;
}

std::chrono::seconds EdmCache::GetEntryLifetime() const {
    std::lock_guard<std::mutex> lock(cache_lock);
    return entry_lifetime;
}

std::string EdmCache::UrlWithoutFragment(const std::string& url_str) const {
    std::stringstream ss;
    auto url = HttpUrl(url_str);
    ss << url.ToSchemeHostAndPort() << url.ToPathQuery();
    return ss.str();
}

// -----------------------------------------------------------------------------
// The one EDM primitive type table
// -----------------------------------------------------------------------------
namespace {

struct EdmPrimitiveMapping {
    const char *edm_type_name;
    duckdb::LogicalTypeId logical_type_id;
    const char *duckdb_type_name;
};

// Edm.Byte is UNSIGNED (0..255); Edm.SByte is the signed one (-128..127). Mapping both
// to TINYINT silently NULLed 128..255 (GitHub #68). Edm.Decimal appears here without
// precision or scale; property-aware callers go through BuildDecimalLogicalType instead.
const EdmPrimitiveMapping EDM_PRIMITIVE_MAPPINGS[] = {
    {"Edm.Binary",         duckdb::LogicalTypeId::BLOB,      "BLOB"},
    {"Edm.Boolean",        duckdb::LogicalTypeId::BOOLEAN,   "BOOLEAN"},
    {"Edm.Byte",           duckdb::LogicalTypeId::UTINYINT,  "UTINYINT"},
    {"Edm.SByte",          duckdb::LogicalTypeId::TINYINT,   "TINYINT"},
    {"Edm.Date",           duckdb::LogicalTypeId::DATE,      "DATE"},
    {"Edm.DateTime",       duckdb::LogicalTypeId::TIMESTAMP, "TIMESTAMP"},
    {"Edm.DateTimeOffset", duckdb::LogicalTypeId::TIMESTAMP, "TIMESTAMP"},
    {"Edm.Decimal",        duckdb::LogicalTypeId::DECIMAL,   "DECIMAL"},
    {"Edm.Double",         duckdb::LogicalTypeId::DOUBLE,    "DOUBLE"},
    {"Edm.Duration",       duckdb::LogicalTypeId::INTERVAL,  "INTERVAL"},
    {"Edm.Guid",           duckdb::LogicalTypeId::VARCHAR,   "VARCHAR"},
    {"Edm.Int16",          duckdb::LogicalTypeId::SMALLINT,  "SMALLINT"},
    {"Edm.Int32",          duckdb::LogicalTypeId::INTEGER,   "INTEGER"},
    {"Edm.Int64",          duckdb::LogicalTypeId::BIGINT,    "BIGINT"},
    {"Edm.Single",         duckdb::LogicalTypeId::FLOAT,     "FLOAT"},
    {"Edm.Stream",         duckdb::LogicalTypeId::BLOB,      "BLOB"},
    {"Edm.String",         duckdb::LogicalTypeId::VARCHAR,   "VARCHAR"},
    {"Edm.Time",           duckdb::LogicalTypeId::TIME,      "TIME"},
    {"Edm.TimeOfDay",      duckdb::LogicalTypeId::TIME,      "TIME"},
};

const EdmPrimitiveMapping *FindEdmPrimitiveMapping(const std::string &type_name) {
    for (const auto &mapping : EDM_PRIMITIVE_MAPPINGS) {
        if (type_name == mapping.edm_type_name) {
            return &mapping;
        }
    }
    return nullptr;
}

// Geography/Geometry are surfaced as VARCHAR for now; the family is recognised by prefix
// because CSDL spells out a dozen of them.
bool IsEdmSpatialType(const std::string &type_name) {
    return type_name.rfind("Edm.Geography", 0) == 0 || type_name.rfind("Edm.Geometry", 0) == 0;
}

} // namespace

duckdb::LogicalType DuckTypeConverter::ConvertEdmPrimitiveStringToLogicalType(const std::string &type_name) {
    if (const auto *mapping = FindEdmPrimitiveMapping(type_name)) {
        return duckdb::LogicalType(mapping->logical_type_id);
    }
    // Unknown types, spatial ones included, fall back to VARCHAR.
    return duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
}

std::string DuckTypeConverter::ConvertEdmTypeStringToDuckDbTypeString(const std::string &edm_type) {
    if (const auto *mapping = FindEdmPrimitiveMapping(edm_type)) {
        return mapping->duckdb_type_name;
    }
    return "VARCHAR";
}

duckdb::LogicalType DuckTypeConverter::operator()(PrimitiveType &type) const {
    // Edm.GeographyPoint is the one spatial type this path models structurally, as a pair
    // of doubles. The string-keyed mapping above still reports VARCHAR for it, which is a
    // pre-existing inconsistency between the two entry points and not one this change
    // introduces; unifying them would change catalog column types.
    if (type == erpl_web::GeographyPoint) {
        return duckdb::LogicalType::LIST(duckdb::LogicalType(duckdb::LogicalTypeId::DOUBLE));
    }
    if (IsEdmSpatialType(type.name)) {
        return duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR);
    }
    return ConvertEdmPrimitiveStringToLogicalType(type.name);
}

// Version-specific parsing methods
Edmx Edmx::FromXmlV2(const std::string& xml) {
    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError result = doc.Parse(xml.c_str());
    if (result != tinyxml2::XML_SUCCESS) {
        std::stringstream ss;
        ss << "Failed to parse XML [" << tinyxml2::XMLDocument::ErrorIDToName(result) << "]" << std::endl;   
        ss << "Description: " << doc.ErrorStr() << std::endl;
        ss << "Content: " << std::endl << xml << std::endl;
        throw std::runtime_error(ss.str());
    }
    return FromXmlV2(doc);
}

Edmx Edmx::FromXmlV4(const std::string& xml) {
    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError result = doc.Parse(xml.c_str());
    if (result != tinyxml2::XML_SUCCESS) {
        std::stringstream ss;
        ss << "Failed to parse XML [" << tinyxml2::XMLDocument::ErrorIDToName(result) << "]" << std::endl;   
        ss << "Description: " << doc.ErrorStr() << std::endl;
        ss << "Content: " << std::endl << xml << std::endl;
        throw std::runtime_error(ss.str());
    }
    return FromXmlV4(doc);
}

Edmx Edmx::FromXmlV2(const tinyxml2::XMLDocument& doc) {
    Edmx edmx;
    edmx.SetVersion(ODataVersion::V2);
    
    const tinyxml2::XMLElement* edmx_el = doc.RootElement();
    if (edmx_el == nullptr) {
        throw std::runtime_error("Missing Edmx root element");
    }

    // Parse Edmx version
    const char* version_attr = edmx_el->Attribute("Version");
    if (version_attr) {
        edmx.version = version_attr;
    }

    // Parse DataServices element (v2 namespace)
    const tinyxml2::XMLElement* data_svc_el = edmx_el->FirstChildElement("edmx:DataServices");
    if (data_svc_el) {
        edmx.data_services = DataServices::FromXml(*data_svc_el);
    } else {
        // Try without namespace prefix (some v2 services don't use it)
        data_svc_el = edmx_el->FirstChildElement("DataServices");
        if (data_svc_el) {
            edmx.data_services = DataServices::FromXml(*data_svc_el);
        }
    }

    // Parse Reference elements
    for (const tinyxml2::XMLElement* ref_el = edmx_el->FirstChildElement("Reference");
        ref_el != nullptr;
        ref_el = ref_el->NextSiblingElement("Reference")) 
    {
        edmx.references.push_back(Reference::FromXml(*ref_el));
    }

    return edmx;
}

Edmx Edmx::FromXmlV4(const tinyxml2::XMLDocument& doc) {
    Edmx edmx;
    edmx.SetVersion(ODataVersion::V4);
    
    const tinyxml2::XMLElement* edmx_el = doc.RootElement();
    if (edmx_el == nullptr) {
        throw std::runtime_error("Missing Edmx root element");
    }

    // Parse Edmx version
    const char* version_attr = edmx_el->Attribute("Version");
    if (version_attr) {
        edmx.version = version_attr;
    }

    // Parse DataServices element (v4 namespace)
    const tinyxml2::XMLElement* data_svc_el = edmx_el->FirstChildElement("edmx:DataServices");
    if (data_svc_el) {
        edmx.data_services = DataServices::FromXml(*data_svc_el);
    }

    // Parse Reference elements
    for (const tinyxml2::XMLElement* ref_el = edmx_el->FirstChildElement("Reference");
        ref_el != nullptr;
        ref_el = ref_el->NextSiblingElement("Reference")) 
    {
        edmx.references.push_back(Reference::FromXml(*ref_el));
    }

    return edmx;
}

} // namespace erpl_web
// -----------------------------------------------------------------------------
// ODataEdmTypeBuilder implementation
// -----------------------------------------------------------------------------
namespace erpl_web {

std::pair<bool, std::string> ODataEdmTypeBuilder::ResolveNavTargetOnEntity(const std::string &entity_type_name, const std::string &nav_prop) const {
    try {
        auto tv = edmx.FindType(entity_type_name);
        if (!std::holds_alternative<EntityType>(tv)) return {false, std::string()};
        const auto &entity = std::get<EntityType>(tv);
        for (const auto &np : entity.navigation_properties) {
            if (np.name == nav_prop) {
                auto [is_collection, type_name] = converter.ExtractCollectionType(np.type);
                return {is_collection, type_name};
            }
        }
    } catch (...) {}
    return {false, std::string()};
}

duckdb::LogicalType ODataEdmTypeBuilder::BuildEntityStruct(const std::string &entity_type_name) const {
    try {
        auto tv = edmx.FindType(entity_type_name);
        if (!std::holds_alternative<EntityType>(tv)) return duckdb::LogicalType::VARCHAR;
        const auto &entity = std::get<EntityType>(tv);
        return converter(entity);
    } catch (...) {
        return duckdb::LogicalType::VARCHAR;
    }
}

duckdb::LogicalType ODataEdmTypeBuilder::BuildExpandedColumnType(const std::string &root_entity_type_name,
                                                                 const std::string &top_nav_prop,
                                                                 const std::vector<std::string> &nested_children) const {
    // Resolve target of the top navigation property
    auto [is_collection, target_type_name] = ResolveNavTargetOnEntity(root_entity_type_name, top_nav_prop);
    if (target_type_name.empty()) {
        // Fallback aligns with previous behavior
        return is_collection ? duckdb::LogicalType::LIST(duckdb::LogicalType::VARCHAR) : duckdb::LogicalType::VARCHAR;
    }

    // Base STRUCT for the target entity (properties only)
    auto base_struct = BuildEntityStruct(target_type_name);
    if (base_struct.id() != duckdb::LogicalTypeId::STRUCT) {
        base_struct = duckdb::LogicalType::VARCHAR;
    }

    // Augment with nested child navigation properties
    if (!nested_children.empty() && base_struct.id() == duckdb::LogicalTypeId::STRUCT) {
        auto children = duckdb::StructType::GetChildTypes(base_struct);
        std::set<std::string> seen;
        for (const auto &p : children) seen.insert(duckdb_compat::Str(p.first));
        for (const auto &child_nav : nested_children) {
            if (seen.count(child_nav)) continue;
            auto [child_is_coll, child_target] = ResolveNavTargetOnEntity(target_type_name, child_nav);
            duckdb::LogicalType child_type = duckdb::LogicalType::VARCHAR;
            if (!child_target.empty()) {
                auto child_struct = BuildEntityStruct(child_target);
                child_type = child_is_coll ? duckdb::LogicalType::LIST(child_struct) : child_struct;
            } else {
                child_type = duckdb::LogicalType::LIST(duckdb::LogicalType::VARCHAR);
            }
            children.emplace_back(child_nav, child_type);
            seen.insert(child_nav);
        }
        base_struct = duckdb::LogicalType::STRUCT(children);
    }

    // If the top nav itself is a collection, wrap result in LIST
    if (is_collection) {
        return duckdb::LogicalType::LIST(base_struct);
    }
    return base_struct;
}

} // namespace erpl_web