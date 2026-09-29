// Per-execution scan state for the Delta Sharing catalog functions - the #75 class,
// reported as GitHub #202.
//
// delta_share_show_shares / _show_schemas / _show_tables each fetched their rows during
// bind and then walked them with a `current_index` / `finished` pair kept on the BIND
// DATA. DuckDB reuses bind data across executions of a bound plan and nothing reset that
// pair, so the second EXECUTE of a prepared statement resumed from a drained cursor and
// returned zero rows - with no error, which is what makes this class of defect dangerous.
//
// The cursor now lives on DeltaShareRowCursorState, a GlobalTableFunctionState, so every
// execution starts at index 0 over the same immutable payload.
//
// Delta Sharing needs no test-only escape hatch to be driven against a loopback server:
// the endpoint is a field of the profile file the caller passes in, so the fixture is
// just a profile pointing at ODataTestServer.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;

namespace {

// See CLAUDE.md: a bare DuckDB(nullptr) crashes in DEBUG builds shortly after the first
// query unless jemalloc's background threads own arena management.
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

// Writes a Delta Sharing profile next to the test binary and removes it again, so the
// fixture leaves nothing behind whichever way the test exits. The name carries a
// caller-supplied tag plus a run-unique suffix so two cases - or two shards of the same
// binary - never collide. The suffix is built from the clock and a counter rather than the
// pid: getpid()/<unistd.h> do not exist on MSVC, and this file is compiled on Windows CI.
class ProfileFile {
public:
    ProfileFile(const std::string &endpoint, const std::string &tag)
        : path("erpl_delta_share_" + tag + "_" + UniqueSuffix() + ".json")
    {
        std::ofstream out(path);
        out << R"({"shareCredentialsVersion":1,"endpoint":")" << endpoint
            << R"(","bearerToken":"test-token"})";
    }

    ~ProfileFile() { std::remove(path.c_str()); }

    const std::string &Path() const { return path; }

private:
    static std::string UniqueSuffix()
    {
        static std::atomic<unsigned> counter{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::to_string(static_cast<long long>(ticks)) + "_" +
               std::to_string(counter.fetch_add(1));
    }

    std::string path;
};

// Three shares, two schemas and two tables: more than one row, so a cursor that is
// merely off-by-one is also caught, and a count that survives three executions cannot
// be explained by an empty payload.
void ServeCatalog(ODataTestServer &server)
{
    server.OnPath("/shares",
                  CannedResponse::Json(
                      R"({"items":[],"shares":[{"name":"alpha","id":"1"},)"
                      R"({"name":"beta","id":"2"},{"name":"gamma","id":"3"}]})"));
    server.OnPath("/shares/alpha/schemas",
                  CannedResponse::Json(R"({"schemas":[{"name":"sales"},{"name":"hr"}]})"));
    server.OnPath("/shares/alpha/schemas/sales/tables",
                  CannedResponse::Json(R"({"tables":[{"name":"orders"},{"name":"returns"}]})"));
}

int64_t ScalarOf(duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result)
{
    return result->GetValue(0, 0).GetValue<int64_t>();
}

// Prepares `sql`, executes it three times and requires `expected_rows` every time. One
// execution proves nothing here: the defect only shows from the second onwards.
void RequireStableAcrossExecutions(duckdb::Connection &con, const std::string &sql,
                                   int64_t expected_rows)
{
    auto prep = con.Query("PREPARE p AS " + sql);
    INFO("PREPARE: " << (prep->HasError() ? prep->GetError() : std::string("ok")));
    REQUIRE_FALSE(prep->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE p");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == expected_rows);
    }
    REQUIRE_FALSE(con.Query("DEALLOCATE p")->HasError());
}

}  // namespace

