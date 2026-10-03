// GitHub #161 left odata_read with a hard ceiling of 10000 server-driven pages and no way
// for a caller to do anything about it. The error text tells them to "ask the service for
// larger pages", which is sound advice with no supported way to follow it: OData says a
// client asks with `Prefer: odata.maxpagesize=N`, and odata_read had no such parameter.
// odp_odata_read has had one all along (odp_http_request_factory.cpp), so the asymmetry
// was accidental rather than deliberate.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odata_client.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {


const char *const AIRLINE_AA = R"({"AirlineCode":"AA","Name":"American Airlines"})";
const char *const AIRLINE_FM = R"({"AirlineCode":"FM","Name":"Shanghai Airline"})";

}  // namespace

TEST_CASE("odata_read asks the service for a page size with Prefer", "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/mps/Airlines");
    const std::string context = server.Url("/mps/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/mps/$metadata", "edm_trippin.xml");
    server.OnPath("/mps/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url +
                            "', max_page_size=5000)");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 2);

    bool asked = false;
    for (const auto &request : server.RequestsFor("/mps/Airlines")) {
        INFO("Prefer: " << request.Header("Prefer"));
        if (request.Header("Prefer").find("odata.maxpagesize=5000") != std::string::npos) {
            asked = true;
        }
    }
    INFO("no request to the entity set carried Prefer: odata.maxpagesize=5000");
    REQUIRE(asked);
}

// Without the parameter nothing changes: a Prefer header the caller did not ask for can
// make a service page differently than it otherwise would.
TEST_CASE("odata_read sends no page-size preference unless asked", "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/nomps/Airlines");
    const std::string context = server.Url("/nomps/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/nomps/$metadata", "edm_trippin.xml");
    server.OnPath("/nomps/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "')")->HasError());

    // Without this the loop below asserts nothing whenever the entity set was never
    // requested at all - which is every way this test could break other than the one it
    // is meant to catch.
    const auto requests = server.RequestsFor("/nomps/Airlines");
    INFO("the entity set was never requested, so the loop below proves nothing");
    REQUIRE_FALSE(requests.empty());

    for (const auto &request : requests) {
        INFO("Prefer: " << request.Header("Prefer"));
        REQUIRE(request.Header("Prefer").find("odata.maxpagesize") == std::string::npos);
    }
}

// A page size of zero is not "no preference" - it is a request the service cannot honour,
// and sending it would be worse than rejecting it here where the caller can see why.
TEST_CASE("odata_read rejects a page size of zero", "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/zeromps/Airlines");
    const std::string context = server.Url("/zeromps/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/zeromps/$metadata", "edm_trippin.xml");
    server.OnPath("/zeromps/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result =
        con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "', max_page_size=0)");
    REQUIRE(result->HasError());
    INFO(result->GetError());
    REQUIRE(result->GetError().find("max_page_size") != std::string::npos);
    // A bad argument is the caller's, not a broken invariant of ours: an INTERNAL error
    // here would invalidate the whole database instance.
    REQUIRE(result->GetErrorObject().Type() != duckdb::ExceptionType::INTERNAL);
}

// The ceiling's own error message tells the caller to ask for larger pages. It has to name
// the parameter that does that, or the advice is unactionable - which is the whole reason
// this parameter exists. Driving the real ceiling would need 10000 round trips, so the
// message is built by a named function and that function is what is asserted.
TEST_CASE("the page-ceiling error names the parameter that raises the page size",
          "[odata_maxpagesize]") {
    const auto message = erpl_web::BuildPageLimitExceededMessage(
        erpl_web::ODataClient<erpl_web::ODataEntitySetResponse>::MAX_PAGE_REQUESTS,
        "https://service.example/Entity?$skiptoken=9999");

    INFO(message);
    REQUIRE(message.find("10000") != std::string::npos);
    REQUIRE(message.find("max_page_size") != std::string::npos);
    REQUIRE(message.find("https://service.example/Entity?$skiptoken=9999") != std::string::npos);
}

