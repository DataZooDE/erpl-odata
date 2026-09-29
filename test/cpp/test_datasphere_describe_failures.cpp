#include "catch.hpp"

#include "duckdb.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

// A Datasphere describe that cannot answer must FAIL, not answer wrongly.
//
// LoadResourceDetails turned every failure into a row: {"error", "<message>"}. That got
// resize(15)'d and emitted, so `SELECT name FROM datasphere_describe_asset(...)` returned the
// string "error" as the asset's NAME. A missing asset was indistinguishable from an asset
// called "error", and an unreachable tenant from an empty one - wrong answers rather than
// errors, which is the worst shape a defect can take.
//
// Two reviewers disagreed about which way it failed: a STRUCT cast error on column 10, or an
// emitted error row. Reading the code settles it - the extended-metadata block resize(15)s
// before the scan looks, so the ROW wins, and column 10 was left as an untyped NULL because
// only the catch branch filled it with MakeEmptyAnalyticalSchemaValue(). Both concerns were
// real; neither description was complete. See GitHub #244.
//
// Driven against a tenant that does not resolve, which needs no credentials of consequence:
// the catalog URLs are built as https://<tenant>.<data_center>.hcs.cloud.sap/... with no
// loopback hatch, so a local server cannot stand in for one.

namespace {

class TestDatabase {
public:
    TestDatabase()
    {
        config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));
        database = duckdb::make_uniq<duckdb::DuckDB>(nullptr, &config);
        connection = duckdb::make_uniq<duckdb::Connection>(*database);

        // Isolated from the developer's stored secrets. Without this the test picks up a
        // real `datasphere` secret and, if its token has expired, starts an interactive
        // OAuth2 browser flow - which a test must never do.
        secret_directory = std::filesystem::temp_directory_path() /
                           ("erpl_describe_test_" + std::to_string(NextId()) + "_" +
                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(secret_directory);
        auto isolated = connection->Query("SET secret_directory = '" + secret_directory.string() + "'");
        if (isolated->HasError()) {
            throw std::runtime_error("could not isolate secret directory: " + isolated->GetError());
        }
    }

    ~TestDatabase()
    {
        connection.reset();
        database.reset();
        std::error_code ignored;
        std::filesystem::remove_all(secret_directory, ignored);
    }

    duckdb::Connection &Con() const { return *connection; }

    // A secret whose token is already valid, so resolution never reaches RefreshTokens and
    // therefore never opens a browser. The tenant deliberately does not resolve.
    std::string WriteUnreachableTenantConfig() const
    {
        const auto path = secret_directory / "datasphere.config";
        std::ofstream file(path);
        file << "tenant_name=erpl-tenant-does-not-resolve\n"
             << "data_center=eu10\n"
             << "access_token=a-token\n"
             << "expires_at=4102444800\n";
        file.close();
        return path.string();
    }

private:
    static unsigned long NextId()
    {
        static std::atomic<unsigned long> counter{0};
        return ++counter;
    }

    std::filesystem::path secret_directory;
    duckdb::DBConfig config;
    duckdb::unique_ptr<duckdb::DuckDB> database;
    duckdb::unique_ptr<duckdb::Connection> connection;
};

}  // namespace

TEST_CASE("a Datasphere describe against an unreachable tenant errors rather than answering",
          "[datasphere][describe_failure]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(con.Query("CREATE SECRET datasphere (TYPE datasphere, PROVIDER config, "
                            "config_file '" + database.WriteUnreachableTenantConfig() + "')")
                      ->HasError());

    SECTION("describe_asset") {
        auto result = con.Query("SELECT name FROM datasphere_describe_asset('SPACE', 'ASSET')");
        INFO("result: " << (result->HasError() ? result->GetError()
                                               : "succeeded, returned: " +
                                                     result->GetValue(0, 0).ToString()));

        // It used to return a row here, with the literal string "error" as the asset's name.
        REQUIRE(result->HasError());
    }

    SECTION("describe_space") {
        // This one I observed returning SUCCESS against a non-resolving tenant while working
        // on an unrelated fix - the same failure-encoded-as-data path at a sibling function.
        auto result = con.Query("SELECT name FROM datasphere_describe_space('SPACE')");
        INFO("result: " << (result->HasError() ? result->GetError()
                                               : "succeeded, returned: " +
                                                     result->GetValue(0, 0).ToString()));

        REQUIRE(result->HasError());
    }
}

TEST_CASE("the describe failure never reports itself as a row value",
          "[datasphere][describe_failure]") {
    // The specific shape that made this a wrong answer: "error" arriving as a name, or a
    // message arriving as a space name. Asserted separately from the throw above, because a
    // future change could reintroduce the row while still erroring on some other path.
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    REQUIRE_FALSE(con.Query("CREATE SECRET datasphere (TYPE datasphere, PROVIDER config, "
                            "config_file '" + database.WriteUnreachableTenantConfig() + "')")
                      ->HasError());

    auto result = con.Query("SELECT name, space_name FROM datasphere_describe_asset('SP', 'A')");
    if (!result->HasError()) {
        const auto name = result->GetValue(0, 0).ToString();
        INFO("name column: " << name);
        REQUIRE(name != "error");
        REQUIRE(name.find("Error:") == std::string::npos);
    } else {
        SUCCEED("failed instead of answering, which is the point");
    }
}
