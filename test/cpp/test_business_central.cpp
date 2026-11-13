#include "catch.hpp"
#include "business_central_client.hpp"
#include "business_central_errors.hpp"

using namespace erpl_web;

// =============================================================================
// URL Builder Tests
// =============================================================================

TEST_CASE("Business Central URL Builder - API URL generation", "[business_central][url]") {
    SECTION("Standard API URL generation") {
        auto url = BusinessCentralUrlBuilder::BuildApiUrl("test-tenant-123", "sandbox");
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/test-tenant-123/sandbox/api/v2.0");
    }

    SECTION("Production environment") {
        auto url = BusinessCentralUrlBuilder::BuildApiUrl("prod-tenant-456", "production");
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/prod-tenant-456/production/api/v2.0");
    }

    SECTION("Empty tenant ID") {
        auto url = BusinessCentralUrlBuilder::BuildApiUrl("", "sandbox");
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0//sandbox/api/v2.0");
    }
}

TEST_CASE("Business Central URL Builder - Company URL generation", "[business_central][url]") {
    std::string base_url = "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0";

    SECTION("Standard company URL with GUID") {
        auto url = BusinessCentralUrlBuilder::BuildCompanyUrl(base_url, "12345678-abcd-1234-abcd-123456789012");
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(12345678-abcd-1234-abcd-123456789012)");
    }

    SECTION("Company URL with simple ID") {
        auto url = BusinessCentralUrlBuilder::BuildCompanyUrl(base_url, "my-company");
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(my-company)");
    }
}

TEST_CASE("Business Central URL Builder - Entity Set URL generation", "[business_central][url]") {
    std::string company_url = "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(company-id)";

    SECTION("Standard entity set URL") {
        auto url = BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, "customers");
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(company-id)/customers");
    }

    SECTION("Different entity sets") {
        REQUIRE(BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, "vendors") ==
                "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(company-id)/vendors");

        REQUIRE(BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, "items") ==
                "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(company-id)/items");

        REQUIRE(BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, "salesOrders") ==
                "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies(company-id)/salesOrders");
    }
}

TEST_CASE("Business Central URL Builder - Metadata URL generation", "[business_central][url]") {
    std::string base_url = "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0";

    SECTION("Standard metadata URL") {
        auto url = BusinessCentralUrlBuilder::BuildMetadataUrl(base_url);
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/$metadata");
    }
}

TEST_CASE("Business Central URL Builder - Companies URL generation", "[business_central][url]") {
    std::string base_url = "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0";

    SECTION("Standard companies URL") {
        auto url = BusinessCentralUrlBuilder::BuildCompaniesUrl(base_url);
        REQUIRE(url == "https://api.businesscentral.dynamics.com/v2.0/tenant/sandbox/api/v2.0/companies");
    }
}

TEST_CASE("Business Central URL Builder - Resource URL", "[business_central][url]") {
    SECTION("Resource URL for OAuth2 scope") {
        auto url = BusinessCentralUrlBuilder::GetResourceUrl();
        REQUIRE(url == "https://api.businesscentral.dynamics.com");
    }
}

TEST_CASE("Business Central company ID detection", "[business_central][company]") {
    REQUIRE(BusinessCentralClientFactory::LooksLikeCompanyId("12345678-abcd-1234-abcd-123456789012"));
    REQUIRE(BusinessCentralClientFactory::LooksLikeCompanyId("12345678-ABCD-1234-ABCD-123456789012"));
    REQUIRE_FALSE(BusinessCentralClientFactory::LooksLikeCompanyId("12345678abcd1234abcd123456789012"));
    REQUIRE_FALSE(BusinessCentralClientFactory::LooksLikeCompanyId("12345678_abcd-1234-abcd-123456789012"));
    REQUIRE_FALSE(BusinessCentralClientFactory::LooksLikeCompanyId("12345678-abcd-1234-abcd-12345678901z"));
}

// =============================================================================
// Integration URL Flow Tests
// =============================================================================

