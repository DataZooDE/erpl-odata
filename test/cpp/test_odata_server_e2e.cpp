// End-to-end tests for odata_read() against a local HTTP server.
//
// These exercise the real HTTP client, the real OData probe/metadata/parse path
// and the real pagination loop -- nothing in the extension is stubbed. Only the
// remote service is replaced, by ODataTestServer, so that OData and SAP ODP wire
// behaviour can be tested without a live SAP system (GitHub #103).

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"

#include <string>
#include <vector>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeOdpDeltaPage;
using erpl_web::test_support::MakeV2Page;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {


bool AnyRequestHasQueryParam(const std::vector<RecordedRequest> &requests, const std::string &name)
{
    for (const auto &request : requests) {
        if (request.HasQueryParam(name)) {
            return true;
        }
    }
    return false;
}

bool AnyRequestQueryContains(const std::vector<RecordedRequest> &requests,
                             const std::string &needle)
{
    for (const auto &request : requests) {
        if (request.query.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const char *const AIRLINE_AA = R"({"AirlineCode":"AA","Name":"American Airlines"})";
const char *const AIRLINE_FM = R"({"AirlineCode":"FM","Name":"Shanghai Airline"})";
const char *const AIRLINE_MU = R"({"AirlineCode":"MU","Name":"China Eastern Airlines"})";
const char *const AIRLINE_AF = R"({"AirlineCode":"AF","Name":"Air France"})";
const char *const AIRLINE_KL = R"({"AirlineCode":"KL","Name":"KLM"})";

}  // namespace

// ----------------------------------------------------------------------
// Harness self-checks

TEST_CASE("ODataTestServer binds an ephemeral loopback port", "[odata_e2e][harness]") {
    ODataTestServer first;
    ODataTestServer second;

    REQUIRE(first.Port() > 0);
    REQUIRE(second.Port() > 0);
    // Ephemeral ports guarantee parallel test runs do not collide.
    REQUIRE(first.Port() != second.Port());
    REQUIRE(first.BaseUrl() == std::string("http://127.0.0.1:") + std::to_string(first.Port()));
}

// ----------------------------------------------------------------------
// Entity set reads

// Catches: a regression in v4 page parsing, metadata resolution, or the shape of
// the URL we put on the wire (the probe must ask for JSON and must hit exactly
// the entity set the user named).
TEST_CASE("odata_read reads an OData v4 entity set end to end", "[odata_e2e][v4]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/v4/$metadata", "edm_trippin.xml");
    server.OnPath("/v4/Airlines",
                  CannedResponse::Json(MakeV4Page(server.Url("/v4/$metadata") + "#Airlines",
                                                  {AIRLINE_AA, AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT AirlineCode, Name FROM odata_read('" +
                            server.Url("/v4/Airlines") + "') ORDER BY AirlineCode");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);
    REQUIRE(result->GetValue(0, 0).GetValue<std::string>() == "AA");
    REQUIRE(result->GetValue(1, 0).GetValue<std::string>() == "American Airlines");
    REQUIRE(result->GetValue(0, 1).GetValue<std::string>() == "FM");

    // Assert on what was REQUESTED, not just on what came back.
    REQUIRE(server.RequestsFor("/v4/$metadata").size() >= 1);
    const auto data_requests = server.RequestsFor("/v4/Airlines");
    REQUIRE(data_requests.size() >= 1);
    REQUIRE(data_requests.front().method == "GET");
    REQUIRE(data_requests.front().DecodedQueryParam("$format") == "json");
}

// Catches: v2 ("d"/"results") payload parsing regressions and v2 metadata
// (EDMX 1.0) handling.
TEST_CASE("odata_read reads an OData v2 entity set end to end", "[odata_e2e][v2]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/v2/$metadata", "edm_northwind_v2.xml");
    server.OnPath("/v2/Regions",
                  CannedResponse::Json(MakeV2Page({R"({"RegionID":1,"RegionDescription":"Eastern"})",
                                                   R"({"RegionID":2,"RegionDescription":"Western"})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT RegionID, RegionDescription FROM odata_read('" +
                            server.Url("/v2/Regions") + "') ORDER BY RegionID");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);
    REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 1);
    REQUIRE(result->GetValue(1, 0).GetValue<std::string>() == "Eastern");
    REQUIRE(result->GetValue(0, 1).GetValue<int32_t>() == 2);

    REQUIRE(server.RequestsFor("/v2/$metadata").size() >= 1);
    REQUIRE(server.RequestsFor("/v2/Regions").size() >= 1);
}

// ----------------------------------------------------------------------
// Pagination

// Catches: silent truncation -- a scan that stops after the first page, or that
// never follows @odata.nextLink at all. Both are data-loss defects.
TEST_CASE("odata_read follows every @odata.nextLink page", "[odata_e2e][paging]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/pg/Airlines");
    const std::string context = server.Url("/pg/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/pg/$metadata", "edm_trippin.xml");

    // Route on the skiptoken rather than on call order, so the assertions hold
    // no matter how often the first page is (re-)fetched during binding.
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/pg/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF},
                                        entity_url + "?$format=json&$skiptoken=3")));
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/pg/Airlines" && request.QueryParam("$skiptoken") == "3";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_KL})));
    server.OnPath("/pg/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT AirlineCode FROM odata_read('" + entity_url +
                            "') ORDER BY AirlineCode");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 5);

    const auto requests = server.RequestsFor("/pg/Airlines");
    REQUIRE(AnyRequestQueryContains(requests, "$skiptoken=2"));
    REQUIRE(AnyRequestQueryContains(requests, "$skiptoken=3"));
}

