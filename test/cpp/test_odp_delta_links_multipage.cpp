// GitHub #250: the #169 fallback that recovers a delta token from DeltaLinksOf<EntitySet> only ever
// ran for a single-page initial load. It is gated on `preference_applied && !has_more_pages`, but
// preference_applied is only set for the "initial_load" request - the first page - and the last page
// of a multi-page extraction is a "next_page" request, so the flag is never set on the one page where
// !has_more_pages finally holds. Every multi-page extraction from such a service then silently
// re-extracted everything on the next read.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odp_test_db.hpp"

#include <string>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {

const std::string SERVICE = "/sap/opu/odata/sap/Z_TEST_SRV";
const std::string ENTITY_SET = "FactsOf0D_NW_C01";

std::string Rows(int first, int count)
{
    std::string rows;
    for (int i = first; i < first + count; i++) {
        rows += std::string(rows.empty() ? "" : ",") + R"({"D_NW_DIV":")" + std::to_string(i) +
                R"(","ODQ_CHANGEMODE":"C"})";
    }
    return rows;
}

// An initial load that spans two pages, from a service that never puts __delta in a body but does
// populate DeltaLinksOf<EntitySet> (the shape #169 describes).
void ServeTwoPageExtractionWithoutBodyDelta(ODataTestServer &server)
{
    const std::string entity_path = SERVICE + "/" + ENTITY_SET;
    server.ServeMetadataFixture(SERVICE + "/$metadata", "edm_sap_odp_bw_fact.xml");
    server.ServeMetadataFixture(entity_path + "/$metadata", "edm_sap_odp_bw_fact.xml");

    // A request carrying the token is the follow-up read; it reports no changes.
    server.OnMatch(
        [entity_path](const RecordedRequest &request) {
            return request.path == entity_path && request.query.find("deltatoken") != std::string::npos;
        },
        CannedResponse::Json(R"({"d":{"results":[]}})"));

    // Page 2: the last page. No __next, and no __delta.
    server.OnMatch(
        [entity_path](const RecordedRequest &request) {
            return request.path == entity_path && request.query.find("$skiptoken=2") != std::string::npos;
        },
        CannedResponse::Json(R"({"d":{"results":[)" + Rows(2, 2) + R"(]}})"));

    // Page 1: change tracking confirmed, a next link, and no __delta.
    server.OnPath(entity_path,
                  CannedResponse::Json(R"({"d":{"results":[)" + Rows(0, 2) + R"(],"__next":")" +
                                       server.Url(entity_path) + R"(?$format=json&$skiptoken=2"}})")
                      .WithHeader("Preference-Applied", "odata.track-changes"));

    server.OnPath(SERVICE + "/DeltaLinksOf" + ENTITY_SET,
                  CannedResponse::Json(R"({"d":{"results":[{"DeltaToken":"D_RECOVERED_MULTI",)"
                                       R"("IsInitialLoad":"True"}]}})"));
}

}  // namespace

TEST_CASE("a multi-page initial load recovers its delta token from DeltaLinksOf",
          "[odp_delta_links][multipage]") {
    ODataTestServer server;
    ServeTwoPageExtractionWithoutBodyDelta(server);

    odp_test::TempDatabase database("odp_250_multipage");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const std::string url = server.Url(SERVICE + "/" + ENTITY_SET);

    auto read = con.Query("SELECT COUNT(*) FROM odp_odata_read('" + url + "')");
    INFO((read->HasError() ? read->GetError() : std::string()));
    REQUIRE_FALSE(read->HasError());
    REQUIRE(read->GetValue(0, 0).GetValue<int64_t>() == 4);

    auto state = con.Query("SELECT delta_token FROM erpl_web.odp_subscriptions");
    REQUIRE_FALSE(state->HasError());
    REQUIRE(state->RowCount() == 1);
    INFO("stored delta token: '" << state->GetValue(0, 0).ToString() << "'");
    CHECK(state->GetValue(0, 0).ToString() == "D_RECOVERED_MULTI");
}

TEST_CASE("the read after a multi-page initial load is a delta, not a second full extraction",
          "[odp_delta_links][multipage]") {
    ODataTestServer server;
    ServeTwoPageExtractionWithoutBodyDelta(server);

    odp_test::TempDatabase database("odp_250_followup");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const std::string url = server.Url(SERVICE + "/" + ENTITY_SET);

    REQUIRE_FALSE(con.Query("SELECT COUNT(*) FROM odp_odata_read('" + url + "')")->HasError());

    server.ClearRequests();
    auto second = con.Query("SELECT COUNT(*) FROM odp_odata_read('" + url + "')");
    INFO((second->HasError() ? second->GetError() : std::string()));
    REQUIRE_FALSE(second->HasError());

    bool sent_token = false;
    for (const auto &request : server.RequestsFor(SERVICE + "/" + ENTITY_SET)) {
        sent_token = sent_token || request.query.find("deltatoken") != std::string::npos;
    }
    INFO("without a recovered token the second read re-extracts everything");
    CHECK(sent_token);
    CHECK(second->GetValue(0, 0).GetValue<int64_t>() == 0);
}
