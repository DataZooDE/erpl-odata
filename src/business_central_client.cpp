#include "business_central_client.hpp"
#include "business_central_errors.hpp"
#include "duckdb/common/error_data.hpp"
#include "odata_url_helpers.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include <utility>

namespace erpl_web {

namespace {

static std::string ValueToString(const duckdb::Value &value) {
    return value.IsNull() ? std::string() : value.ToString();
}

} // namespace

// URL Builder implementation
std::string BusinessCentralUrlBuilder::BuildApiUrl(const std::string &tenant_id, const std::string &environment) {
    // An `environment` that is already a URL is used as the API base verbatim. Without
    // this the host is hardcoded, so bc_read cannot be pointed at a local server and its
    // behaviour cannot be tested at all - which is how it kept a per-execution-state
    // defect nobody could reproduce (GitHub #182). DatasphereReadRelational has had the
    // same escape hatch on space_id for as long as it has existed.
    // Shared predicate: the three hatches must agree on what counts as "already a URL",
    // or the comment claiming one shared rule is false. A case-sensitive trigger also let
    // an uppercase scheme skip the hatch entirely and get embedded into the real endpoint
    // as an environment segment (GitHub #193).
    if (LooksLikeAbsoluteHttpUrl(environment)) {
        std::string base = environment;
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        // Shared with Datasphere's space_id and Dataverse's environment_url hatches, so the
        // loopback rule - and the IPv6 and prefix subtleties in it - has one implementation.
        RequireGatedServiceUrl(base, "Business Central 'environment'");
        return base;
    }
    return "https://api.businesscentral.dynamics.com/v2.0/" + tenant_id + "/" + environment + "/api/v2.0";
}

std::string BusinessCentralUrlBuilder::BuildCompanyUrl(const std::string &base_url, const std::string &company_id) {
    return base_url + "/companies(" + company_id + ")";
}

std::string BusinessCentralUrlBuilder::BuildEntitySetUrl(const std::string &company_url, const std::string &entity_set) {
    return company_url + "/" + entity_set;
}

std::string BusinessCentralUrlBuilder::BuildMetadataUrl(const std::string &base_url) {
    return base_url + "/$metadata";
}

std::string BusinessCentralUrlBuilder::BuildCompaniesUrl(const std::string &base_url) {
    return base_url + "/companies";
}

std::string BusinessCentralUrlBuilder::GetResourceUrl() {
    return "https://api.businesscentral.dynamics.com";
}

// Client Factory implementation
std::shared_ptr<ODataEntitySetClient> BusinessCentralClientFactory::CreateCompaniesClient(
    const std::string &tenant_id,
    const std::string &environment,
    std::shared_ptr<HttpAuthParams> auth_params) {

    ERPL_TRACE_DEBUG("BC_CLIENT", "Creating companies client for tenant: " + tenant_id + ", environment: " + environment);

    std::string base_url = BusinessCentralUrlBuilder::BuildApiUrl(tenant_id, environment);
    std::string companies_url = BusinessCentralUrlBuilder::BuildCompaniesUrl(base_url);

    auto http_client = CreateODataHttpClient();
    HttpUrl url(companies_url);

    auto client = std::make_shared<ODataEntitySetClient>(http_client, url, auth_params);
    client->SetODataVersionDirectly(ODataVersion::V4);

    ERPL_TRACE_INFO("BC_CLIENT", "Created companies client with URL: " + companies_url);
    return client;
}

std::shared_ptr<ODataEntitySetClient> BusinessCentralClientFactory::CreateEntitySetClient(
    const std::string &tenant_id,
    const std::string &environment,
    const std::string &company_id,
    const std::string &entity_set,
    std::shared_ptr<HttpAuthParams> auth_params) {

    ERPL_TRACE_DEBUG("BC_CLIENT", "Creating entity set client for: " + entity_set + " in company: " + company_id);

    std::string base_url = BusinessCentralUrlBuilder::BuildApiUrl(tenant_id, environment);
    std::string company_url = BusinessCentralUrlBuilder::BuildCompanyUrl(base_url, company_id);
    std::string entity_set_url = BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, entity_set);

    auto http_client = CreateODataHttpClient();
    HttpUrl url(entity_set_url);

