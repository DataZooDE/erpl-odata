#pragma once

#include "odata_client.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "odata_edm.hpp"
#include "duckdb/function/table_function.hpp"
#include <memory>
#include <string>

namespace erpl_web {

// Forward declarations
class DatasphereUrlBuilder;
struct DatasphereAuthParams;

// Datasphere client factory for creating appropriate clients
class DatasphereClientFactory {
public:
    // Create client for relational data access
    static std::shared_ptr<ODataEntitySetClient> CreateRelationalClient(
        const std::string& tenant, 
        const std::string& data_center,
        const std::string& space_id, 
        const std::string& asset_id,
        std::shared_ptr<HttpAuthParams> auth_params);
    
    // Create client for analytical data access
    static std::shared_ptr<ODataEntitySetClient> CreateAnalyticalClient(
        const std::string& tenant, 
        const std::string& data_center,
        const std::string& space_id, 
        const std::string& asset_id,
        std::shared_ptr<HttpAuthParams> auth_params);
    
    // Create client for catalog service
    static std::shared_ptr<ODataServiceClient> CreateCatalogClient(
        const std::string& tenant,
        const std::string& data_center,
        std::shared_ptr<HttpAuthParams> auth_params);
};

// URL builder for Datasphere-specific URL patterns
class DatasphereUrlBuilder {
public:
    // Existing methods
    static std::string BuildCatalogUrl(const std::string& tenant, const std::string& data_center);
    static std::string BuildRelationalUrl(const std::string& tenant, const std::string& data_center, 
                                        const std::string& space_id, const std::string& asset_id);
    static std::string BuildAnalyticalUrl(const std::string& tenant, const std::string& data_center, 
                                        const std::string& space_id, const std::string& asset_id);
    
    // New methods for DWAAS core API
    static std::string BuildDwaasCoreUrl(const std::string& tenant_name, 
                                        const std::string& data_center, 
                                        const std::string& endpoint);
    static std::string BuildDwaasCoreSpacesUrl(const std::string& tenant_name, 
                                              const std::string& data_center);
                        static std::string BuildDwaasCoreSpaceObjectsUrl(const std::string& tenant_name,
                                                                    const std::string& data_center,
                                                                    const std::string& space_id,
                                                                    const std::string& object_type);
                    static std::string BuildDwaasCoreObjectUrl(const std::string& tenant_name,
                                                              const std::string& data_center,
                                                              const std::string& space_id,
                                                              const std::string& object_type,
                                                              const std::string& object_id);

                    // New methods for catalog endpoints
    static std::string BuildCatalogSpacesUrl(const std::string& tenant_name, 
                                            const std::string& data_center);
    static std::string BuildCatalogAssetsUrl(const std::string& tenant_name, 
                                            const std::string& data_center);
    static std::string BuildCatalogAssetsFilteredUrl(const std::string& tenant_name, 
                                                    const std::string& data_center, 
                                                    const std::string& space_id);
    static std::string BuildCatalogAssetFilteredUrl(const std::string& tenant_name, 
                                                   const std::string& data_center, 
                                                   const std::string& space_id, 
                                                   const std::string& asset_id);
    static std::string BuildSpaceFilteredUrl(const std::string& tenant_name, 
                                            const std::string& data_center, 
                                            const std::string& space_id);
    
    // URL patterns:
    // Catalog: https://{tenant}.{data_center}.hcs.cloud.sap/api/v1/dwc/catalog
    // Relational: https://{tenant}.{data_center}.hcs.cloud.sap/api/v1/dwc/consumption/relational/{space_id}/{asset_id}
    // Analytical: https://{tenant}.{data_center}.hcs.cloud.sap/api/v1/dwc/consumption/analytical/{space_id}/{asset_id}
    // DWAAS Core: https://{tenant}.{data_center}.hcs.cloud.sap/dwaas-core/api/v1
};

// Enhanced authentication parameters for Datasphere OAuth2
struct DatasphereAuthParams : public HttpAuthParams {
    std::string tenant_name;
    std::string data_center;
    std::string client_id;
    std::string client_secret;
    std::string scope;
    
    // OAuth2 token management
    std::optional<std::string> access_token;
    std::optional<std::string> refresh_token;
    std::optional<std::chrono::time_point<std::chrono::system_clock>> token_expiry;
    
    bool IsTokenExpired() const;
    bool NeedsRefresh() const;
    void RefreshToken();
    
    // OAuth2 endpoints
    std::string GetAuthorizationUrl() const;
    std::string GetTokenUrl() const;
};




} // namespace erpl_web
