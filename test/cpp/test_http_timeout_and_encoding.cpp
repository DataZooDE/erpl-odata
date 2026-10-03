#include "catch.hpp"

#include "odata_test_server.hpp"
#include "duckdb.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using namespace erpl_web::test_support;

// The timeout, url_encode and binary-content behaviour of http_get.
//
// These were asserted in test/sql/web_http.test as
//
//     SELECT CASE WHEN status IN (200, 502) THEN 200 ELSE status END
//     FROM http_get('https://httpbin.org/delay/2') LIMIT 1;  ----> 200
//
// which cannot fail for two independent reasons: /delay/2 answers 200 whatever the client
// does with timeouts, and normalising 502 to 200 passes a failed request too. Deleting the
// timeout assignment in web_functions.cpp left the timeout tests green.
//
// A client timeout can only be tested against a server SLOWER than it, which is what
// CannedResponse::DelayedBy is for. See GitHub #246.


TEST_CASE("a timeout shorter than the server's delay fails the request", "[http_timeout]") {
    // THE case the old tests could not express. Without it, nothing distinguishes a timeout
    // that is honoured from one that is ignored.
    ODataTestServer server;
    server.OnPath("/slow", CannedResponse::Json(R"({"ok":true})").DelayedBy(3000));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/slow") +
                            "', timeout := 500)");
    INFO("result: " << (result->HasError() ? result->GetError() : std::string("<succeeded>")));
    REQUIRE(result->HasError());

    // And the request really was sent - the client gave up waiting, rather than never
    // having gone out. Otherwise this would pass for the wrong reason.
    REQUIRE_FALSE(server.RequestsFor("/slow").empty());
}

TEST_CASE("a timeout longer than the server's delay succeeds", "[http_timeout]") {
    // The other half. A timeout implementation that failed everything would satisfy the
    // case above on its own.
    ODataTestServer server;
    server.OnPath("/brief", CannedResponse::Json(R"({"ok":true})").DelayedBy(200));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/brief") +
                            "', timeout := 10000)");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 200);
}

TEST_CASE("url_encode is visible in what reaches the wire", "[http_encoding]") {
    // Asserted on the recorded request target, because that is the only place the setting
    // has any observable effect. The old test asserted a status, which it cannot change.
    ODataTestServer server;
    server.OnPath("/q", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/q") +
                            "?param=a%20b', url_encode := false)");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    const auto requests = server.RequestsFor("/q");
    REQUIRE_FALSE(requests.empty());
    INFO("target: " << requests.front().target);

    // With url_encode := false the caller's encoding is written verbatim: the %20 survives
    // as %20 and is not re-encoded into %2520.
    REQUIRE(requests.front().target.find("param=a%20b") != std::string::npos);
    REQUIRE(requests.front().target.find("%2520") == std::string::npos);
}

TEST_CASE("a response body arrives whole", "[http_encoding]") {
    // The old test asserted only a status against /bytes/100, so it could not tell a
    // complete body from a truncated one - which is the thing worth knowing.
    //
    // Printable ASCII on purpose. http_get runs responses through charset conversion, so a
    // body of raw 0x01 bytes comes back longer than it went out (256 in, 366 counted) and an
    // exact-length assertion would be measuring the converter rather than the transfer.
    // Truncation is what this case is for; byte-exact binary passthrough is a separate
    // question and not one this test should pretend to answer.
    const std::string body(4096, 'x');

    ODataTestServer server;
    server.OnPath("/bulk", CannedResponse::Json(body));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT length(content) FROM http_get('" + server.Url("/bulk") + "')");
    INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    // Length, not status: a short read is exactly what a status cannot show.
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == static_cast<int64_t>(body.size()));
}

TEST_CASE("a truncated response is not reported as a whole one", "[http_encoding]") {
    // The counterpart: the server announces the full length and delivers a prefix, which is
    // what a service dying mid-response looks like. Whatever http_get does here, it must not
    // hand back a short body as though it were complete.
    const std::string body(4096, 'x');

    ODataTestServer server;
    server.OnPath("/cut", CannedResponse::Json(body).TruncatedAfter(512));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT length(content) FROM http_get('" + server.Url("/cut") + "')");

    if (result->HasError()) {
        INFO("errored, which is a fine answer: " << result->GetError());
        SUCCEED("a truncated response was reported as a failure");
    } else {
        const auto length = result->GetValue(0, 0).GetValue<int64_t>();
        INFO("length: " << length);
        // It must not claim the full body. Either it errors, or it returns what it got.
        REQUIRE(length != static_cast<int64_t>(body.size()));
    }
}

// ---------------------------------------------------------------------------
// Which HTTP method each function actually sends.
//
// The SQL suite covers the verbs against httpbin, but most of those cases cannot tell the
// verbs apart: /status/200 and /anything answer 200 to ANY method, so http_get, http_head,
// http_post, http_put and http_delete would all pass there even if every one of them sent
// the same request. Only the /patch case constrains its method at all.
//
// The recorded request carries the method verbatim, which settles it.

TEST_CASE("each http_* function sends its own method", "[http_encoding][http_methods]") {
    ODataTestServer server;
    server.OnPath("/verb", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    const std::string url = server.Url("/verb");

    struct Case {
        const char *query;
        const char *expected_method;
    };

    // http_head is deliberately absent: httplib's server does not record a body-less HEAD
    // through the same handler path, so asserting on it here would be testing the fixture.
    const Case cases[] = {
        {"SELECT status FROM http_get('%URL%')", "GET"},
        {"SELECT status FROM http_post('%URL%', '{\"a\":1}', 'application/json')", "POST"},
        {"SELECT status FROM http_put('%URL%', '{\"a\":1}', 'application/json')", "PUT"},
        {"SELECT status FROM http_patch('%URL%', '{\"a\":1}', 'application/json')", "PATCH"},
        {"SELECT status FROM http_delete('%URL%', '{}'::JSON)", "DELETE"},
    };

    for (const auto &test_case : cases) {
        std::string query = test_case.query;
        const auto placeholder = query.find("%URL%");
        query.replace(placeholder, 5, url);

        INFO("query: " << query);
        auto result = con.Query(query);
        INFO("error: " << (result->HasError() ? result->GetError() : std::string("<none>")));
        REQUIRE_FALSE(result->HasError());
    }

    const auto requests = server.RequestsFor("/verb");
    REQUIRE(requests.size() == 5);

    std::vector<std::string> methods;
    for (const auto &request : requests) {
        methods.push_back(request.method);
    }

    REQUIRE(methods == std::vector<std::string>{"GET", "POST", "PUT", "PATCH", "DELETE"});
}
