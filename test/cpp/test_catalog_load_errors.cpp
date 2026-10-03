// GitHub #253: LoadTables in the remote catalogs caught every metadata failure, left the catalog
// EMPTY and then marked it loaded, so an expired token, a 403 or a network blip looked exactly
// like a tenant with no tables - and the empty answer was cached for the rest of the session even
// after the cause was fixed. A failure must reach the user and must not be cached.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "test_database.hpp"

#include <ctime>
#include <string>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::ReadFixture;
using erpl_web::test_support::TestDatabase;

namespace {

std::string FarFutureEpoch()
{
    return std::to_string(static_cast<long long>(std::time(nullptr)) + 24LL * 3600LL);
}

}  // namespace

TEST_CASE("a Business Central catalog reports a metadata failure and does not cache it",
          "[catalog_load_errors][bc]") {
    // A GUID short-circuits ResolveCompanyId, which would otherwise GET <base>/companies.
    const std::string company = "11111111-2222-3333-4444-555555555555";

    ODataTestServer server;
    // The first $metadata request is rejected the way an expired token is; the retry succeeds.
    server.OnPathSequence(
        "/$metadata",
        {CannedResponse::Error(401, R"({"error":{"message":"token expired"}})"),
         CannedResponse::Xml(ReadFixture("edm_business_central_min.xml"))});

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET bc (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto attach = con.Query("ATTACH 'bc' AS bccat (TYPE business_central, COMPANY '" + company + "', READ_ONLY)");
    INFO((attach->HasError() ? attach->GetError() : std::string()));
    REQUIRE_FALSE(attach->HasError());

    const std::string list_tables =
        "SELECT count(*) FROM duckdb_tables() WHERE database_name = 'bccat'";

    // 1. The 401 must not be reported as "this company has no tables".
    auto failed = con.Query(list_tables);
    INFO("first listing: " << (failed->HasError() ? failed->GetError() : failed->ToString()));
    REQUIRE(failed->HasError());
    CHECK(failed->GetError().find("401") != std::string::npos);

    // 2. The failure must not have been cached: once the service recovers the tables appear.
    auto recovered = con.Query(list_tables);
    INFO("second listing: " << (recovered->HasError() ? recovered->GetError() : recovered->ToString()));
    REQUIRE_FALSE(recovered->HasError());
    CHECK(recovered->GetValue(0, 0).GetValue<int64_t>() > 0);
}
