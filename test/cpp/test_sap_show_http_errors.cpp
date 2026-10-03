// GitHub #261: odata_sap_show / odp_odata_show reported ordinary HTTP failures as INTERNAL
// errors. The SQL test covers a refused connection; this one drives real HTTP status
// failures through the in-process server, which a SQL test cannot do without a live service.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {


void RequireIOError(const std::string &function, int status)
{
    ODataTestServer server;
    server.OnMatch([](const RecordedRequest &) { return true; },
                   CannedResponse::Error(status, R"({"error":{"message":"catalog unavailable"}})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM " + function + "('" + server.BaseUrl() + "')");

    REQUIRE(result->HasError());
    INFO(result->GetError());
    CHECK(result->GetErrorType() == duckdb::ExceptionType::IO);
    CHECK(result->GetError().find("service discovery failed") != std::string::npos);
    CHECK(result->GetError().find(std::to_string(status)) != std::string::npos);
}

}  // namespace

TEST_CASE("odata_sap_show reports an HTTP failure as an IO error", "[sap_show_errors]") {
    for (const int status : {401, 404, 500}) {
        RequireIOError("odata_sap_show", status);
    }
}

TEST_CASE("odp_odata_show reports an HTTP failure as an IO error", "[sap_show_errors]") {
    for (const int status : {401, 404, 500}) {
        RequireIOError("odp_odata_show", status);
    }
}