// A client is re-minted in two places mid-query: CloneForScan gives each execution a
// private pagination cursor, and predicate pushdown rebuilds it whenever it changes the
// URL. A setting not copied at both sites is silently lost - which is how this parameter
// was first written, and the reason the first test above failed before the fix. A filter
// forces the pushdown path specifically.
TEST_CASE("the page-size preference survives a client rebuilt by predicate pushdown",
          "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/mpspush/Airlines");
    const std::string context = server.Url("/mpspush/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/mpspush/$metadata", "edm_trippin.xml");
    server.OnPath("/mpspush/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT Name FROM odata_read('" + entity_url +
                            "', max_page_size=250) WHERE AirlineCode = 'AA'");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    // Only requests that actually carry the pushed-down filter go through the rebuilt
    // client, so those are the ones that prove the setting travelled.
    bool rebuilt_client_asked = false;
    bool rebuilt_client_forgot = false;
    for (const auto &request : server.RequestsFor("/mpspush/Airlines")) {
        if (request.DecodedQueryParam("$filter").empty()) {
            continue;
        }
        INFO("filtered request Prefer: " << request.Header("Prefer"));
        if (request.Header("Prefer").find("odata.maxpagesize=250") != std::string::npos) {
            rebuilt_client_asked = true;
        } else {
            rebuilt_client_forgot = true;
        }
    }
    INFO("no filtered request was made, so the pushdown path was never exercised");
    REQUIRE(rebuilt_client_asked);
    REQUIRE_FALSE(rebuilt_client_forgot);
}

// The same across executions of a bound plan, which is the CloneForScan site.
TEST_CASE("the page-size preference survives re-execution of a bound plan",
          "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/mpsreexec/Airlines");
    const std::string context = server.Url("/mpsreexec/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/mpsreexec/$metadata", "edm_trippin.xml");
    // Two pages on purpose. With a single page the first execution issues no request at
    // all - the bind-time probe already paid for page one and CloneForScan adopts it - so
    // there would be nothing on the wire to assert against.
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/mpsreexec/Airlines" &&
                   request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_FM})));
    server.OnPath("/mpsreexec/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(con.Query("PREPARE p AS SELECT * FROM odata_read('" + entity_url +
                            "', max_page_size=750)")
                      ->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        INFO("execution " << execution);
        server.ClearRequests();
        auto result = con.Query("EXECUTE p");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());

        // The next-link page is fetched through the per-execution client, so it is the
        // one that proves the setting survived the clone.
        bool asked = false;
        for (const auto &request : server.RequestsFor("/mpsreexec/Airlines")) {
            if (request.QueryParam("$skiptoken") != "2") {
                continue;
            }
            INFO("page-2 Prefer: " << request.Header("Prefer"));
            if (request.Header("Prefer").find("odata.maxpagesize=750") != std::string::npos) {
                asked = true;
            }
        }
        INFO("this execution fetched no next-link page, so nothing was proven");
        REQUIRE(asked);
    }
}

// GitHub #185 / crew F3. ODataReadBind probes the URL before ProcessNamedParameters runs,
// and FromProbeResult buffers the probe body as page one. With no projection and no filter
// the URL never changes, so UpdateUrlFromPredicatePushdown takes its early return, the
// buffered page survives and PrefetchFirstPage is skipped. The result: the headline shape
// for this feature - a large unprojected extraction - sent page one with NO Prefer header
// and got the service's default page size; only page two onwards carried the preference.
//
// Every other test in this file uses COUNT(*) or a projection, which changes the URL and
// forces a refetch, so none of them could see it.
TEST_CASE("odata_read sends the page-size preference on the very first request",
          "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/mpsfirst/Airlines");
    const std::string context = server.Url("/mpsfirst/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/mpsfirst/$metadata", "edm_trippin.xml");
    // Deliberately a SINGLE page: with no next link there is no second request that could
    // carry the header and rescue the assertion.
    server.OnPath("/mpsfirst/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // SELECT * with an ORDER BY keeps every column live, so no $select is emitted and the
    // URL is unchanged - the exact shape that skips the refetch.
    auto result = con.Query("SELECT * FROM odata_read('" + entity_url +
                            "', max_page_size=1000) ORDER BY AirlineCode");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);

    const auto requests = server.RequestsFor("/mpsfirst/Airlines");
    INFO("no request at all reached the entity set");
    REQUIRE_FALSE(requests.empty());

    INFO("first request Prefer: [" << requests.front().Header("Prefer") << "]");
    REQUIRE(requests.front().Header("Prefer").find("odata.maxpagesize=1000") !=
            std::string::npos);
}

// Raised by an agent-crew review (F12). NULL is a distinct caller mistake from zero and
// deserves its own message: reporting "must be greater than 0" for a NULL misdescribes
// what the caller actually wrote.
TEST_CASE("odata_read rejects a NULL page size with its own message", "[odata_maxpagesize]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/nullmps/Airlines");
    const std::string context = server.Url("/nullmps/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/nullmps/$metadata", "edm_trippin.xml");
    server.OnPath("/nullmps/Airlines", CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result =
        con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "', max_page_size=NULL)");
    REQUIRE(result->HasError());
    INFO(result->GetError());
    REQUIRE(result->GetError().find("NULL") != std::string::npos);
    // A caller mistake, not a broken invariant of ours.
    REQUIRE(result->GetErrorObject().Type() != duckdb::ExceptionType::INTERNAL);
}
