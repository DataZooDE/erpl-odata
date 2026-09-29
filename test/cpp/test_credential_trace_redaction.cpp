#include "catch.hpp"

#include "tracing.hpp"
#include "odata_test_server.hpp"
#include "duckdb.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace erpl_web::test_support;

// A credential must never reach the trace file.
//
// The trace is written to disk whenever erpl_trace_output is 'file' or 'both', so anything
// traced in cleartext outlives the session. http_get's `auth` parameter WAS traced whole,
// three lines above the two redactions ("password: ***", "token: ***") that show the intent
// had always been there - so for auth_type BEARER the token itself was written out.
//
// No test asserted trace redaction anywhere before this one, which is why the leak sat next
// to the redactions that prove it was unintended.

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

// Points the tracer at a directory of our own at DEBUG, and puts it back afterwards -
// the tracer is a process-wide singleton, so leaving it enabled would leak into every
// test that runs after this one.
class CapturedTrace {
public:
    CapturedTrace()
        // Portable unique name: getpid() needs unistd.h, which MSVC does not have. A
        // process-local counter plus the object address is unique enough for a temp dir
        // that is created and removed within one test.
        : directory(std::filesystem::temp_directory_path() /
                    ("erpl_trace_test_" + std::to_string(NextId()) + "_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this))))
    {
        std::filesystem::create_directories(directory);

        auto &tracer = erpl_web::ErplTracer::Instance();
        previously_enabled = tracer.IsEnabled();
        previous_mode = tracer.GetOutputMode();
        previous_level = tracer.GetLevel();

        tracer.SetTraceDirectory(directory.string());
        tracer.SetOutputMode("file");
        tracer.SetLevel(erpl_web::TraceLevel::DEBUG_LEVEL);
        tracer.SetEnabled(true);
    }

    ~CapturedTrace()
    {
        auto &tracer = erpl_web::ErplTracer::Instance();
        tracer.SetEnabled(previously_enabled);
        tracer.SetOutputMode(previous_mode);
        tracer.SetLevel(previous_level);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::string Contents() const
    {
        std::ifstream file(directory / "erpl_odata_trace.log");
        if (!file) {
            return std::string();
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

private:
    static unsigned long NextId()
    {
        static std::atomic<unsigned long> counter{0};
        return ++counter;
    }

    std::filesystem::path directory;
    bool previously_enabled = false;
    std::string previous_mode;
    erpl_web::TraceLevel previous_level = erpl_web::TraceLevel::INFO;
};

// Distinctive enough that finding it in the trace cannot be a coincidence.
const char *const SECRET_TOKEN = "eyJhbGciOiJIUzI1NiJ9.ERPL-TRACE-LEAK-CANARY.signature";

}  // namespace

TEST_CASE("a bearer token passed to http_get never reaches the trace file",
          "[tracing][security][credential_redaction]") {
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    CapturedTrace trace;

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := '" + SECRET_TOKEN + "', auth_type := 'BEARER')");
    INFO("query error: " << (result->HasError() ? result->GetError() : std::string("<no error>")));
    REQUIRE_FALSE(result->HasError());

    const std::string traced = trace.Contents();

    // Guard against a vacuous pass: if nothing was traced at all, the absence of the token
    // below would prove nothing. HTTP_AUTH is the component that handles the parameter.
    REQUIRE_FALSE(traced.empty());
    REQUIRE(traced.find("HTTP_AUTH") != std::string::npos);

    // The actual assertion.
    REQUIRE(traced.find(SECRET_TOKEN) == std::string::npos);
}

TEST_CASE("a colonless auth value is not traced either - auth_type defaults to BASIC",
          "[tracing][security][credential_redaction]") {
    // The sibling seam. A caller who writes `auth := '<token>'` and omits auth_type gets
    // BASIC, and a colonless value is then treated as a username with an empty password -
    // where it was traced in full. A value with no colon is indistinguishable from a token,
    // so it is not traced either.
    ODataTestServer server;
    server.OnPath("/echo", CannedResponse::Json(R"({"ok":true})"));

    CapturedTrace trace;

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT status FROM http_get('" + server.Url("/echo") +
                            "', auth := '" + SECRET_TOKEN + "')");
    INFO("query error: " << (result->HasError() ? result->GetError() : std::string("<no error>")));
    REQUIRE_FALSE(result->HasError());

    const std::string traced = trace.Contents();
    REQUIRE_FALSE(traced.empty());
    REQUIRE(traced.find("HTTP_AUTH") != std::string::npos);
    REQUIRE(traced.find(SECRET_TOKEN) == std::string::npos);
}

// ---------------------------------------------------------------------------
// The other way a credential leaves the machine: sent to a host we never checked.

TEST_CASE("the Datasphere token_url is gated before the client secret is posted to it",
          "[datasphere][security][credential_redaction]") {
    // The client_credentials token request carries Authorization: Basic
    // base64(client_id:client_secret) - a long-lived credential. token_url comes out of the
    // secret, and the `config` provider copies every key=value line of a file into the
    // secret map, so a tampered config can supply a token_url the user never typed.
    //
    // The sibling seam (BuildDataUrl in datasphere_read.cpp) already gated its
    // caller-supplied URL for exactly this reason; this one did not.
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto created = con.Query(
        "CREATE SECRET ds_hostile (TYPE datasphere, PROVIDER oauth2, "
        "grant_type 'client_credentials', client_id 'cid', client_secret 's3cr3t', "
        "tenant_name 'tenant', data_center 'eu10', "
        "token_url 'http://attacker.example/token')");
    INFO("create error: " << (created->HasError() ? created->GetError() : std::string("<none>")));
    REQUIRE_FALSE(created->HasError());

    auto result = con.Query(
        "SELECT * FROM datasphere_read_relational('SPACE', 'ASSET', 'ds_hostile')");
    REQUIRE(result->HasError());

    const std::string error = result->GetError();
    INFO("error was: " << error);

    // Refused by the gate, naming the parameter - not by some unrelated failure further on.
    REQUIRE(error.find("token_url") != std::string::npos);
    REQUIRE(error.find("attacker.example") != std::string::npos);

    // And the secret itself never appears in the message.
    REQUIRE(error.find("s3cr3t") == std::string::npos);
}
