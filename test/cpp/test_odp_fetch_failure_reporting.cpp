// GitHub #173: when an ODP fetch fails against the service, the reader discards the
// service's own explanation and throws duckdb::InternalException("Failed to perform ODP
// data fetch").
//
// Two separate problems, both of which this file pins down:
//
//   1. The message is useless. SAP answers a refused extraction with a precise reason
//      ("Could not open data access via extraction API RODPS_REPL_ODP_OPEN",
//      RSODP_ODATA/013). That text reaches the trace log and is then thrown away, so the
//      user is told only that something failed.
//
//   2. InternalException is the wrong type, and not merely cosmetically. DuckDB treats
//      ExceptionType::INTERNAL as "this process's invariants are broken": client_context
//      calls ValidChecker::Invalidate on the whole database instance, and every later
//      query on that database - including ones with nothing to do with ODP - fails with
//      "Failure within transaction management!". A remote service answering HTTP 400 is
//      an ordinary, recoverable I/O outcome and must not take the database down with it.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odp_test_db.hpp"

#include <string>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;

namespace {

// The body SAP actually returns for a refused extraction, trimmed to what matters.
constexpr const char *SAP_REFUSAL_BODY = R"({"error":{"code":"RSODP_ODATA/013","message":{"lang":"en","value":"Could not open data access via extraction API RODPS_REPL_ODP_OPEN"}}})";

}  // namespace

TEST_CASE("a refused ODP extraction reports the service's reason and spares the database",
          "[odp_fetch_failure]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/sap/opu/odata/sap/Z_TEST_SRV/$metadata",
                                "edm_sap_odp_bw_fact.xml");
    server.ServeMetadataFixture("/sap/opu/odata/sap/Z_TEST_SRV/FactsOf0D_NW_C01/$metadata",
                                "edm_sap_odp_bw_fact.xml");
    server.OnPath("/sap/opu/odata/sap/Z_TEST_SRV/FactsOf0D_NW_C01",
                  CannedResponse::Json(SAP_REFUSAL_BODY, 400));

    odp_test::TempDatabase db("odp_fetch_failure");
    auto &con = db.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    const auto url = server.Url("/sap/opu/odata/sap/Z_TEST_SRV/FactsOf0D_NW_C01");
    auto read = con.Query("SELECT COUNT(*) FROM odp_odata_read('" + url + "')");
    REQUIRE(read->HasError());

    // 1. The service's own reason survives to the user.
    const auto message = read->GetError();
    INFO("error reported to the user: " << message);
    REQUIRE(message.find("RODPS_REPL_ODP_OPEN") != std::string::npos);

    // 2. It is not an internal error, so the database instance is still usable. A plain
    //    SELECT is the strongest evidence: it shares nothing with ODP but fails anyway
    //    once the instance has been invalidated.
    REQUIRE(read->GetErrorObject().Type() != duckdb::ExceptionType::INTERNAL);

    auto after = con.Query("SELECT 42");
    INFO((after->HasError() ? after->GetError() : std::string()));
    REQUIRE_FALSE(after->HasError());
    REQUIRE(after->GetValue(0, 0).GetValue<int32_t>() == 42);
}
