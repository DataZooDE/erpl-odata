// Guards for server-driven paging: it must terminate, and it must not monopolise
// a single scan call (GitHub #78).
//
// Like test_odata_server_e2e.cpp these run against a real loopback HTTP server,
// so the real HTTP client, the real metadata probe and the real pagination loop
// are all exercised; only the remote service is local.

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

bool AnyRequestQueryContains(const std::vector<RecordedRequest> &requests, const std::string &needle)
{
    for (const auto &request : requests) {
        if (request.query.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Total number of pages in the chain the cap tests read. Comfortably above the
// per-scan-call cap of 32 so that a capped scan cannot possibly reach the end in
// one call, and small enough that the whole chain still runs in well under a
// second over loopback.
constexpr int PAGE_COUNT = 100;

// Registers a chain of PAGE_COUNT single-row pages under `path`. Page n is
// selected by "$skiptoken=n" and links to page n+1; the last page carries no
// next link. Routing on the skiptoken rather than on call order keeps the
// assertions valid however often the first page is (re-)fetched during binding.
void ServeSingleRowPageChain(ODataTestServer &server, const std::string &path)
{
    const std::string entity_url = server.Url(path);
    const std::string context = server.Url("/cap/$metadata") + "#Airlines";

    for (int page = PAGE_COUNT; page >= 2; --page) {
        const std::string token = std::to_string(page);
        const std::string row = R"({"AirlineCode":")" + token + R"(","Name":"Airline )" + token + R"("})";
        const std::string next_link =
            (page == PAGE_COUNT) ? std::string()
                                 : entity_url + "?$format=json&$skiptoken=" + std::to_string(page + 1);

        server.OnMatch(
            [path, token](const RecordedRequest &request) {
                return request.path == path && request.QueryParam("$skiptoken") == token;
            },
            CannedResponse::Json(MakeV4Page(context, {row}, next_link)));
    }

    // Page 1: anything without a skiptoken.
    server.OnPath(path, CannedResponse::Json(MakeV4Page(
                            context, {R"({"AirlineCode":"1","Name":"Airline 1"})"},
                            entity_url + "?$format=json&$skiptoken=2")));
}

}  // namespace

// ----------------------------------------------------------------------
// Per-call page cap (GitHub #78, part 2)

// Catches: one GetData call draining an arbitrarily long next-link chain. Each
// page is a separate sequential HTTP request, so an uncapped call can occupy the
// executor for minutes -- during which nothing else, an interrupt included, can
// run. LIMIT 1 stops the scan after its first chunk, which makes the number of
// pages a SINGLE call fetched directly observable in the request recorder.
//
// Pre-fix failure: without the cap, the first (and only) scan call follows the
// chain to its end, so the recorder holds ~101 requests for /cap/Airlines
// including one carrying "$skiptoken=100" -- both REQUIREs below fail.
TEST_CASE("odata_read caps the pages fetched in a single scan call", "[odata_e2e][paging][cap]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/cap/$metadata", "edm_trippin.xml");
    ServeSingleRowPageChain(server, "/cap/Airlines");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result =
        con.Query("SELECT AirlineCode FROM odata_read('" + server.Url("/cap/Airlines") + "') LIMIT 1");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);

    const auto requests = server.RequestsFor("/cap/Airlines");
    // The cap is 32 follow-up pages per call. Allow generous slack for the bind
    // probe, the prefetch and a possible second scan call, while staying far
    // below the 100-page chain length.
    INFO("requests issued: " << requests.size());
    REQUIRE(requests.size() <= 69);
    // The end of the chain must not have been reached by a single-chunk scan.
    REQUIRE_FALSE(AnyRequestQueryContains(requests, "$skiptoken=" + std::to_string(PAGE_COUNT)));
}

// Catches: the per-call cap silently truncating a result set. Breaking out of the
// fetch loop is only safe because HasMoreResults() still reports a next page and
// the scan resumes; if that ever stops holding, rows go missing without an error.
//
// Pre-fix status: this passes before the change as well -- it is the companion
// regression guard that keeps the cap honest, not a reproducer.
TEST_CASE("odata_read still returns every row across a chain longer than the page cap",
          "[odata_e2e][paging][cap]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/cap/$metadata", "edm_trippin.xml");
    ServeSingleRowPageChain(server, "/cap/Airlines");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + server.Url("/cap/Airlines") + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == PAGE_COUNT);

    // The whole chain really was walked -- the cap only splits it across calls.
    REQUIRE(AnyRequestQueryContains(server.RequestsFor("/cap/Airlines"),
                                    "$skiptoken=" + std::to_string(PAGE_COUNT)));
}

// ----------------------------------------------------------------------
// Self-referential next link (GitHub #78, client half)

// Catches: a service whose next link points back at the request that produced it
// -- SAP ODP repeating a "!deltatoken", or an empty page that keeps re-serving
// its own link. The cursor never advances, so the scan issues the identical HTTP
// request forever.
//
// Pre-fix failure: this test does not fail, it HANGS -- the paging loop never
// terminates and the run has to be killed. That non-termination is precisely the
// defect; after the fix the query errors out within two requests.
TEST_CASE("odata_read refuses a next link identical to the request that produced it",
          "[odata_e2e][paging][loop]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/loop/Airlines");
    const std::string context = server.Url("/loop/$metadata") + "#Airlines";
    const std::string self_link = entity_url + "?$format=json&$skiptoken=2";

    server.ServeMetadataFixture("/loop/$metadata", "edm_trippin.xml");
    // Page 2 hands back its own URL as the next link: the cursor never advances.
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/loop/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(
            MakeV4Page(context, {R"({"AirlineCode":"FM","Name":"Shanghai Airline"})"}, self_link)));
    server.OnPath("/loop/Airlines",
                  CannedResponse::Json(MakeV4Page(
                      context, {R"({"AirlineCode":"AA","Name":"American Airlines"})"}, self_link)));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + entity_url + "')");
    REQUIRE(result->HasError());
    INFO(result->GetError());
    REQUIRE(result->GetError().find("identical") != std::string::npos);

    // It stopped almost immediately rather than spinning.
    REQUIRE(server.RequestsFor("/loop/Airlines").size() <= 5);
}
