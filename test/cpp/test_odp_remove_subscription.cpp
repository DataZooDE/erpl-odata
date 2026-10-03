// GitHub #253: odp_odata_remove_subscription skipped an unknown subscription id with a log line, so
// a typo reported success; its audit row claimed an HTTP 200 for an operation that never touches the
// network; and its header said it removed the remote subscription when it only ever forgets one
// locally (docs/ODP.md has always said so).

#include "catch.hpp"
#include "duckdb.hpp"

#include "odp_subscription_repository.hpp"
#include "odp_test_db.hpp"

using erpl_web::OdpSubscriptionRepository;

namespace {

std::string CreateSubscription(OdpSubscriptionRepository &repository, const std::string &entity_set)
{
    return repository.CreateSubscription("https://test.example/sap/opu/odata/sap/TEST_SRV/" + entity_set,
                                         entity_set, "secret");
}

int64_t Count(duckdb::Connection &con, const std::string &sql)
{
    auto result = con.Query(sql);
    REQUIRE_FALSE(result->HasError());
    return result->GetValue(0, 0).GetValue<int64_t>();
}

}  // namespace

TEST_CASE("removing an unknown subscription id is an error", "[odp_remove_subscription]") {
    odp_test::TempDatabase database;
    duckdb::Connection &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    OdpSubscriptionRepository repository(database.Context());
    const auto known = CreateSubscription(repository, "Facts");

    auto result = con.Query("PRAGMA odp_odata_remove_subscription('no_such_subscription')");
    REQUIRE(result->HasError());
    CHECK(result->GetErrorType() == duckdb::ExceptionType::INVALID_INPUT);
    CHECK(result->GetError().find("no_such_subscription") != std::string::npos);

    // All or nothing: a list with one unknown id must not remove the known one.
    auto mixed = con.Query("PRAGMA odp_odata_remove_subscription(['" + known + "', 'no_such_subscription'])");
    REQUIRE(mixed->HasError());
    CHECK(Count(con, "SELECT count(*) FROM odp_odata_list_subscriptions()") == 1);
}

TEST_CASE("removing a subscription forgets it locally and audits it honestly", "[odp_remove_subscription]") {
    odp_test::TempDatabase database;
    duckdb::Connection &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    OdpSubscriptionRepository repository(database.Context());
    const auto id = CreateSubscription(repository, "Facts");

    auto removed = con.Query("PRAGMA odp_odata_remove_subscription('" + id + "')");
    INFO((removed->HasError() ? removed->GetError() : std::string()));
    REQUIRE_FALSE(removed->HasError());

    CHECK(Count(con, "SELECT count(*) FROM odp_odata_list_subscriptions()") == 0);

    // No request was sent to SAP, so there is no HTTP status to record.
    CHECK(Count(con, "SELECT count(*) FROM erpl_web.odp_subscription_audit WHERE subscription_id = '" + id +
                         "' AND operation_type = 'subscription_removed' AND http_status_code IS NULL") == 1);
}

TEST_CASE("keep_local_data marks the subscription terminated instead of deleting it",
          "[odp_remove_subscription]") {
    odp_test::TempDatabase database;
    duckdb::Connection &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    OdpSubscriptionRepository repository(database.Context());
    const auto id = CreateSubscription(repository, "Facts");

    auto result = con.Query("PRAGMA odp_odata_remove_subscription('" + id + "', true)");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    CHECK(Count(con, "SELECT count(*) FROM odp_odata_list_subscriptions() WHERE subscription_id = '" + id +
                         "' AND subscription_status = 'terminated'") == 1);
}