    auto client = std::make_shared<ODataEntitySetClient>(http_client, url, auth_params);
    client->SetODataVersionDirectly(ODataVersion::V4);

    ERPL_TRACE_INFO("BC_CLIENT", "Created entity set client with URL: " + entity_set_url);
    return client;
}

std::shared_ptr<ODataServiceClient> BusinessCentralClientFactory::CreateCatalogClient(
    const std::string &tenant_id,
    const std::string &environment,
    std::shared_ptr<HttpAuthParams> auth_params) {

    ERPL_TRACE_DEBUG("BC_CLIENT", "Creating catalog client for tenant: " + tenant_id);

    std::string base_url = BusinessCentralUrlBuilder::BuildApiUrl(tenant_id, environment);

    auto http_client = CreateODataHttpClient();
    HttpUrl url(base_url);

    auto client = std::make_shared<ODataServiceClient>(http_client, url, auth_params);

    ERPL_TRACE_INFO("BC_CLIENT", "Created catalog client with URL: " + base_url);
    return client;
}

std::vector<BusinessCentralCompany> BusinessCentralClientFactory::DiscoverCompanies(
    const std::string &tenant_id,
    const std::string &environment,
    std::shared_ptr<HttpAuthParams> auth_params) {

    ERPL_TRACE_DEBUG("BC_CLIENT", "Discovering Business Central companies");

    auto client = CreateCompaniesClient(tenant_id, environment, std::move(auth_params));
    std::vector<std::string> column_names = {"id", "name"};
    std::vector<duckdb::LogicalType> column_types = {
        duckdb::LogicalType::VARCHAR,
        duckdb::LogicalType::VARCHAR
    };

    std::vector<BusinessCentralCompany> companies;
    for (auto response = client->Get(); response; response = client->Get(true)) {
        auto rows = response->ToRows(column_names, column_types);
        for (const auto &row : rows) {
            if (row.size() < 2) {
                continue;
            }
            BusinessCentralCompany company;
            company.id = ValueToString(row[0]);
            company.name = ValueToString(row[1]);
            if (!company.id.empty()) {
                companies.push_back(std::move(company));
            }
        }

        if (!response->NextUrl()) {
            break;
        }
    }

    ERPL_TRACE_INFO("BC_CLIENT", "Discovered " + std::to_string(companies.size()) + " Business Central companies");
    return companies;
}

bool BusinessCentralClientFactory::LooksLikeCompanyId(const std::string &value) {
    // BC company GUIDs follow the canonical 8-4-4-4-12 format (36 chars total)
    if (value.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') {
                return false;
            }
        } else {
            char c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
    }
    return true;
}

std::string BusinessCentralClientFactory::ResolveCompanyId(
    const std::string &name_or_id,
    const std::string &tenant_id,
    const std::string &environment,
    std::shared_ptr<HttpAuthParams> auth_params) {

    if (LooksLikeCompanyId(name_or_id)) {
        return name_or_id;
    }

    ERPL_TRACE_DEBUG("BC_CLIENT", "Resolving company name '" + name_or_id + "' to ID");

    for (const auto &company : DiscoverCompanies(tenant_id, environment, std::move(auth_params))) {
        if (company.name == name_or_id) {
            ERPL_TRACE_DEBUG("BC_CLIENT", "Resolved company '" + name_or_id + "' -> " + company.id);
            return company.id;
        }
    }

    throw duckdb::InvalidInputException(
        "No company found with name '%s'. Use bc_show_companies() to list available companies, "
        "or pass the company GUID directly.", name_or_id.c_str());
}

void RethrowWithBusinessCentralContext(const std::string &environment, const std::string &company) {
    try {
        throw;
    } catch (const std::exception &e) {
        const auto raw = duckdb::ErrorData(e).RawMessage();
        if (raw.rfind(BUSINESS_CENTRAL_ERROR_PREFIX, 0) == 0) {
            throw;  // already described
        }
        const auto failure = ParseBusinessCentralHttpFailure(raw);
        if (failure.status < 300) {
            throw;  // not an HTTP failure: nothing to add
        }
        throw duckdb::IOException(DescribeBusinessCentralFailure(failure, environment, company));
    }
}

} // namespace erpl_web