TEST_CASE("Business Central URL Builder - Full URL chain", "[business_central][url][integration]") {
    std::string tenant_id = "contoso-tenant";
    std::string environment = "production";
    std::string company_id = "company-guid-12345";
    std::string entity_set = "customers";

    SECTION("Build complete URL chain") {
        auto base_url = BusinessCentralUrlBuilder::BuildApiUrl(tenant_id, environment);
        REQUIRE(base_url == "https://api.businesscentral.dynamics.com/v2.0/contoso-tenant/production/api/v2.0");

        auto companies_url = BusinessCentralUrlBuilder::BuildCompaniesUrl(base_url);
        REQUIRE(companies_url == "https://api.businesscentral.dynamics.com/v2.0/contoso-tenant/production/api/v2.0/companies");

        auto company_url = BusinessCentralUrlBuilder::BuildCompanyUrl(base_url, company_id);
        REQUIRE(company_url == "https://api.businesscentral.dynamics.com/v2.0/contoso-tenant/production/api/v2.0/companies(company-guid-12345)");

        auto entity_url = BusinessCentralUrlBuilder::BuildEntitySetUrl(company_url, entity_set);
        REQUIRE(entity_url == "https://api.businesscentral.dynamics.com/v2.0/contoso-tenant/production/api/v2.0/companies(company-guid-12345)/customers");

        auto metadata_url = BusinessCentralUrlBuilder::BuildMetadataUrl(base_url);
        REQUIRE(metadata_url == "https://api.businesscentral.dynamics.com/v2.0/contoso-tenant/production/api/v2.0/$metadata");
    }
}

// =============================================================================
// Failure reporting (GitHub #277). The bodies are the ones Business Central really
// returned for an environment that does not exist, captured 2026-11-13.
// =============================================================================

TEST_CASE("Business Central failure - $metadata 404 NoEnvironment (XML body)", "[business_central][errors]") {
    const std::string text =
        "Cannot determine the columns of 'https://api.businesscentral.dynamics.com/v2.0/t/Production/api/v2.0/companies': "
        "its $metadata could not be read, and there is no data row to take them from. "
        "Failed to get OData metadata from https://api.businesscentral.dynamics.com/v2.0/t/Production/api/v2.0/$metadata (HTTP 404)\n"
        "Response: <error xmlns=\"http://docs.oasis-open.org/odata/ns/metadata\"><code>NoEnvironment</code>"
        "<message>Environment does not exist.</message></error>";

    auto failure = ParseBusinessCentralHttpFailure(text);
    CHECK(failure.status == 404);
    CHECK(failure.code == "NoEnvironment");
    CHECK(failure.message == "Environment does not exist.");

    auto message = DescribeBusinessCentralFailure(failure, "Production", "");
    CHECK(message.rfind(BUSINESS_CENTRAL_ERROR_PREFIX, 0) == 0);
    CHECK(message.find("environment 'Production' does not exist") != std::string::npos);
    CHECK(message.find("HTTP 404, NoEnvironment: Environment does not exist.") != std::string::npos);
    CHECK(message.find("ENVIRONMENT of the business_central secret") != std::string::npos);
}

TEST_CASE("Business Central failure - data request 404 NoEnvironment (JSON body)", "[business_central][errors]") {
    const std::string text =
        "Failed to get OData response from https://api.businesscentral.dynamics.com/v2.0/t/Production/api/v2.0/companies: HTTP 404\n"
        "Content: \n{\"error\": {\"code\": \"NoEnvironment\",\"message\": \"Environment does not exist.\"}}\n";

    auto failure = ParseBusinessCentralHttpFailure(text);
    CHECK(failure.status == 404);
    CHECK(failure.code == "NoEnvironment");
    CHECK(failure.message == "Environment does not exist.");
    CHECK(DescribeBusinessCentralFailure(failure, "Production", "CRONUS").find("'Production' does not exist") !=
          std::string::npos);
}

TEST_CASE("Business Central failure - status-specific guidance", "[business_central][errors]") {
    BusinessCentralHttpFailure failure;

    failure.status = 401;
    auto unauthorized = DescribeBusinessCentralFailure(failure, "sandbox", "");
    CHECK(unauthorized.find("HTTP 401") != std::string::npos);
    CHECK(unauthorized.find("environment 'sandbox'") != std::string::npos);
    CHECK(unauthorized.find("CLIENT_SECRET") != std::string::npos);

    failure.status = 403;
    CHECK(DescribeBusinessCentralFailure(failure, "sandbox", "").find("permission sets") != std::string::npos);

    failure.status = 404;
    auto not_found = DescribeBusinessCentralFailure(failure, "sandbox", "CRONUS DE");
    CHECK(not_found.find("company 'CRONUS DE'") != std::string::npos);
    CHECK(not_found.find("bc_show_companies()") != std::string::npos);

    failure.status = 503;
    CHECK(DescribeBusinessCentralFailure(failure, "sandbox", "").find("HTTP 503") != std::string::npos);
}

TEST_CASE("Business Central failure - text without an HTTP status", "[business_central][errors]") {
    auto failure = ParseBusinessCentralHttpFailure("No company found with name 'X'. Use bc_show_companies()");
    CHECK(failure.status == 0);
    CHECK(failure.code.empty());
    // "HTTP GET to" is not a status.
    CHECK(ParseBusinessCentralHttpFailure("Failed to read connection error for HTTP GET to 'x'").status == 0);
}
