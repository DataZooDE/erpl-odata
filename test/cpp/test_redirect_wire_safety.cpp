#include "catch.hpp"

#include "datazoo/oauth2/http_client.hpp"
#include "odata_test_server.hpp"
#include "duckdb.hpp"

#include <string>

using erpl_web::HttpUrl;
using namespace erpl_web::test_support;

namespace {

// Same shape as the other server-backed tests here. allocator_background_threads is
// required: without it a DEBUG+jemalloc build throws from a worker thread shortly after
// the first query (see CLAUDE.md).
class TestDatabase {
public:
    TestDatabase()
    {
        config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));
        database = duckdb::make_uniq<duckdb::DuckDB>(nullptr, &config);
        connection = duckdb::make_uniq<duckdb::Connection>(*database);
    }

    duckdb::Connection &Con() const { return *connection; }

private:
    duckdb::DBConfig config;
    duckdb::unique_ptr<duckdb::DuckDB> database;
    duckdb::unique_ptr<duckdb::Connection> connection;
};

}  // namespace

// The redirect follower was the last seam that checked only HALF the question about a
// service-supplied URL.
//
// Following a redirect asks two independent things, and the answer to one says nothing
// about the other:
//
//   May CREDENTIALS survive the hop?  -> IsSameOrigin. This was already checked.
//   Can the URL be SENT at all?       -> IsWireSafeTarget. This was not (GitHub #234).
//
// A Location of "https://<same-host>/x\r\nX-Injected: 1" keeps its host, so it passes the
// origin check and is then requested WITH credentials attached - while httplib writes the
// target verbatim, so the CRLF splits the request line and the attacker's header goes out.

TEST_CASE("IsWireSafeTarget refuses what cannot go in a request line", "[redirect][security]") {
    SECTION("an ordinary URL is fine") {
        REQUIRE(HttpUrl("https://example.com/svc/Airlines?$top=10").IsWireSafeTarget());
    }

    SECTION("a percent-encoded space is fine - encoded, it is three ordinary characters") {
        REQUIRE(HttpUrl("https://example.com/svc/Airlines?$orderby=Name%20desc").IsWireSafeTarget());
    }

    SECTION("a raw space is refused - it ends the target in the request line") {
        REQUIRE_FALSE(HttpUrl("https://example.com/svc/Airlines?$orderby=Name desc").IsWireSafeTarget());
    }

    SECTION("a CR or LF anywhere in path or query is refused") {
        REQUIRE_FALSE(HttpUrl("https://example.com/x\r\nX-Injected: 1").IsWireSafeTarget());
        REQUIRE_FALSE(HttpUrl("https://example.com/x?a=b\rX-Injected: 1").IsWireSafeTarget());
        REQUIRE_FALSE(HttpUrl("https://example.com/x?a=b\nX-Injected: 1").IsWireSafeTarget());
    }

    SECTION("the check covers the host, not just the path and query") {
        // The host reaches the Host header, and behind a proxy the request line too.
        REQUIRE_FALSE(HttpUrl("https://exa\r\nmple.com/x").IsWireSafeTarget());
    }

    SECTION("a same-origin URL can still be unsendable - which is the whole point") {
        const HttpUrl base("https://example.com/svc/Airlines");
        const HttpUrl hostile("https://example.com/svc/x?a=b c");

        // Origin says yes; sendability says no. Checking only the first is what #234 was.
        REQUIRE(base.IsSameOrigin(hostile));
        REQUIRE_FALSE(hostile.IsWireSafeTarget());
    }
}

// End to end. A raw CR cannot be pushed through a test server's Location header - httplib
// validates header values and would refuse to send it - so the case driven here is the raw
// space, which is a legal header value and reaches the client intact. The control-character
// half is pinned by the predicate above, which is what the follower calls.
TEST_CASE("a redirect to an unsendable URL is refused rather than followed",
          "[redirect][security]") {
    ODataTestServer server;

    const std::string context = server.Url("/svc/$metadata") + "#Airlines";
    server.ServeMetadataFixture("/svc/$metadata", "edm_trippin.xml");

    // Same origin, so the credential check would have let this through.
    server.OnPath("/svc/Airlines",
                  CannedResponse::Error(302).WithHeader("Location", server.Url("/svc/Moved?a=b c")));

    const std::string airline = R"({"AirlineCode":"AA","Name":"American Airlines"})";
    server.OnPath("/svc/Moved",
                  CannedResponse::Json(MakeV4Page(context, std::vector<std::string>{airline})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + server.Url("/svc/Airlines") + "')");
    REQUIRE(result->HasError());

    const auto error = result->GetError();
    INFO("error was: " << error);
    REQUIRE(error.find("cannot be sent intact") != std::string::npos);

    // The redirect target was never requested. Note this one does NOT discriminate on its
    // own: without the guard the follower does go, but the mangled request line never
    // arrives intact, so the server records nothing either way. The error message above is
    // the assertion that actually distinguishes guarded from unguarded - it is the one that
    // fails when the guard is removed.
    REQUIRE(server.RequestsFor("/svc/Moved").empty());

    // And the first request did happen, so the assertion above is not vacuous.
    REQUIRE_FALSE(server.RequestsFor("/svc/Airlines").empty());
}
