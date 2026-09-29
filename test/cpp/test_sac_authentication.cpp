#include "catch.hpp"

#include "sac_secret_helper.hpp"
#include "odata_test_server.hpp"
#include "duckdb.hpp"

#include <string>

using namespace erpl_web::test_support;

// SAC was registered, documented, and structurally unable to authenticate.
//
// Three separate things made that true, and each hid the next:
//   - ResolveSacSecretData found access_token and did nothing with it (the `if` body was
//     empty), so auth_params went back with bearer_token unset;
//   - no `sac` secret type was registered at all, so the CREATE SECRET statement the code's
//     own error message tells the user to run could not parse;
//   - the catalog service returned empty vectors, which DuckDB cannot tell apart from a
//     tenant that genuinely has no models.
//
// See GitHub #243.

namespace {

class TestDatabase {
public:
    TestDatabase()
    {
        config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));
        database = duckdb::make_uniq<duckdb::DuckDB>(nullptr, &config);
        connection = duckdb::make_uniq<duckdb::Connection>(*database);
    }

    duckdb::Connection &Con() const { return *connection; }

private:
    duckdb::DBConfig config;
    duckdb::unique_ptr<duckdb::DuckDB> database;
    duckdb::unique_ptr<duckdb::Connection> connection;
};

// ResolveSacSecretData resolves through the system catalog transaction, which requires an
// active transaction on the connection. Queries open one implicitly; a direct call does not.
class ActiveTransaction {
public:
    explicit ActiveTransaction(duckdb::Connection &connection) : connection(connection)
    {
        connection.BeginTransaction();
    }
    ~ActiveTransaction()
    {
        try {
            connection.Commit();
        } catch (...) {
        }
    }

private:
    duckdb::Connection &connection;
};

}  // namespace

TEST_CASE("the sac secret type exists and CREATE SECRET parses", "[sac][auth]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // This is verbatim the shape sac_secret_helper's own error message instructs users to
    // run. Before the type was registered it failed with "Secret type 'sac' not found".
    auto created = con.Query(
        "CREATE SECRET sac_test (TYPE sac, PROVIDER access_token, "
        "access_token 'the-token', tenant_name 'acme', region 'eu10')");
    INFO("create error: " << (created->HasError() ? created->GetError() : std::string("<none>")));
    REQUIRE_FALSE(created->HasError());

    auto listed = con.Query("SELECT name, type FROM duckdb_secrets() WHERE name = 'sac_test'");
    REQUIRE_FALSE(listed->HasError());
    REQUIRE(listed->RowCount() == 1);
    REQUIRE(listed->GetValue(1, 0).ToString() == "sac");
}

TEST_CASE("the access token is actually attached to auth params", "[sac][auth]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    REQUIRE_FALSE(con.Query("CREATE SECRET sac_tok (TYPE sac, PROVIDER access_token, "
                            "access_token 'the-expected-token', tenant_name 'acme', region 'eu10')")
                      ->HasError());

    ActiveTransaction transaction(con);
    auto resolved = erpl_web::ResolveSacSecretData(*con.context, "sac_tok");

    REQUIRE(resolved.auth_params != nullptr);
    // The whole defect in one assertion: this was empty, so every SAC request went out with
    // no Authorization header and failed as if the tenant had denied access.
    REQUIRE(resolved.auth_params->bearer_token == "the-expected-token");
    REQUIRE(resolved.tenant == "acme");
    REQUIRE(resolved.region == "eu10");
}

TEST_CASE("a SAC secret with no token is refused rather than sent anonymously", "[sac][auth]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // Built without a token. Previously this resolved happily and produced unauthenticated
    // requests; the resulting 401 named nothing about the missing token.
    auto created = con.Query("CREATE SECRET sac_notoken (TYPE sac, PROVIDER config, "
                             "tenant_name 'acme', region 'eu10')");
    REQUIRE_FALSE(created->HasError());

    ActiveTransaction transaction(con);
    REQUIRE_THROWS_AS(erpl_web::ResolveSacSecretData(*con.context, "sac_notoken"),
                      duckdb::InvalidInputException);
}

TEST_CASE("the base_url hatch is gated like every other caller-supplied service URL",
          "[sac][auth][security]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // The hatch exists so SAC can be tested against a local server at all. It carries the
    // tenant's bearer token, so it gets the same gate as Datasphere's equivalent: loopback
    // http is fine, an arbitrary remote host is not.
    REQUIRE_FALSE(con.Query("CREATE SECRET sac_loopback (TYPE sac, PROVIDER access_token, "
                            "access_token 't', base_url 'http://localhost:1234/sac')")
                      ->HasError());
    ActiveTransaction transaction(con);
    REQUIRE_NOTHROW(erpl_web::ResolveSacSecretData(*con.context, "sac_loopback"));

    REQUIRE_FALSE(con.Query("CREATE SECRET sac_remote (TYPE sac, PROVIDER access_token, "
                            "access_token 't', base_url 'http://attacker.example/sac')")
                      ->HasError());
    REQUIRE_THROWS_AS(erpl_web::ResolveSacSecretData(*con.context, "sac_remote"),
                      duckdb::InvalidInputException);
}

TEST_CASE("SAC catalog discovery reports that it is unimplemented instead of answering empty",
          "[sac][auth]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    REQUIRE_FALSE(con.Query("CREATE SECRET sac (TYPE sac, PROVIDER access_token, "
                            "access_token 't', tenant_name 'acme', region 'eu10')")
                      ->HasError());

    // A zero-row chunk means end-of-scan in DuckDB, so returning one from a stub is an
    // authoritative "this tenant has no models" - indistinguishable from the truth.
    auto result = con.Query("SELECT * FROM sac_show_models()");
    REQUIRE(result->HasError());

    const std::string error = result->GetError();
    INFO("error was: " << error);
    REQUIRE(error.find("not implemented") != std::string::npos);
}
