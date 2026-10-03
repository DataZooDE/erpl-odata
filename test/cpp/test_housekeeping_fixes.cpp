#include "catch.hpp"

#include "datasphere_read.hpp"
#include "duckdb_argument_helper.hpp"
#include "odata_test_server.hpp"
#include "duckdb.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using namespace erpl_web::test_support;

// A batch of small defects that each turned a user mistake into something worse than an
// error message. See GitHub #243.


TEST_CASE("a missing secret field is bad input, not an internal error", "[housekeeping]") {
    // InternalException INVALIDATES THE WHOLE DATABASE INSTANCE. Using it for a key the
    // user forgot to put in their own CREATE SECRET took the database down and reported
    // "INTERNAL Error: Failed to fetch key 'tenant_name' from secret".
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(con.Query("CREATE SECRET ds_incomplete (TYPE datasphere, PROVIDER oauth2, "
                            "client_id 'c', client_secret 's')")
                      ->HasError());

    auto result = con.Query("SELECT * FROM datasphere_read_relational('SP', 'A', 'ds_incomplete')");
    REQUIRE(result->HasError());

    const std::string error = result->GetError();
    INFO("error: " << error);
    REQUIRE(error.find("INTERNAL") == std::string::npos);
    // And it names what is actually missing, and where.
    REQUIRE(error.find("tenant_name") != std::string::npos);
    REQUIRE(error.find("ds_incomplete") != std::string::npos);
}

TEST_CASE("a BEARER auth_type actually sends a bearer token", "[housekeeping][security]") {
    // auth_type is a DuckDB ENUM (BASIC/DIGEST/BEARER), so the binder rejects any other
    // spelling before the function runs - lowercase included. What this pins is that the
    // declared value that IS implemented does what it says.
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := 'tok', auth_type := 'BEARER')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    const auto requests = server.RequestsFor("/echo");
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().HasHeader("Authorization"));
    REQUIRE(requests.front().Header("Authorization") == "Bearer tok");
}

TEST_CASE("an unsupported auth_type is refused rather than silently substituted",
          "[housekeeping][security]") {
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := 'user:pass', auth_type := 'DIGEST')");
    REQUIRE(result->HasError());

    const std::string error = result->GetError();
    INFO("error: " << error);
    REQUIRE(error.find("DIGEST") != std::string::npos);

    // And nothing went out. Previously the call succeeded using registered secrets, so the
    // caller's own credentials were dropped without a word.
    REQUIRE(server.RequestsFor("/echo").empty());
}

TEST_CASE("asset segments are appended to the path, not after the query string",
          "[housekeeping]") {
    // "https://host/path?$top=5" used to become "https://host/path?$top=5/A/A", burying the
    // segments inside the query value. Reachable through the documented absolute-URL hatch.
    std::string url = "https://tenant.eu10.hcs.cloud.sap/api/v1/dwaas-core/data/SP?$top=5";
    erpl_web::EnsureAssetSegmentPattern(url, "CUSTOMER");

    INFO(url);
    // The query is intact and still at the end, and nothing was appended after it.
    REQUIRE(url.find("?$top=5") != std::string::npos);
    REQUIRE(url.find("$top=5/") == std::string::npos);
    REQUIRE(url.substr(url.size() - 7) == "?$top=5");
    // The asset segment landed on the path, before the query.
    REQUIRE(url.find("/CUSTOMER?$top=5") != std::string::npos);
}
