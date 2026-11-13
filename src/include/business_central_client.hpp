#pragma once

#include "odata_client.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include <string>
#include <memory>
#include <vector>

namespace erpl_web {

struct BusinessCentralCompany {
    std::string id;
    std::string name;
};

// URL builder for Business Central endpoints
class BusinessCentralUrlBuilder {
public:
    // Base API URL for BC API v2.0
    static std::string BuildApiUrl(const std::string &tenant_id, const std::string &environment);

    // Company-scoped URL
    static std::string BuildCompanyUrl(const std::string &base_url, const std::string &company_id);

    // Entity set URL within a company
    static std::string BuildEntitySetUrl(const std::string &company_url, const std::string &entity_set);

    // Metadata URL for $metadata
    static std::string BuildMetadataUrl(const std::string &base_url);

    // Companies endpoint URL
    static std::string BuildCompaniesUrl(const std::string &base_url);

    // Resource URL for OAuth2 scope
    static std::string GetResourceUrl();
};

// Client factory for Business Central OData clients
// Reuses existing OData infrastructure
class BusinessCentralClientFactory {
public:
    // Create client for listing companies
    static std::shared_ptr<ODataEntitySetClient> CreateCompaniesClient(
        const std::string &tenant_id,
        const std::string &environment,
        std::shared_ptr<HttpAuthParams> auth_params);

    // Create client for entity set within a company
    static std::shared_ptr<ODataEntitySetClient> CreateEntitySetClient(
        const std::string &tenant_id,
        const std::string &environment,
        const std::string &company_id,
        const std::string &entity_set,
        std::shared_ptr<HttpAuthParams> auth_params);

    // Create catalog client for $metadata
    static std::shared_ptr<ODataServiceClient> CreateCatalogClient(
        const std::string &tenant_id,
        const std::string &environment,
        std::shared_ptr<HttpAuthParams> auth_params);

    // Discover companies through the same OData endpoint used by bc_show_companies().
    static std::vector<BusinessCentralCompany> DiscoverCompanies(
        const std::string &tenant_id,
        const std::string &environment,
        std::shared_ptr<HttpAuthParams> auth_params);

    // Returns true if the string looks like a BC company GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    static bool LooksLikeCompanyId(const std::string &value);

    // If value is already a GUID, return it as-is. Otherwise discover companies
    // and resolve by name (case-sensitive match on the 'name' field).
    // Throws InvalidInputException when no matching company is found.
    static std::string ResolveCompanyId(
        const std::string &name_or_id,
        const std::string &tenant_id,
        const std::string &environment,
        std::shared_ptr<HttpAuthParams> auth_params);
};

// Call ONLY from inside a catch block. If the exception being handled carries an HTTP
// status, throws an IOException that names the status, Business Central's error code, the
// environment and company, and what to check (GitHub #277). Otherwise rethrows it unchanged.
[[noreturn]] void RethrowWithBusinessCentralContext(const std::string &environment,
                                                   const std::string &company);

} // namespace erpl_web