// The paging test above selects a single column, which makes the pushdown helper
// append a $select and therefore change the request URL. That difference is what
// kept this bug hidden: the buffered first page is only discarded when the URL
// changes, but the OData client is rebuilt unconditionally. With every column
// selected and no filter, the URL is identical, so the scan kept the first page
// and got a brand-new client with no current_response to advance from --
// pagination stopped after page one and the rest of the entity set was dropped
// without a word. `SELECT *` is the most ordinary query there is; against a real
// SAP service it returned 100 of 4136 rows.
TEST_CASE("odata_read follows every page when all columns are selected",
          "[odata_e2e][paging]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/pgall/Airlines");
    const std::string context = server.Url("/pgall/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/pgall/$metadata", "edm_trippin.xml");

    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/pgall/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF},
                                        entity_url + "?$format=json&$skiptoken=3")));
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/pgall/Airlines" && request.QueryParam("$skiptoken") == "3";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_KL})));
    server.OnPath("/pgall/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // ORDER BY over the star keeps every column live, so the projection cannot be
    // pruned back down to one and no $select is emitted.
    auto result = con.Query("SELECT * FROM odata_read('" + entity_url + "') ORDER BY AirlineCode");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 5);

    const auto requests = server.RequestsFor("/pgall/Airlines");
    REQUIRE(AnyRequestQueryContains(requests, "$skiptoken=2"));
    REQUIRE(AnyRequestQueryContains(requests, "$skiptoken=3"));
}

// GitHub #75 + #149 together. The re-execution guarantee and the all-columns paging fix
// pull in opposite directions: CloneForScan hands every execution the SAME
// shared_ptr<ODataEntitySetClient>, and the only thing that ever gave a scan a private
// client was the unconditional rebuild inside UpdateUrlFromPredicatePushdown. Returning
// early from that rebuild when the URL is unchanged - the exact `SELECT *` case #149 fixes
// - would hand two executions one mutable pagination cursor.
//
// Every existing re-execution case projects (COUNT(*), COUNT(DISTINCT ...)), which emits a
// $select and so still takes the rebuild path. These two do not, which is why the shape
// was uncovered.
TEST_CASE("a bound SELECT * plan returns every row on re-execution", "[odata_e2e][paging]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/reexec/Airlines");
    const std::string context = server.Url("/reexec/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/reexec/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/reexec/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath("/reexec/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(con.Query("PREPARE p AS SELECT * FROM odata_read('" + entity_url + "')")->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE p");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 4);
    }
}

