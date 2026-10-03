// GitHub #251: ExceptionType::INTERNAL invalidates the whole DuckDB instance and is reserved for
// invariant violations, yet five seams still threw it for conditions a user can cause. Every test
// here asserts the two things that separate this class from an ordinary error: the error is NOT
// INTERNAL, and an unrelated `SELECT 42` still works afterwards.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odp_subscription_repository.hpp"
#include "odp_subscription_state_manager.hpp"
#include "odp_test_db.hpp"
#include "test_database.hpp"

#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

using erpl_web::OdpSubscriptionRepository;
using erpl_web::OdpSubscriptionStateManager;
using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::TestDatabase;

namespace {

// The error is an ordinary one, and the instance is still usable.
void RequireOrdinaryError(duckdb::Connection &con, duckdb::MaterializedQueryResult &result)
{
    REQUIRE(result.HasError());
    INFO(result.GetError());
    CHECK(result.GetErrorType() != duckdb::ExceptionType::INTERNAL);

    auto alive = con.Query("SELECT 42");
    INFO((alive->HasError() ? alive->GetError() : std::string()));
    REQUIRE_FALSE(alive->HasError());
    CHECK(alive->GetValue(0, 0).GetValue<int32_t>() == 42);
}

const char *const ODP_PATH = "/sap/opu/odata/sap/Z_TEST_SRV/FactsOf0D_NW_C01";

}  // namespace

