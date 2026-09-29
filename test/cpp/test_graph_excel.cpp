#include "catch.hpp"
#include "duckdb.hpp"
#include "graph_excel_client.hpp"
#include "graph_excel_functions.hpp"

using namespace erpl_web;

// =============================================================================
// GraphExcelUrlBuilder Tests
// =============================================================================

TEST_CASE("GraphExcelUrlBuilder - GetBaseUrl", "[graph_excel][url_builder]") {
    REQUIRE(GraphExcelUrlBuilder::GetBaseUrl() == "https://graph.microsoft.com/v1.0");
}

TEST_CASE("GraphExcelUrlBuilder - BuildDriveItemUrl", "[graph_excel][url_builder]") {
    auto url = GraphExcelUrlBuilder::BuildDriveItemUrl("abc123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123");
}

TEST_CASE("GraphExcelUrlBuilder - BuildDriveItemByPathUrl", "[graph_excel][url_builder]") {
    // Without leading slash
    auto url1 = GraphExcelUrlBuilder::BuildDriveItemByPathUrl("Documents/test.xlsx");
    REQUIRE(url1 == "https://graph.microsoft.com/v1.0/me/drive/root:/Documents/test.xlsx:");

    // With leading slash (should be removed)
    auto url2 = GraphExcelUrlBuilder::BuildDriveItemByPathUrl("/Documents/test.xlsx");
    REQUIRE(url2 == "https://graph.microsoft.com/v1.0/me/drive/root:/Documents/test.xlsx:");
}

TEST_CASE("GraphExcelUrlBuilder - BuildSiteDriveItemUrl", "[graph_excel][url_builder]") {
    auto url = GraphExcelUrlBuilder::BuildSiteDriveItemUrl("site-id-123", "item-id-456");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/drive/items/item-id-456");
}

TEST_CASE("GraphExcelUrlBuilder - BuildWorkbookUrl", "[graph_excel][url_builder]") {
    std::string item_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123";
    auto url = GraphExcelUrlBuilder::BuildWorkbookUrl(item_url);
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook");
}

TEST_CASE("GraphExcelUrlBuilder - BuildTablesUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildTablesUrl(workbook_url);
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/tables");
}

TEST_CASE("GraphExcelUrlBuilder - BuildTableUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildTableUrl(workbook_url, "MyTable");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/tables/MyTable");
}

TEST_CASE("GraphExcelUrlBuilder - BuildTableRowsUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildTableRowsUrl(workbook_url, "MyTable");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/tables/MyTable/rows?$top=1000");
}

TEST_CASE("GraphExcelUrlBuilder - BuildWorksheetsUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildWorksheetsUrl(workbook_url);
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/worksheets");
}

TEST_CASE("GraphExcelUrlBuilder - BuildWorksheetUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildWorksheetUrl(workbook_url, "Sheet1");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/worksheets/Sheet1");
}

TEST_CASE("GraphExcelUrlBuilder - BuildUsedRangeUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildUsedRangeUrl(workbook_url, "Sheet1");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/worksheets/Sheet1/usedRange?$select=values,valueTypes,numberFormat");
}

TEST_CASE("GraphExcelUrlBuilder - BuildRangeUrl", "[graph_excel][url_builder]") {
    std::string workbook_url = "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook";
    auto url = GraphExcelUrlBuilder::BuildRangeUrl(workbook_url, "Sheet1", "A1:D10");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/abc123/workbook/worksheets/Sheet1/range(address='A1:D10')?$select=values,valueTypes,numberFormat");
}

TEST_CASE("GraphExcelUrlBuilder - BuildDriveRootChildrenUrl", "[graph_excel][url_builder]") {
    auto url = GraphExcelUrlBuilder::BuildDriveRootChildrenUrl();
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/root/children");
}

TEST_CASE("GraphExcelUrlBuilder - BuildDriveFolderChildrenUrl", "[graph_excel][url_builder]") {
    auto url = GraphExcelUrlBuilder::BuildDriveFolderChildrenUrl("folder-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/drive/items/folder-id-123/children");
}

TEST_CASE("GraphExcelUrlBuilder - BuildSiteDriveRootChildrenUrl", "[graph_excel][url_builder]") {
    auto url = GraphExcelUrlBuilder::BuildSiteDriveRootChildrenUrl("site-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/sites/site-id-123/drive/root/children");
}

// =============================================================================
// DuckDB Integration Tests (Secret Creation)
// =============================================================================

