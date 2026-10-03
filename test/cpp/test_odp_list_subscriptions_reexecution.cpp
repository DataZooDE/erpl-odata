// GitHub #253 (the #75 class): odp_odata_list_subscriptions kept its rows on the bind data, loaded
// once by the first scan, so a re-executed prepared statement returned the list as it was then -
// a subscription created or removed in between stayed invisible. The rows belong to the
// execution, not to the plan.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odp_subscription_repository.hpp"
#include "odp_test_db.hpp"

using erpl_web::OdpSubscriptionRepository;

TEST_CASE("a bound odp_odata_list_subscriptions plan sees subscriptions created between executions",
          "[odp_list_reexec]") {
    // ODP state refuses an in-memory catalog (GitHub #92), so this needs a real database file.
    odp_test::TempDatabase database;
    duckdb::Connection &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    OdpSubscriptionRepository repository(database.Context());
    repository.EnsureSchemaExists();
    repository.EnsureTablesExist();

    auto prepared = con.Query("PREPARE listing AS SELECT count(*) FROM odp_odata_list_subscriptions()");
    INFO((prepared->HasError() ? prepared->GetError() : std::string()));
    REQUIRE_FALSE(prepared->HasError());

    auto before = con.Query("EXECUTE listing");
    REQUIRE_FALSE(before->HasError());
    REQUIRE(before->GetValue(0, 0).GetValue<int64_t>() == 0);

    repository.CreateSubscription("https://test.example/sap/opu/odata/sap/TEST_SRV/Facts", "Facts", "secret");

    auto after = con.Query("EXECUTE listing");
    INFO((after->HasError() ? after->GetError() : std::string()));
    REQUIRE_FALSE(after->HasError());
    CHECK(after->GetValue(0, 0).GetValue<int64_t>() == 1);
}