// ---------------------------------------------------------------------------------------------
// 1. A http_basic secret without a password, reached through a plain ATTACH or read.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a half-specified http_basic secret is bad input, not an internal error",
          "[internal_seams][secret]") {
    ODataTestServer server;
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // Accepted by CREATE SECRET: the password is not required there. The scope makes it the secret
    // that the URL-matched lookup finds for the test server.
    REQUIRE_FALSE(con.Query("CREATE SECRET half (TYPE http_basic, USERNAME 'u', SCOPE '" + server.BaseUrl() +
                            "')")
                      ->HasError());

    SECTION("ATTACH ... TYPE odata") {
        auto result = con.Query("ATTACH '" + server.BaseUrl() + "/svc/' AS x (TYPE odata, READ_ONLY)");
        RequireOrdinaryError(con, *result);
        CHECK(result->GetError().find("password") != std::string::npos);
    }
    SECTION("odata_read") {
        auto result = con.Query("SELECT * FROM odata_read('" + server.BaseUrl() + "/svc/Things')");
        RequireOrdinaryError(con, *result);
        CHECK(result->GetError().find("password") != std::string::npos);
    }
    SECTION("odata_sap_show") {
        auto result = con.Query("SELECT * FROM odata_sap_show('" + server.BaseUrl() + "')");
        RequireOrdinaryError(con, *result);
        CHECK(result->GetError().find("password") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------
// 2a. A read-only catalog must never be picked as the ODP state catalog.
// ---------------------------------------------------------------------------------------------

namespace {

std::string MakeStateFile(const std::string &name)
{
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
    duckdb::DuckDB creator(path);
    duckdb::Connection(creator).Query("CREATE TABLE marker(i INTEGER)");
    return path;
}

}  // namespace

TEST_CASE("a read-only attached database is not used as ODP state", "[internal_seams][state_catalog]") {
    const auto read_only_file = MakeStateFile("erpl_251_state_ro.db");

    SECTION("it is the only candidate: a clear error naming it") {
        TestDatabase database;
        duckdb::Connection &con = database.Con();
        REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
        REQUIRE_FALSE(con.Query("ATTACH '" + read_only_file + "' AS ro_state (READ_ONLY)")->HasError());

        auto result = con.Query("SELECT * FROM odp_odata_list_subscriptions()");
        RequireOrdinaryError(con, *result);
        CHECK(result->GetError().find("ro_state") != std::string::npos);
    }

    SECTION("a writable persistent catalog alongside it is chosen instead") {
        const auto writable_file = MakeStateFile("erpl_251_state_rw.db");
        TestDatabase database;
        duckdb::Connection &con = database.Con();
        REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
        // 'ro_state' sorts before 'rw_state', which is how the old code picked it.
        REQUIRE_FALSE(con.Query("ATTACH '" + read_only_file + "' AS ro_state (READ_ONLY)")->HasError());
        REQUIRE_FALSE(con.Query("ATTACH '" + writable_file + "' AS rw_state")->HasError());

        auto result = con.Query("SELECT count(*) FROM odp_odata_list_subscriptions()");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        CHECK(result->GetValue(0, 0).GetValue<int64_t>() == 0);

        std::filesystem::remove(writable_file);
        std::filesystem::remove(writable_file + ".wal");
    }

    std::filesystem::remove(read_only_file);
    std::filesystem::remove(read_only_file + ".wal");
}

// ---------------------------------------------------------------------------------------------
// 2b. Two sessions creating the same subscription at once.
// ---------------------------------------------------------------------------------------------

TEST_CASE("concurrent first reads of one subscription all get the same subscription",
          "[internal_seams][race]") {
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 12;

    odp_test::TempDatabase database("odp_251_race");
    auto &setup_connection = database.Conn();
    REQUIRE_FALSE(setup_connection.Query("LOAD erpl_odata")->HasError());
    OdpSubscriptionRepository(database.Context()).EnsureSchemaExists();
    OdpSubscriptionRepository(database.Context()).EnsureTablesExist();

    for (int round = 0; round < ROUNDS; round++) {
        const std::string entity_set = "FactsOfRace" + std::to_string(round);
        const std::string service_url = "https://test.example/sap/opu/odata/sap/RACE_SRV/" + entity_set;

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::string> ids(THREADS);
        std::vector<std::string> failures(THREADS);
        std::vector<std::thread> workers;
        for (int t = 0; t < THREADS; t++) {
            workers.emplace_back([&, t]() {
                duckdb::Connection connection(*database.Conn().context->db);
                OdpSubscriptionRepository repository(*connection.context);
                ready++;
                while (!go) {
                    std::this_thread::yield();
                }
                try {
                    ids[t] = repository.CreateSubscription(service_url, entity_set, "race_secret");
                } catch (const std::exception &e) {
                    failures[t] = e.what();
                }
            });
        }
        while (ready < THREADS) {
            std::this_thread::yield();
        }
        go = true;
        for (auto &worker : workers) {
            worker.join();
        }

        std::set<std::string> distinct;
        for (int t = 0; t < THREADS; t++) {
            INFO("round " << round << " thread " << t << ": " << failures[t]);
            REQUIRE(failures[t].empty());
            distinct.insert(ids[t]);
        }
        CHECK(distinct.size() == 1);
    }
}

// ---------------------------------------------------------------------------------------------
// 3. The error handler must not be able to fail louder than the error it reports.
// ---------------------------------------------------------------------------------------------

TEST_CASE("TransitionToError never throws, even when the subscription row is gone",
          "[internal_seams][transition]") {
    odp_test::TempDatabase database("odp_251_transition");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    OdpSubscriptionStateManager manager(database.Context(), "https://test.example/sap/opu/odata/sap/T_SRV/Facts",
                                        "Facts", "secret");
    // Another connection removed it while this scan was running.
    OdpSubscriptionRepository(database.Context()).RemoveSubscription(manager.GetSubscriptionId());

    CHECK_NOTHROW(manager.TransitionToError("HTTP 500 on page 3"));
    // Reported twice per failure by the read path; the second must be harmless too.
    CHECK_NOTHROW(manager.TransitionToError("HTTP 500 on page 3"));

    auto alive = con.Query("SELECT 42");
    REQUIRE_FALSE(alive->HasError());
}

TEST_CASE("the audit row still records the fetch error when the status update fails",
          "[internal_seams][transition]") {
    odp_test::TempDatabase database("odp_251_audit");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    OdpSubscriptionStateManager manager(database.Context(), "https://test.example/sap/opu/odata/sap/A_SRV/Facts",
                                        "Facts", "secret");
    const auto audit_id = manager.CreateAuditEntry("initial_load", "https://test.example/page3");
    REQUIRE(audit_id > 0);
    // The subscription row is gone, so the status update fails; the audit update must still be tried.
    OdpSubscriptionRepository(database.Context()).RemoveSubscription(manager.GetSubscriptionId());

    REQUIRE_NOTHROW(manager.TransitionToError("HTTP 500 on page 3"));

    auto audit = con.Query("SELECT count(*) FROM erpl_web.odp_subscription_audit WHERE audit_id = " +
                           std::to_string(audit_id) + " AND error_message LIKE '%HTTP 500 on page 3%'");
    REQUIRE_FALSE(audit->HasError());
    CHECK(audit->GetValue(0, 0).GetValue<int64_t>() == 1);
}

// ---------------------------------------------------------------------------------------------
// 4. A failed first fetch leaves the reader in ERROR_STATE; running the plan again must not
//    turn that into an internal error.
// ---------------------------------------------------------------------------------------------

TEST_CASE("re-executing an ODP read after a failed fetch is an ordinary error",
          "[internal_seams][reexec]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/sap/opu/odata/sap/Z_TEST_SRV/$metadata", "edm_sap_odp_bw_fact.xml");
    server.ServeMetadataFixture(std::string(ODP_PATH) + "/$metadata", "edm_sap_odp_bw_fact.xml");
    server.OnPath(ODP_PATH, CannedResponse::Error(500, R"({"error":{"message":{"value":"extractor down"}}})"));

    odp_test::TempDatabase database("odp_251_reexec");
    auto &con = database.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(con.Query("PREPARE p AS SELECT COUNT(*) FROM odp_odata_read('" + server.Url(ODP_PATH) + "')")
                      ->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        INFO("execution " << execution);
        auto result = con.Query("EXECUTE p");
        RequireOrdinaryError(con, *result);
    }
}
