#include "catch.hpp"

#include "odata_test_server.hpp"
#include "duckdb.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using namespace erpl_web::test_support;

// What http_get actually PUTS ON THE WIRE for each way of supplying credentials.
//
// This coverage used to live in test/sql/web_http.test against
// https://httpbin.org/status/200 - an endpoint that answers 200 whatever you send - and
// asserted only on the status. Deleting request->AuthHeadersFromParams() in
// web_functions.cpp left all six cases green.
//
// Rewriting them against httpbin's echo endpoints fixed that, and then cost something else:
// five live requests per run, one of which failed on the musl CI leg while its siblings
// passed. The old `CASE WHEN status IN (200, 502) THEN 200` wrapper had been papering over
// exactly that, at the price of also passing when the request failed outright.
//
// Asserting against the local test server keeps what the rewrite gained - these fail when
// auth breaks - and drops what it cost, because the recorded request is inspected directly
// rather than round-tripped through a third party. See GitHub #243.

namespace {


// base64 of the credential pairs under test, so the expectations read as what they are.
// "username_only:" keeps its trailing colon: the empty password is the point of that case.
constexpr const char *BASIC_PARAM = "Basic cGFyYW1fdXNlcjpwYXJhbV9wYXNz";        // param_user:param_pass
constexpr const char *BASIC_SECRET = "Basic c2VjcmV0X3VzZXI6c2VjcmV0X3Bhc3M=";   // secret_user:secret_pass
constexpr const char *BASIC_COLONLESS = "Basic dXNlcm5hbWVfb25seTo=";            // username_only:
constexpr const char *BASIC_EXPLICIT = "Basic ZXhwbGljaXRfdXNlcjpleHBsaWNpdF9wYXNz";

std::string SoleAuthorizationHeader(const ODataTestServer &server)
{
    const auto requests = server.RequestsFor("/echo");
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().HasHeader("Authorization"));
    return requests.front().Header("Authorization");
}

}  // namespace

TEST_CASE("the auth parameter is sent as basic credentials", "[http_auth]") {
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := 'param_user:param_pass')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(SoleAuthorizationHeader(server) == BASIC_PARAM);
}

TEST_CASE("the auth parameter takes precedence over a registered secret", "[http_auth]") {
    // Asserted in both directions: the parameter's credentials present AND the secret's
    // absent. Checking only the first would pass if both were somehow sent.
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    REQUIRE_FALSE(con.Query("CREATE SECRET scoped_basic (TYPE http_basic, USERNAME 'secret_user', "
                            "PASSWORD 'secret_pass', SCOPE '" + server.Url("/") + "')")
                      ->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := 'param_user:param_pass')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    const auto sent = SoleAuthorizationHeader(server);
    REQUIRE(sent == BASIC_PARAM);
    REQUIRE(sent != BASIC_SECRET);
}

TEST_CASE("with no auth parameter the registered secret is sent", "[http_auth]") {
    // The other half of the precedence case. Without this, "the parameter won" could not be
    // told apart from "the secret was never usable in the first place".
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    REQUIRE_FALSE(con.Query("CREATE SECRET scoped_basic (TYPE http_basic, USERNAME 'secret_user', "
                            "PASSWORD 'secret_pass', SCOPE '" + server.Url("/") + "')")
                      ->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") + "')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(SoleAuthorizationHeader(server) == BASIC_SECRET);
}

TEST_CASE("an explicit BEARER auth_type sends a bearer token", "[http_auth]") {
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query(
        "SELECT status FROM http_get('" + server.Url("/echo") +
        "', auth := 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.test.token', auth_type := 'BEARER')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(SoleAuthorizationHeader(server) ==
            "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.test.token");
}

TEST_CASE("an explicit BASIC auth_type sends those credentials", "[http_auth]") {
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := 'explicit_user:explicit_pass', auth_type := 'BASIC')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(SoleAuthorizationHeader(server) == BASIC_EXPLICIT);
}

TEST_CASE("a colonless auth value becomes a username with an empty password", "[http_auth]") {
    // The trailing colon in base64("username_only:") is the empty password, and it is the
    // whole point of the case - an encoder that dropped it would still look plausible.
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := 'username_only')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(SoleAuthorizationHeader(server) == BASIC_COLONLESS);
}