TEST_CASE("two SELECT * scans of one call each see every row", "[odata_e2e][paging]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/selfjoin/Airlines");
    const std::string context = server.Url("/selfjoin/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/selfjoin/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/selfjoin/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath("/selfjoin/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // A cross join of two unprojected scans: 4 x 4. Sharing one cursor between them makes
    // this collapse.
    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "') a, odata_read('" +
                            entity_url + "') b");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 16);
}

// The OData clients wrapped their HTTP client in CachingHttpClient, a process-wide 30s
// response cache keyed on method + URL + body hash - credentials are NOT part of the key.
// Nothing that repeats ever reached it (DoMetadataHttpGet bypasses it because the EDM is
// cached separately, and ProbeUrl builds its own bare client), so its only traffic was
// nextLink pages, each fetched once: a 0% hit rate with 100% retention. The one time the
// key did match was the dangerous one - the same page URL fetched under two different
// credentials, where the second caller was served the first caller's rows.
TEST_CASE("a page fetched under different credentials is not served from cache",
          "[odata_e2e][security]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/creds/Airlines");
    const std::string context = server.Url("/creds/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/creds/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/creds/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath("/creds/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    const std::string scope = server.BaseUrl();
    for (const std::string &user : {std::string("alice"), std::string("bob")}) {
        REQUIRE_FALSE(con.Query("DROP SECRET IF EXISTS s")->HasError());
        auto created = con.Query("CREATE SECRET s (TYPE http_basic, USERNAME '" + user +
                                 "', PASSWORD '" + user + "-pw', SCOPE '" + scope + "')");
        INFO((created->HasError() ? created->GetError() : std::string()));
        REQUIRE_FALSE(created->HasError());

        auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "')");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 4);
    }

    // The second page is the one that goes through the client under test. Both reads must
    // have fetched it themselves, under their own Authorization header.
    std::vector<std::string> page_two_authorizations;
    for (const auto &request : server.RequestsFor("/creds/Airlines")) {
        if (request.QueryParam("$skiptoken") == "2") {
            page_two_authorizations.push_back(request.Header("Authorization"));
        }
    }
    REQUIRE(page_two_authorizations.size() == 2);
    REQUIRE(page_two_authorizations[0] != page_two_authorizations[1]);
}

// Catches GitHub #93: a follow-up page that errors must fail the scan. Swallowing
// the error would hand the user a silently truncated result set, which is worse
// than no result at all.
TEST_CASE("odata_read fails loudly when a next-link page errors", "[odata_e2e][paging][error]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/err/Airlines");
    const std::string context = server.Url("/err/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/err/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/err/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Error(500, R"({"error":{"code":"boom","message":"page 2 exploded"}})"));
    server.OnPath("/err/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + entity_url + "')");
    REQUIRE(result->HasError());

    // The second page really was attempted -- the failure is not a bind failure.
    REQUIRE(AnyRequestQueryContains(server.RequestsFor("/err/Airlines"), "$skiptoken=2"));
}

// Catches: treating an empty page as end-of-stream. A server is allowed to return
// an empty value array together with a next link; aborting there drops every row
// on the pages that follow.
TEST_CASE("odata_read keeps paging through an empty page that has a next link",
          "[odata_e2e][paging]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/empty/Airlines");
    const std::string context = server.Url("/empty/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/empty/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/empty/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {}, entity_url + "?$format=json&$skiptoken=3")));
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/empty/Airlines" && request.QueryParam("$skiptoken") == "3";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath("/empty/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + entity_url + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 4);

    REQUIRE(AnyRequestQueryContains(server.RequestsFor("/empty/Airlines"), "$skiptoken=3"));
}

// Catches GitHub #79 (v4 branch): a page that omits the "value" array altogether while carrying
// a next link. Graph delta and skip-token pages legitimately do this. Before the fix the missing
// array made ToRows() throw "No value array found in OData response", which aborted the whole
// scan and dropped every row on the pages that follow.
TEST_CASE("odata_read keeps paging when a v4 page omits the value array entirely",
          "[odata_e2e][paging]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/novalue/Airlines");
    const std::string context = server.Url("/novalue/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/novalue/$metadata", "edm_trippin.xml");
    // Page 2 has a next link but no "value" member at all.
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/novalue/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json("{\"@odata.context\":\"" + context + "\",\"@odata.nextLink\":\"" +
                             entity_url + "?$format=json&$skiptoken=3\"}"));
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/novalue/Airlines" && request.QueryParam("$skiptoken") == "3";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath("/novalue/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + entity_url + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 4);

    // The page AFTER the value-less one really was fetched.
    REQUIRE(AnyRequestQueryContains(server.RequestsFor("/novalue/Airlines"), "$skiptoken=3"));
}