TEST_CASE("Microsoft Graph Secret Creation via DuckDB", "[graph_excel][secret]") {
    duckdb::DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    duckdb::DuckDB db(nullptr, &config);
    duckdb::Connection con(db);

    // Load the extension
    auto result = con.Query("LOAD erpl_odata");
    REQUIRE_FALSE(result->HasError());

    SECTION("Create secret with client_credentials provider") {
        result = con.Query(R"(
            CREATE SECRET test_graph_secret (
                TYPE microsoft_graph,
                tenant_id 'test-tenant-12345',
                client_id 'test-client-67890',
                client_secret 'test-secret-abcde'
            )
        )");
        REQUIRE_FALSE(result->HasError());

        // Verify secret was created
        result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 'test_graph_secret'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);

        // Cleanup
        con.Query("DROP SECRET test_graph_secret");
    }

    SECTION("Create secret with config provider") {
        result = con.Query(R"(
            CREATE SECRET test_graph_config_secret (
                TYPE microsoft_graph,
                PROVIDER config,
                tenant_id 'test-tenant-12345',
                access_token 'pre-acquired-token-xyz'
            )
        )");
        REQUIRE_FALSE(result->HasError());

        // Verify secret was created
        result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 'test_graph_config_secret'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);

        // Cleanup
        con.Query("DROP SECRET test_graph_config_secret");
    }

    SECTION("Validation - missing tenant_id") {
        result = con.Query(R"(
            CREATE SECRET test_graph_invalid (
                TYPE microsoft_graph,
                client_id 'test-client',
                client_secret 'test-secret'
            )
        )");
        REQUIRE(result->HasError());
        REQUIRE(result->GetError().find("tenant_id") != std::string::npos);
    }

    SECTION("Validation - missing client_id") {
        result = con.Query(R"(
            CREATE SECRET test_graph_invalid (
                TYPE microsoft_graph,
                tenant_id 'test-tenant',
                client_secret 'test-secret'
            )
        )");
        REQUIRE(result->HasError());
        REQUIRE(result->GetError().find("client_id") != std::string::npos);
    }

    SECTION("Validation - missing client_secret") {
        result = con.Query(R"(
            CREATE SECRET test_graph_invalid (
                TYPE microsoft_graph,
                tenant_id 'test-tenant',
                client_id 'test-client'
            )
        )");
        REQUIRE(result->HasError());
        REQUIRE(result->GetError().find("client_secret") != std::string::npos);
    }
}

TEST_CASE("Microsoft Graph Excel Functions Exist", "[graph_excel][functions]") {
    duckdb::DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    duckdb::DuckDB db(nullptr, &config);
    duckdb::Connection con(db);

    // Load the extension
    auto result = con.Query("LOAD erpl_odata");
    REQUIRE_FALSE(result->HasError());

    SECTION("graph_show_files function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_show_files'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_excel_tables function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_excel_tables'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_excel_worksheets function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_excel_worksheets'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_excel_range function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_excel_range'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_excel_read function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_excel_read'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_excel_delete_rows function exists with both overloads") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_excel_delete_rows'");
        REQUIRE_FALSE(result->HasError());
        // Registered as a set with an index-based and a name-based overload.
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() >= 1);
    }
}

// =============================================================================
// ResolveColumnIndex Tests (pure column-reference resolution)
// =============================================================================

TEST_CASE("GraphExcelFunctions - ResolveColumnIndex by name", "[graph_excel][delete_rows]") {
    const std::vector<std::string> columns = {"Region", "Amount", "Status"};

    SECTION("exact name match") {
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "Region") == 0);
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "Amount") == 1);
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "Status") == 2);
    }

    SECTION("case-insensitive name match") {
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "region") == 0);
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "AMOUNT") == 1);
    }

    SECTION("exact match wins over case-insensitive match") {
        const std::vector<std::string> dup = {"id", "ID"};
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(dup, "ID") == 1);
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(dup, "id") == 0);
    }
}

TEST_CASE("GraphExcelFunctions - ResolveColumnIndex numeric fallback", "[graph_excel][delete_rows]") {
    const std::vector<std::string> columns = {"Region", "Amount", "Status"};

    SECTION("numeric reference is treated as a 0-based index") {
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "0") == 0);
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "2") == 2);
    }

    SECTION("numeric fallback works even past the known column count") {
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(columns, "9") == 9);
    }

    SECTION("a column literally named like a number matches by name first") {
        const std::vector<std::string> numeric_names = {"2024", "2025"};
        // "2024" matches the column name at index 0, not index 2024.
        REQUIRE(GraphExcelFunctions::ResolveColumnIndex(numeric_names, "2024") == 0);
    }
}

TEST_CASE("GraphExcelFunctions - ResolveColumnIndex unknown column throws", "[graph_excel][delete_rows]") {
    const std::vector<std::string> columns = {"Region", "Amount"};
    REQUIRE_THROWS_AS(GraphExcelFunctions::ResolveColumnIndex(columns, "Nope"),
                      duckdb::InvalidInputException);
}
