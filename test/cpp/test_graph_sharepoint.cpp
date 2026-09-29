#include "catch.hpp"
#include "odata_url_helpers.hpp"
#include "duckdb.hpp"
#include "graph_sharepoint_client.hpp"

using namespace erpl_web;

// =============================================================================
// GraphSharePointUrlBuilder Tests
// =============================================================================

TEST_CASE("GraphSharePointUrlBuilder - GetBaseUrl", "[graph_sharepoint][url_builder]") {
    REQUIRE(GraphSharePointUrlBuilder::GetBaseUrl() == "https://graph.microsoft.com/v1.0");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildSitesSearchUrl", "[graph_sharepoint][url_builder]") {
    // Empty search returns wildcard
    auto url1 = GraphSharePointUrlBuilder::BuildSitesSearchUrl();
    REQUIRE(url1 == "https://graph.microsoft.com/v1.0/sites?search=*");

    // With search query
    auto url2 = GraphSharePointUrlBuilder::BuildSitesSearchUrl("contoso");
    REQUIRE(url2 == "https://graph.microsoft.com/v1.0/sites?search=contoso");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildSiteUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildSiteUrl("site-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildSiteListsUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildSiteListsUrl("site-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildListUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildListUrl("site-id-123", "list-id-456");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildListColumnsUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildListColumnsUrl("site-id-123", "list-id-456");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/columns");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildListItemsUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildListItemsUrl("site-id-123", "list-id-456");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/items");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildListItemsWithFieldsUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildListItemsWithFieldsUrl("site-id-123", "list-id-456");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/items?expand=fields");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildListItemsWithSelectUrl", "[graph_sharepoint][url_builder]") {
    // With select and top
    auto url1 = GraphSharePointUrlBuilder::BuildListItemsWithSelectUrl("site-id-123", "list-id-456", "Title,Created", 100);
    REQUIRE(url1 == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/items?expand=fields&$select=Title,Created&$top=100");

    // Without select, with top
    auto url2 = GraphSharePointUrlBuilder::BuildListItemsWithSelectUrl("site-id-123", "list-id-456", "", 50);
    REQUIRE(url2 == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/items?expand=fields&$top=50");

    // Without select, without top
    auto url3 = GraphSharePointUrlBuilder::BuildListItemsWithSelectUrl("site-id-123", "list-id-456");
    REQUIRE(url3 == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/items?expand=fields");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildItemUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildItemUrl("site-id-123", "list-id-456", "item-id-789");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/lists/list-id-456/items/item-id-789");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildFollowedSitesUrl", "[graph_sharepoint][url_builder]") {
    auto url = GraphSharePointUrlBuilder::BuildFollowedSitesUrl();
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/followedSites");
}

TEST_CASE("GraphSharePointUrlBuilder - BuildSiteByPathUrl", "[graph_sharepoint][url_builder]") {
    // Without site path (root site)
    auto url1 = GraphSharePointUrlBuilder::BuildSiteByPathUrl("contoso.sharepoint.com", "");
    REQUIRE(url1 == "https://graph.microsoft.com/v1.0/sites/contoso.sharepoint.com");

    // With site path
    auto url2 = GraphSharePointUrlBuilder::BuildSiteByPathUrl("contoso.sharepoint.com", "sites/marketing");
    REQUIRE(url2 == "https://graph.microsoft.com/v1.0/sites/contoso.sharepoint.com:/sites/marketing:");

    // With leading slash (should be removed)
    auto url3 = GraphSharePointUrlBuilder::BuildSiteByPathUrl("contoso.sharepoint.com", "/sites/hr");
    REQUIRE(url3 == "https://graph.microsoft.com/v1.0/sites/contoso.sharepoint.com:/sites/hr:");
}

// =============================================================================
// DuckDB Integration Tests (Function Existence)
// =============================================================================

TEST_CASE("Microsoft Graph SharePoint Functions Exist", "[graph_sharepoint][functions]") {
    duckdb::DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    duckdb::DuckDB db(nullptr, &config);
    duckdb::Connection con(db);

    // Load the extension
    auto result = con.Query("LOAD erpl_odata");
    REQUIRE_FALSE(result->HasError());

    SECTION("graph_show_sites function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_show_sites'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_show_lists function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_show_lists'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_describe_list function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_describe_list'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_sharepoint_list_read function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_sharepoint_list_read'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }
}

TEST_CASE("Microsoft Graph SharePoint uses Graph Secret Type", "[graph_sharepoint][secret]") {
    duckdb::DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    duckdb::DuckDB db(nullptr, &config);
    duckdb::Connection con(db);

    // Load the extension
    auto result = con.Query("LOAD erpl_odata");
    REQUIRE_FALSE(result->HasError());

    SECTION("Graph secret type can be used for SharePoint") {
        result = con.Query(R"(
            CREATE SECRET test_sp_secret (
                TYPE microsoft_graph,
                tenant_id 'test-tenant-12345',
                client_id 'test-client-67890',
                client_secret 'test-secret-abcde'
            )
        )");
        REQUIRE_FALSE(result->HasError());

        // Verify secret was created
        result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 'test_sp_secret'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);

        // Cleanup
        con.Query("DROP SECRET test_sp_secret");
    }
}

// GitHub #202 review, F9: ResolveSiteId tested the scheme prefixes byte-exactly while the
// shared LooksLikeAbsoluteHttpUrl lowercases first, so an uppercase scheme fell into the
// name-search path and failed with "No SharePoint site found". One predicate now.
TEST_CASE("an absolute site URL is recognised whatever the scheme's case",
          "[graph_sharepoint][url_builder]") {
    REQUIRE(erpl_web::LooksLikeAbsoluteHttpUrl("https://tenant.sharepoint.com/sites/Finance"));
    REQUIRE(erpl_web::LooksLikeAbsoluteHttpUrl("HTTPS://tenant.sharepoint.com/sites/Finance"));
    REQUIRE(erpl_web::LooksLikeAbsoluteHttpUrl("Http://tenant.sharepoint.com"));
    REQUIRE_FALSE(erpl_web::LooksLikeAbsoluteHttpUrl("Finance"));
    REQUIRE_FALSE(erpl_web::LooksLikeAbsoluteHttpUrl("tenant.sharepoint.com/sites/Finance"));
}

// GitHub #206 follow-up: the case-insensitive scheme predicate was applied to ResolveSiteId
// and left byte-exact in four siblings. ResolveDriveIdFromUrl was the worst of them: it
// assumed "not https, therefore http" and chopped seven characters off whatever it was
// handed, so an uppercase scheme or a bare name produced a silently wrong hostname rather
// than an error. Predicate and strip now share one definition in odata_url_helpers.
TEST_CASE("StripHttpScheme removes either scheme in any case", "[graph_sharepoint][url_builder]") {
    REQUIRE(erpl_web::StripHttpScheme("https://tenant.sharepoint.com/sites/X") ==
            "tenant.sharepoint.com/sites/X");
    REQUIRE(erpl_web::StripHttpScheme("HTTPS://tenant.sharepoint.com") == "tenant.sharepoint.com");
    REQUIRE(erpl_web::StripHttpScheme("Http://host") == "host");
}

TEST_CASE("StripHttpScheme refuses what it cannot strip", "[graph_sharepoint][url_builder]") {
    // The old inline code returned value.substr(7) here, turning "team-site" into "site".
    REQUIRE_THROWS(erpl_web::StripHttpScheme("team-site"));
    REQUIRE_THROWS(erpl_web::StripHttpScheme(""));
    REQUIRE_THROWS(erpl_web::StripHttpScheme("ftp://host/x"));
}

TEST_CASE("HostOfAbsoluteHttpUrl takes the host and nothing else",
          "[graph_sharepoint][url_builder]") {
    REQUIRE(erpl_web::HostOfAbsoluteHttpUrl("https://tenant.sharepoint.com/sites/Finance") ==
            "tenant.sharepoint.com");
    REQUIRE(erpl_web::HostOfAbsoluteHttpUrl("https://tenant.sharepoint.com") ==
            "tenant.sharepoint.com");
    REQUIRE(erpl_web::HostOfAbsoluteHttpUrl("HTTPS://Tenant.SharePoint.com/x/y") ==
            "Tenant.SharePoint.com");
    REQUIRE_THROWS(erpl_web::HostOfAbsoluteHttpUrl("tenant.sharepoint.com/sites/Finance"));
}
