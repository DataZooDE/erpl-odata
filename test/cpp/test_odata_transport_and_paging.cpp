// Transport fidelity and pagination safety, driven end to end against the local
// HTTP test server so that the assertions are about the bytes and the request
// count the extension actually produced.
//
//  * GitHub #126 -- httplib's client form-urlencodes the whole query on its way
//    to the socket, turning the "%20" the extension composed into "+". Only a
//    server-side recording of the RAW request target can catch that, because
//    every layer we own still looks correct.
//  * GitHub #78  -- server-driven paging followed whatever next link came back
//    without ever comparing it to the URL that produced it, so a service echoing
//    the same link paged forever.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"

#include <string>
#include <vector>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {

// The jemalloc/background-thread option is mandatory: a bare DuckDB(nullptr)
// crashes in DEBUG builds shortly after the first query (see CLAUDE.md).
// DBConfig is neither copyable nor movable, hence the small RAII wrapper.
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

const char *const AIRLINE_AA = R"({"AirlineCode":"AA","Name":"American Airlines"})";
const char *const AIRLINE_FM = R"({"AirlineCode":"FM","Name":"Shanghai Airline"})";

// A next link that keeps pointing at the same URL would page forever before the
// fix. Cap the canned sequence so a regression fails the suite in bounded time
// instead of hanging CI: after this many pages the service starts erroring, which
// terminates the scan with a *different* (and clearly wrong) message.
constexpr std::size_t LOOP_PAGE_BUDGET = 50;

}  // namespace

// ----------------------------------------------------------------------
// GitHub #126 -- the bytes we composed must be the bytes on the wire

// Catches: httplib re-encoding the query as application/x-www-form-urlencoded.
// The extension percent-encodes the filter itself (%20 for a space, %27 for a
// quote); anything that decodes and re-encodes it turns those into '+' and a
// bare quote. services.odata.org and Microsoft Graph tolerate the form spelling,
// so no existing test notices -- a strict service (SAP Gateway) need not.
//
// Deliberately reads the RAW QueryParam(): the encoding itself is what is under
// test, so the harness's form-decoding read-back would hide the defect.
TEST_CASE("odata_read puts percent-encoded filter bytes on the wire",
          "[odata_e2e][transport][encoding]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/enc/$metadata", "edm_trippin.xml");
    server.OnPath("/enc/Airlines",
                  CannedResponse::Json(MakeV4Page(server.Url("/enc/$metadata") + "#Airlines",
                                                  {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT AirlineCode FROM odata_read('" + server.Url("/enc/Airlines") +
                            "') WHERE AirlineCode = 'AA'");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    std::string raw_filter;
    std::string raw_query;
    for (const auto &request : server.RequestsFor("/enc/Airlines")) {
        if (request.HasQueryParam("$filter")) {
            raw_filter = request.QueryParam("$filter");
            raw_query = request.query;
        }
    }

    INFO("raw query: " << raw_query);
    REQUIRE_FALSE(raw_filter.empty());
    // ODataUrlCodec::encodeFilterExpression writes every non-unreserved byte as a
    // percent escape, so this is what the server must see -- not "+eq+" and not a
    // bare quote.
    REQUIRE(raw_filter.find("%20eq%20") != std::string::npos);
    REQUIRE(raw_filter.find("%27AA%27") != std::string::npos);
    REQUIRE(raw_filter.find('+') == std::string::npos);
    REQUIRE(raw_filter.find('\'') == std::string::npos);
    REQUIRE(raw_query.find('+') == std::string::npos);
}

// Catches the same defect one layer lower and without the OData stack in the
// way: http_get(url_encode := false) is the contract "I prepared these bytes,
// send them". Before the fix the flag guarded only the path half, so the nested
// '=' of an $expand option group came out as %3D and every %20 came out as '+'.
TEST_CASE("http_get with url_encode := false transmits the query verbatim",
          "[odata_e2e][transport][encoding]") {
    ODataTestServer server;
    server.OnPath("/raw/Echo", CannedResponse::Json("{}"));

    const std::string query =
        "?$filter=Country%20eq%20%27UK%27"
        "&$expand=Category($filter=CategoryName%20eq%20%27Beverages%27)";
    const std::string url = server.Url("/raw/Echo") + query;

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + url + "', url_encode := false)");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    const auto requests = server.RequestsFor("/raw/Echo");
    REQUIRE(requests.size() == 1);

    INFO("raw target: " << requests.front().target);
    // Byte-for-byte: the query we handed in is the query that arrived, options in
    // the order we wrote them.
    REQUIRE(requests.front().query == query.substr(1));
    // Spelled out so a partial regression is legible in the failure output.
    REQUIRE(requests.front().query.find("Country%20eq%20%27UK%27") != std::string::npos);
    REQUIRE(requests.front().query.find("Category($filter=CategoryName%20eq%20%27Beverages%27)") !=
            std::string::npos);
    REQUIRE(requests.front().query.find('+') == std::string::npos);
    REQUIRE(requests.front().query.find("%3D") == std::string::npos);
}

// ----------------------------------------------------------------------
// GitHub #78 -- a repeated next link must terminate with an error

// Catches: a next link that resolves back onto the URL that produced it being
// followed anyway. SAP ODP repeating a $skiptoken on a "!deltatoken" response is
// the real-world trigger. Before the fix this issued HTTP requests forever and
// could not be interrupted with Ctrl-C, which is worse than a crash: it neither
// terminates nor reports.
//
// The canned sequence stops handing out the self-referential page after
// LOOP_PAGE_BUDGET entries so that a regression fails here in bounded time
// rather than hanging the suite.
TEST_CASE("odata_read stops with an error when a next link repeats itself",
          "[odata_e2e][paging][error]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/loop/Airlines");
    const std::string context = server.Url("/loop/$metadata") + "#Airlines";
    // Fixed link: every page advertises the very same next URL.
    const std::string repeated_next = entity_url + "?$format=json&$skiptoken=stuck";

    server.ServeMetadataFixture("/loop/$metadata", "edm_trippin.xml");

    std::vector<CannedResponse> pages(
        LOOP_PAGE_BUDGET,
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM}, repeated_next)));
    // Repeated for every request past the budget -- the escape hatch that keeps a
    // regression bounded.
    pages.push_back(CannedResponse::Error(508, R"({"error":{"code":"loop_detected"}})"));
    server.OnPathSequence("/loop/Airlines", std::move(pages));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + entity_url + "')");
    REQUIRE(result->HasError());

    const auto data_requests = server.RequestsFor("/loop/Airlines");
    INFO("error: " << result->GetError());
    INFO("data requests: " << data_requests.size());
    // The loop was detected, not merely survived because the service gave up.
    REQUIRE(result->GetError().find("next link identical to the request that produced it") !=
            std::string::npos);
    // First page, then the repeated link once, then the stop. Never the budget.
    REQUIRE(data_requests.size() < LOOP_PAGE_BUDGET);
}
