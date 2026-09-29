#include "catch.hpp"

#include "duckdb.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

// The Datasphere catalog functions take the secret the caller names.
//
// They used to be locked to a hardcoded "datasphere". Two of the binds even contained code
// to read a `secret` named parameter - but no catalog function ever REGISTERED that
// parameter, and DuckDB's binder rejects an undeclared named parameter before the bind
// runs. So `secret := 'x'` failed with "Invalid named parameter" and the reading code was
// unreachable, while the datasphere_read_* path threaded the name correctly all along.
//
// See GitHub #243. Note the failure mode: this was never a silent cross-tenant read - it
// was an outright rejection plus dead code. Worth stating, because a report of this defect
// described it the other way round.

namespace {

class TestDatabase {
public:
    TestDatabase()
    {
        config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));

        // Isolate from the developer's persisted secrets. Without this the test reads
        // ~/.duckdb/stored_secrets, so a machine with a real `datasphere` secret resolves
        // it, finds the token expired, and starts an INTERACTIVE OAuth2 browser flow that
        // blocks until it times out. That makes the test both environment-dependent and
        // capable of hijacking the developer's session.
        secret_directory = std::filesystem::temp_directory_path() /
                           ("erpl_secrets_test_" + std::to_string(NextId()) + "_" +
                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(secret_directory);
        database = duckdb::make_uniq<duckdb::DuckDB>(nullptr, &config);
        connection = duckdb::make_uniq<duckdb::Connection>(*database);

        // secret_directory is a runtime setting, not a DBConfig option. Secrets load
        // lazily, so pointing it at an empty directory before the first secret lookup is
        // enough to keep the developer's stored secrets out of this test.
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

    // Writes a Datasphere config file inside this test's own directory.
    std::string WriteConfigFile(const std::string &contents) const
    {
        const auto path = secret_directory / "datasphere.config";
        std::ofstream file(path);
        file << contents;
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

// Every catalog function, so a future one added without the parameter is visible here.
const char *const CATALOG_QUERIES[] = {
    "SELECT * FROM datasphere_describe_asset('SP', 'ASSET', secret := 'named_by_caller')",
    "SELECT * FROM datasphere_describe_space('SP', secret := 'named_by_caller')",
    "SELECT * FROM datasphere_show_assets('SP', secret := 'named_by_caller')",
    "SELECT * FROM datasphere_show_spaces(secret := 'named_by_caller')",
};

}  // namespace

TEST_CASE("every Datasphere catalog function accepts and uses the named secret",
          "[datasphere][secret_selection]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // Only the caller's secret exists. There is deliberately NO secret named "datasphere",
    // which is what makes this discriminating: if resolution still looked up the hardcoded
    // name, it would fail with "Secret 'datasphere' not found".
    //
    // Asserting on the error TEXT alone would not have worked - the message interpolates
    // the requested name, so it mentions the caller's secret either way. An earlier version
    // of this test did exactly that and passed against the unfixed code.
    // The token is deliberately pre-valid (expires_at is a Unix epoch far in the future).
    // Without a valid cached token, resolution calls RefreshTokens, which runs the
    // INTERACTIVE OAuth2 browser flow - which a test must never do.
    const auto config_file = database.WriteConfigFile(
        "tenant_name=erpl-test-tenant-does-not-resolve\n"
        "data_center=eu10\n"
        "access_token=a-token\n"
        "expires_at=4102444800\n");

    auto created = con.Query("CREATE SECRET named_by_caller (TYPE datasphere, PROVIDER config, "
                             "config_file '" + config_file + "')");
    INFO("create error: " << (created->HasError() ? created->GetError() : std::string("<none>")));
    REQUIRE_FALSE(created->HasError());

    for (const char *query : CATALOG_QUERIES) {
        INFO("query: " << query);

        auto result = con.Query(query);

        // Some of these fail on the unreachable tenant and some return empty; either is
        // fine here, because what is under test is which SECRET was resolved, not what the
        // call went on to do. So the error is inspected only when there is one.
        const std::string error = result->HasError() ? result->GetError() : std::string();
        INFO("error: " << (error.empty() ? std::string("<succeeded>") : error));

        // The binder used to stop here with "Invalid named parameter".
        REQUIRE(error.find("Invalid named parameter") == std::string::npos);

        // Resolution found the caller's secret and moved on. Revert the threading and this
        // becomes "Secret 'datasphere' not found", because no such secret exists here -
        // which is what makes the assertion discriminating rather than decorative.
        REQUIRE(error.find("not found") == std::string::npos);
    }
}

TEST_CASE("omitting the secret still resolves the conventional 'datasphere' one",
          "[datasphere][secret_selection]") {
    // The default has to stay what it has always been, or every existing script breaks.
    // (One bind defaulted its local variable to "default", which was never a secret anyone
    // had - harmless only because the value was discarded before use.)
    //
    // Asserted through the error text rather than a successful call: with no secrets
    // configured, resolution fails naming the secret it looked for, which is exactly the
    // fact under test.
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM datasphere_describe_asset('SP', 'ASSET')");
    REQUIRE(result->HasError());

    const std::string error = result->GetError();
    INFO("error: " << error);
    REQUIRE(error.find("Secret 'datasphere' not found") != std::string::npos);
}