TEST_CASE("bound delta_share catalog plans return every row on each execution",
          "[delta_share][reexec]") {
    ODataTestServer server;
    ServeCatalog(server);
    ProfileFile profile(server.BaseUrl(), "catalog");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    SECTION("delta_share_show_shares") {
        RequireStableAcrossExecutions(
            con, "SELECT COUNT(*) FROM delta_share_show_shares('" + profile.Path() + "')", 3);
    }

    SECTION("delta_share_show_schemas") {
        RequireStableAcrossExecutions(
            con, "SELECT COUNT(*) FROM delta_share_show_schemas('" + profile.Path() + "', 'alpha')",
            2);
    }

    SECTION("delta_share_show_tables") {
        RequireStableAcrossExecutions(
            con,
            "SELECT COUNT(*) FROM delta_share_show_tables('" + profile.Path() +
                "', 'alpha', 'sales')",
            2);
    }
}

// Two scans of the same function inside one query share neither cursor. Before the fix
// this shape could not even be reached (the first EXECUTE already drained the cursor for
// the second scan), and it is the shape a join or a UNION produces in real use.
TEST_CASE("two delta_share_show_shares scans in one query each see every row",
          "[delta_share][reexec]") {
    ODataTestServer server;
    ServeCatalog(server);
    ProfileFile profile(server.BaseUrl(), "twoscans");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    const std::string scan = "delta_share_show_shares('" + profile.Path() + "')";
    auto result = con.Query("SELECT (SELECT COUNT(*) FROM " + scan + ") + "
                            "(SELECT COUNT(*) FROM " + scan + ")");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(ScalarOf(result) == 6);
}

// A payload larger than one output chunk. `finished` is only set once the cursor passes
// the end of the vector, so this is the shape that distinguishes a cursor that resets per
// execution from one that merely happens to fit in a single Scan call.
TEST_CASE("a bound delta_share_show_shares plan re-reads a multi-chunk payload",
          "[delta_share][reexec]") {
    const int SHARE_COUNT = 5000;  // > STANDARD_VECTOR_SIZE (2048), so at least three chunks

    std::string shares;
    for (int i = 0; i < SHARE_COUNT; i++) {
        if (i > 0) {
            shares += ",";
        }
        shares += R"({"name":"s)" + std::to_string(i) + R"(","id":")" + std::to_string(i) + R"("})";
    }

    ODataTestServer server;
    server.OnPath("/shares", CannedResponse::Json(R"({"shares":[)" + shares + "]}"));
    ProfileFile profile(server.BaseUrl(), "multichunk");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    RequireStableAcrossExecutions(
        con, "SELECT COUNT(*) FROM delta_share_show_shares('" + profile.Path() + "')",
        SHARE_COUNT);
}

// The bind used to pre-set `finished` when the payload was empty; the cursor now handles
// it. An empty share list must stay empty - and stay an empty RESULT, not an error - on
// every execution.
TEST_CASE("a bound delta_share_show_shares plan over an empty payload stays empty",
          "[delta_share][reexec]") {
    ODataTestServer server;
    server.OnPath("/shares", CannedResponse::Json(R"({"shares":[]})"));
    ProfileFile profile(server.BaseUrl(), "empty");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    RequireStableAcrossExecutions(
        con, "SELECT COUNT(*) FROM delta_share_show_shares('" + profile.Path() + "')", 0);
}

// GitHub #202 third review, F2: the six http_* verbs were invisible to the catalog walk
// (its prefix list omitted "http_") and all six latched on the bind data. The read verbs
// now re-issue their request per execution; the mutating verbs stay one-shot by design.
//
// This lives here because the delta-share file already has an ODataTestServer fixture and
// http_get is trivially drivable against one.
TEST_CASE("a bound http_get plan issues its request on every execution",
          "[delta_share][reexec][http]") {
    ODataTestServer server;
    server.OnPath("/ping", CannedResponse::Json(R"({"ok":true})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto prep = con.Query("PREPARE h AS SELECT status FROM http_get('" + server.Url("/ping") + "')");
    INFO((prep->HasError() ? prep->GetError() : std::string("ok")));
    REQUIRE_FALSE(prep->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE h");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 1);
        REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 200);
    }

    // One request per execution: the response is not a bind-time snapshot.
    REQUIRE(server.RequestsFor("/ping").size() == 3);
}
