// GitHub #249: force_full_load := true was silently ignored once an active subscription held a
// delta token. Bind built its state manager with force_full_load, but CreateSubscription returned
// the existing active row untouched (token intact), and the scan runs on a clone built with
// force_full_load = false - which loaded that row, saw the token and sent !deltatoken=T. The user
// asked for a full extraction and got a delta, usually zero rows.
//
// What matters is what reaches the wire, so these tests read the requests the service received.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odp_test_db.hpp"

#include <sstream>
#include <string>
#include <vector>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeOdpDeltaPage;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {

constexpr std::size_t ROW_COUNT = 120;
const char *const SERVICE_PATH = "/sap/opu/odata/sap/Z_TEST_SRV/FactsOf0D_NW_C01";

std::vector<std::string> MakeOdpRows(std::size_t row_count)
{
    std::vector<std::string> rows;
    for (std::size_t i = 0; i < row_count; i++) {
        rows.push_back(R"({"D_NW_DIV":")" + std::to_string(i) + R"(","ODQ_CHANGEMODE":"C"})");
    }
    return rows;
}

bool CarriesDeltaToken(const RecordedRequest &request)
{
    return request.query.find("deltatoken") != std::string::npos;
}

// A delta-capable service: a request with a delta token reports no changes, anything else is a
// full load that hands back a delta link and Preference-Applied.
void ServeDeltaCapableService(ODataTestServer &server)
{
    const std::string path = SERVICE_PATH;
    server.ServeMetadataFixture("/sap/opu/odata/sap/Z_TEST_SRV/$metadata", "edm_sap_odp_bw_fact.xml");
    server.ServeMetadataFixture(path + "/$metadata", "edm_sap_odp_bw_fact.xml");
    server.OnMatch([path](const RecordedRequest &request) { return request.path == path && CarriesDeltaToken(request); },
                   CannedResponse::Json(R"({"d":{"results":[]}})"));
    server.OnPath(path, MakeOdpDeltaPage(MakeOdpRows(ROW_COUNT),
                                         server.Url(path) + "?!deltatoken=D20260912000000_000001000"));
}

int64_t Count(duckdb::Connection &con, const std::string &sql)
{
    auto result = con.Query(sql);
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    return result->GetValue(0, 0).GetValue<int64_t>();
}

}  // namespace

TEST_CASE("force_full_load := true re-extracts in full even when a delta token is stored",
          "[odp_force_full_load]") {
    ODataTestServer server;
    ServeDeltaCapableService(server);

    odp_test::TempDatabase database("odp_249_force");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const std::string url = server.Url(SERVICE_PATH);

    // Seed the token: the initial load stores the delta link.
    REQUIRE(Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "')") == static_cast<int64_t>(ROW_COUNT));
    // And prove it is there, so the forced read below is up against a real stored token.
    REQUIRE(Count(con, "SELECT COUNT(*) FROM odp_odata_list_subscriptions() WHERE delta_token <> ''") == 1);

    server.ClearRequests();
    const auto forced = Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "', force_full_load := true)");

    for (const auto &request : server.RequestsFor(SERVICE_PATH)) {
        INFO("forced read sent: " << request.target);
        CHECK_FALSE(CarriesDeltaToken(request));
    }
    CHECK(forced == static_cast<int64_t>(ROW_COUNT));
}

TEST_CASE("a forced full load asks for change tracking again", "[odp_force_full_load]") {
    ODataTestServer server;
    ServeDeltaCapableService(server);

    odp_test::TempDatabase database("odp_249_prefer");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const std::string url = server.Url(SERVICE_PATH);

    REQUIRE(Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "')") == static_cast<int64_t>(ROW_COUNT));

    server.ClearRequests();
    Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "', force_full_load := true)");

    bool asked_for_tracking = false;
    for (const auto &request : server.RequestsFor(SERVICE_PATH)) {
        if (request.Header("Prefer").find("odata.track-changes") != std::string::npos) {
            asked_for_tracking = true;
        }
    }
    INFO("a full load must request odata.track-changes so a fresh token comes back");
    CHECK(asked_for_tracking);
}

TEST_CASE("force_full_load is one-shot: the read after it resumes from the new token",
          "[odp_force_full_load]") {
    ODataTestServer server;
    ServeDeltaCapableService(server);

    odp_test::TempDatabase database("odp_249_oneshot");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const std::string url = server.Url(SERVICE_PATH);

    REQUIRE(Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "')") == static_cast<int64_t>(ROW_COUNT));
    REQUIRE(Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "', force_full_load := true)") ==
            static_cast<int64_t>(ROW_COUNT));

    server.ClearRequests();
    // Nothing changed since the forced load, so the normal read is a delta and returns nothing.
    CHECK(Count(con, "SELECT COUNT(*) FROM odp_odata_read('" + url + "')") == 0);

    bool resumed_from_token = false;
    for (const auto &request : server.RequestsFor(SERVICE_PATH)) {
        resumed_from_token = resumed_from_token || CarriesDeltaToken(request);
    }
    CHECK(resumed_from_token);
}