// Catches GitHub #79 (v2 branch): the same defect on the OData v2 side, where the abort came from
// a throw inside GetValueArray() ("'d' element ... is not an array or doesn't contain a 'results'
// array") rather than from the nullptr path. A v4-only fix leaves half the bug in place.
TEST_CASE("odata_read keeps paging when a v2 page omits the results array",
          "[odata_e2e][paging][v2]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/v2empty/Regions");

    server.ServeMetadataFixture("/v2empty/$metadata", "edm_northwind_v2.xml");
    // Page 2 is a "d" wrapper with a __next link and no results array.
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/v2empty/Regions" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json("{\"d\":{\"__next\":\"" + entity_url +
                             "?$format=json&$skiptoken=3\"}}"));
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/v2empty/Regions" && request.QueryParam("$skiptoken") == "3";
        },
        CannedResponse::Json(MakeV2Page({R"({"RegionID":3,"RegionDescription":"Northern"})"})));
    server.OnPath("/v2empty/Regions",
                  CannedResponse::Json(MakeV2Page({R"({"RegionID":1,"RegionDescription":"Eastern"})",
                                                   R"({"RegionID":2,"RegionDescription":"Western"})"},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT count(*) FROM odata_read('" + entity_url + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 3);

    REQUIRE(AnyRequestQueryContains(server.RequestsFor("/v2empty/Regions"), "$skiptoken=3"));
}

// Catches GitHub #77: a SAP Gateway style error payload answered with HTTP 200. The scan must
// fail with the code and message the SERVICE reported; before the fix the missing value array
// produced the generic "No value array found in OData response" and the server's own diagnosis
// was discarded.
TEST_CASE("odata_read surfaces the service's own error code and message",
          "[odata_e2e][error]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/svcerr/Airlines");

    server.ServeMetadataFixture("/svcerr/$metadata", "edm_trippin.xml");
    server.OnPath("/svcerr/Airlines",
                  CannedResponse::Json(R"({"error":{"code":"SY/530","message":{"lang":"en",)"
                                       R"("value":"Invalid filter on property Foo"}}})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM odata_read('" + entity_url + "')");
    REQUIRE(result->HasError());
    const auto message = result->GetError();
    INFO(message);
    REQUIRE(message.find("SY/530") != std::string::npos);
    REQUIRE(message.find("Invalid filter on property Foo") != std::string::npos);
}

// ----------------------------------------------------------------------
// What we put on the wire

// Catches: projection and filter pushdown silently regressing to "fetch
// everything". Reading the whole entity set and filtering locally still returns
// the right answer, so only an assertion on the REQUEST can detect it.
TEST_CASE("odata_read pushes projection and filter into the request URL",
          "[odata_e2e][pushdown]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/pd/$metadata", "edm_trippin.xml");
    server.OnPath("/pd/Airlines",
                  CannedResponse::Json(MakeV4Page(server.Url("/pd/$metadata") + "#Airlines",
                                                  {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT AirlineCode FROM odata_read('" + server.Url("/pd/Airlines") +
                            "') WHERE AirlineCode = 'AA'");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);

    const auto requests = server.RequestsFor("/pd/Airlines");
    REQUIRE(AnyRequestHasQueryParam(requests, "$select"));
    REQUIRE(AnyRequestHasQueryParam(requests, "$filter"));

    std::string select_value;
    std::string filter_value;
    for (const auto &request : requests) {
        if (request.HasQueryParam("$select")) {
            select_value = request.DecodedQueryParam("$select");
        }
        if (request.HasQueryParam("$filter")) {
            filter_value = request.DecodedQueryParam("$filter");
        }
    }
    REQUIRE(select_value.find("AirlineCode") != std::string::npos);
    REQUIRE(select_value.find("Name") == std::string::npos);
    REQUIRE(filter_value.find("AirlineCode") != std::string::npos);
    REQUIRE(filter_value.find("eq") != std::string::npos);
    REQUIRE(filter_value.find("AA") != std::string::npos);
}

// Catches: a $top named parameter that never reaches the service. The scan would
// still look right locally while transferring the entire dataset.
TEST_CASE("odata_read pushes the top named parameter into the request URL",
          "[odata_e2e][pushdown]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/top/$metadata", "edm_trippin.xml");
    server.OnPath("/top/Airlines",
                  CannedResponse::Json(MakeV4Page(server.Url("/top/$metadata") + "#Airlines",
                                                  {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM odata_read('" + server.Url("/top/Airlines") +
                            "', top = 1)");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);

    const auto requests = server.RequestsFor("/top/Airlines");
    bool saw_top = false;
    for (const auto &request : requests) {
        if (request.DecodedQueryParam("$top") == "1") {
            saw_top = true;
        }
    }
    REQUIRE(saw_top);
}

// ----------------------------------------------------------------------
// SAP ODP shapes
//
// The extension's ODP entry points need a subscription secret and repository, so
// this covers the payload and header fidelity of the harness itself: the delta
// page shape and the Preference-Applied header that ODP change tracking relies
// on. It is the building block an end-to-end ODP test needs; the ODP read
// function itself is not yet driven from here (see the report).

TEST_CASE("ODataTestServer serves a SAP ODP delta page with a !deltatoken link",
          "[odata_e2e][odp]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/odp/Entity");
    const std::string delta_link = entity_url + "?$format=json&!deltatoken=D20240101120000";

    server.ServeMetadataFixture("/odp/$metadata", "edm_sap_odp_bw_fact.xml");
    server.OnPath("/odp/Entity",
                  MakeOdpDeltaPage({R"({"KeyField":"1","ValueField":"10"})"}, delta_link));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // Drive the plain HTTP function so the assertion is about the wire shape and
    // not about ODP subscription bookkeeping.
    auto result = con.Query("SELECT status, content FROM http_get('" + entity_url + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);

    const auto content = result->GetValue(1, 0).GetValue<std::string>();
    REQUIRE(content.find("__delta") != std::string::npos);
    REQUIRE(content.find("!deltatoken=D20240101120000") != std::string::npos);
    REQUIRE(content.find("\"results\"") != std::string::npos);

    REQUIRE(server.RequestsFor("/odp/Entity").size() == 1);
}

// Catches: a Prefer header that never leaves the client. Recording it server-side
// is the only way to prove change tracking was actually requested.
TEST_CASE("ODataTestServer records the Prefer header sent by the client",
          "[odata_e2e][odp][harness]") {
    ODataTestServer server;
    server.OnPath("/odp/Entity", MakeOdpDeltaPage({}, server.Url("/odp/Entity") +
                                                          "?$format=json&!deltatoken=D1"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query(
        "SELECT status FROM http_get('" + server.Url("/odp/Entity") +
        "', headers = MAP {'Prefer': 'odata.track-changes,odata.maxpagesize=1000'})");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    const auto requests = server.RequestsFor("/odp/Entity");
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().Header("Prefer").find("odata.track-changes") != std::string::npos);
    REQUIRE(requests.front().Header("Prefer").find("odata.maxpagesize=1000") != std::string::npos);
}

// ----------------------------------------------------------------------
// Failure modes

// Catches: a bind that treats an unreachable or erroring service as an empty
// result set instead of an error.
TEST_CASE("odata_read fails when the entity set itself errors", "[odata_e2e][error]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/bad/$metadata", "edm_trippin.xml");
    server.OnPath("/bad/Airlines", CannedResponse::Error(500));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM odata_read('" + server.Url("/bad/Airlines") + "')");
    REQUIRE(result->HasError());
}

// ----------------------------------------------------------------------
// Decimal facets (GitHub #254)

namespace {

// A service that publishes its money columns the way TOPdesk's reporting API does:
// Edm.Decimal with a declared Scale and NO Precision. CSDL leaves an absent Precision
// unbounded, so the only conformant reading is "as wide as the consumer can manage" --
// defaulting it to the scale itself yields DECIMAL(18,18), which has zero integral
// digits and cannot hold any value >= 1.
std::string DecimalEdmx(const char *const scale_facets)
{
    return std::string(R"(<?xml version="1.0" encoding="utf-8"?>
<edmx:Edmx Version="4.0" xmlns:edmx="http://docs.oasis-open.org/odata/ns/edmx">
  <edmx:DataServices>
    <Schema Namespace="Topdesk" xmlns="http://docs.oasis-open.org/odata/ns/edm">
      <EntityType Name="Asset">
        <Key><PropertyRef Name="Id"/></Key>
        <Property Name="Id" Type="Edm.String" Nullable="false"/>
        <Property Name="serviceagreementcost" Type="Edm.Decimal" )") +
           scale_facets + R"(/>
      </EntityType>
      <EntityContainer Name="Container">
        <EntitySet Name="AssetList" EntityType="Topdesk.Asset"/>
      </EntityContainer>
    </Schema>
  </edmx:DataServices>
</edmx:Edmx>)";
}

const char *const ASSET_490 = R"({"Id":"a1","serviceagreementcost":490.0})";
const char *const ASSET_1250 = R"({"Id":"a2","serviceagreementcost":1250.75})";

}  // namespace

// Catches: GitHub #254. Every decimal column of a TOPdesk reporting service came back
// as DECIMAL(18,18), so every value >= 1 failed to cast and was returned as NULL with a
// conversion warning. Nothing here is stubbed but the service itself: the metadata is
// parsed, the catalog is built and the JSON page is converted by the real code.
TEST_CASE("a Decimal with no Precision holds values above 1 end to end",
          "[odata_e2e][decimal]") {
    ODataTestServer server;
    server.ServeMetadata("/topdesk/$metadata", DecimalEdmx(R"(Scale="18")"));
    // ATTACH resolves the metadata context from the service document at the root.
    server.OnPath("/topdesk/",
                  CannedResponse::Json(
                      R"({"@odata.context":")" + server.Url("/topdesk/$metadata") +
                      R"(","value":[{"name":"AssetList","kind":"EntitySet","url":"AssetList"}]})"));
    server.OnPath("/topdesk/AssetList",
                  CannedResponse::Json(MakeV4Page(server.Url("/topdesk/$metadata") + "#AssetList",
                                                  {ASSET_490, ASSET_1250})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    SECTION("through ATTACH, as reported") {
        auto attach = con.Query("ATTACH '" + server.Url("/topdesk/") + "' AS topdesk (TYPE odata)");
        INFO((attach->HasError() ? attach->GetError() : std::string()));
        REQUIRE_FALSE(attach->HasError());

        auto result = con.Query(
            "SELECT DISTINCT serviceagreementcost FROM topdesk.AssetList "
            "ORDER BY serviceagreementcost");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->types.size() == 1);
        INFO("column type " << result->types[0].ToString());
        REQUIRE(result->types[0].ToString() == "DECIMAL(38,18)");
        REQUIRE(result->RowCount() == 2);
        REQUIRE_FALSE(result->GetValue(0, 0).IsNull());
        REQUIRE_FALSE(result->GetValue(0, 1).IsNull());
        REQUIRE(result->GetValue(0, 0).GetValue<double>() == Approx(490.0));
        REQUIRE(result->GetValue(0, 1).GetValue<double>() == Approx(1250.75));
    }

    SECTION("through odata_read") {
        auto result = con.Query("SELECT serviceagreementcost FROM odata_read('" +
                                server.Url("/topdesk/AssetList") + "') ORDER BY Id");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->types[0].ToString() == "DECIMAL(38,18)");
        REQUIRE(result->RowCount() == 2);
        REQUIRE_FALSE(result->GetValue(0, 0).IsNull());
        REQUIRE(result->GetValue(0, 0).GetValue<double>() == Approx(490.0));
    }
}
