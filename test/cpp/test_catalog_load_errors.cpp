// GitHub #253: LoadTables in the remote catalogs caught every metadata failure, left the catalog
// EMPTY and then marked it loaded, so an expired token, a 403 or a network blip looked exactly
// like a tenant with no tables - and the empty answer was cached for the rest of the session even
// after the cause was fixed. A failure must reach the user and must not be cached.

#include "catch.hpp"
#include "test_extension_load_sql.hpp"
#include "duckdb_compat.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "test_database.hpp"

#include <ctime>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("a Business Central catalog defaults to read-only attach", "[catalog_load_errors][bc][attach]") {
    const std::string company = "11111111-2222-3333-4444-555555555555";
    ODataTestServer server;
    TestDatabase database;
    auto &con = database.Con();
    REQUIRE_FALSE(con.Query(ErplOdataLoadSql())->HasError());
    auto secret = con.Query(
        "CREATE SECRET bc_default (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '4102444800')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto attached = con.Query("ATTACH 'bc_default' AS bc_default_catalog "
                              "(TYPE business_central, COMPANY '" + company + "')");
    INFO((attached->HasError() ? attached->GetError() : std::string()));
    REQUIRE_FALSE(attached->HasError());
    const std::vector<std::pair<std::string, std::string>> read_only_options = {
        {"bc_ro", "READ_ONLY"}, {"bc_readonly", "READONLY"},
        {"bc_rw_false", "READ_WRITE false"}, {"bc_readwrite_false", "READWRITE false"}};
    for (const auto &option : read_only_options) {
        auto result = con.Query("ATTACH 'bc_default' AS " + option.first +
                                " (TYPE business_central, COMPANY '" + company + "', " +
                                option.second + ")");
        INFO(option.second << ": " << (result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
    }
    auto writable = con.Query("ATTACH 'bc_default' AS bc_writable_catalog "
                              "(TYPE business_central, COMPANY '" + company + "', READ_WRITE)");
    REQUIRE(writable->HasError());
    REQUIRE(writable->GetError().find("read-only") != std::string::npos);
}

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
    REQUIRE_FALSE(con.Query(ErplOdataLoadSql())->HasError());

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
    CHECK(duckdb_compat::FetchRows(*recovered).at(0).at(0).GetValue<int64_t>() > 0);
}
