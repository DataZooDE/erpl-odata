#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb.hpp"

#include "odata_catalog.hpp"

#include <iostream>
#include <string>

using namespace erpl_web;

// ============================================================================
// IGNORE pattern handling (GitHub #87b)
// ============================================================================

TEST_CASE("OData catalog IGNORE pattern matching", "[odata_catalog]")
{
    std::cout << std::endl;

    // A glob pattern hides the entity sets it matches ...
    REQUIRE(ODataCatalog::MatchesIgnorePattern("Products", "Prod*"));
    REQUIRE(ODataCatalog::MatchesIgnorePattern("I_DraftAdministrativeData", "I_Draft*"));
    REQUIRE(ODataCatalog::MatchesIgnorePattern("Orders", "*"));

    // ... and leaves everything else alone
    REQUIRE_FALSE(ODataCatalog::MatchesIgnorePattern("Orders", "Prod*"));
    REQUIRE_FALSE(ODataCatalog::MatchesIgnorePattern("Products", "products*"));

    // No IGNORE option was given: nothing may be hidden
    REQUIRE_FALSE(ODataCatalog::MatchesIgnorePattern("Products", ""));
}

// ============================================================================
// Metadata failures must not masquerade as missing tables (GitHub #87a)
// ============================================================================

TEST_CASE("OData catalog reports metadata failures instead of hiding tables", "[odata_catalog]")
{
    std::cout << std::endl;

    duckdb::DBConfig config;
    config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));
    duckdb::DuckDB db(nullptr, &config);
    duckdb::Connection con(db);

    auto load_result = con.Query("LOAD erpl_odata;");
    if (load_result->HasError()) {
        std::cout << "Note: Extension not loaded, skipping catalog integration test" << std::endl;
        return;
    }

    // Port 1 is never served, so the $metadata request fails fast with a connection error
    const std::string service_url = "http://127.0.0.1:1/unreachable.svc/";

    // ATTACH itself does not talk to the service, so it must still succeed
    auto attach_result = con.Query("ATTACH '" + service_url + "' AS unreachable_odata (TYPE odata)");
    REQUIRE(!attach_result->HasError());

    auto scan_result = con.Query("SELECT * FROM unreachable_odata.SomeEntitySet");
    REQUIRE(scan_result->HasError());

    const auto error_message = scan_result->GetError();
    std::cout << "Catalog error: " << error_message << std::endl;

    // The real cause and the service URL must both survive
    REQUIRE(error_message.find("Failed to load OData metadata") != std::string::npos);
    REQUIRE(error_message.find("127.0.0.1:1") != std::string::npos);

    // The old behaviour reported the metadata failure as a missing table
    REQUIRE(error_message.find("does not exist") == std::string::npos);
}
