// GitHub #75, still open on the Datasphere side.
//
// All mutable scan state - the row buffer, the activated projection, and the OData
// client's pagination cursor - used to live in the bind data. odata_read(), the ATTACHed
// odata_table_scan and the SAC readers were moved onto ODataReadGlobalState, so each
// execution of a bound plan gets a private clone. The two Datasphere readers reuse
// ODataReadScan but keep their own init-global functions, and those still return a bare
// GlobalTableFunctionState - which makes ResolveScanState fall back to scanning straight
// out of the bind data.
//
// The consequence is the original #75 symptom: the second EXECUTE of a prepared statement
// finds the buffer already drained and returns nothing, and a self-join of one call has
// both scans sharing a single cursor. Silently, in both cases - there is no error.
//
// These drive the real table functions. Passing a full URL as space_id bypasses the
// tenant/data-centre URL builder (see BuildDataUrl), which is what lets them run against
// the local server instead of a Datasphere tenant.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odata_url_helpers.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {


const char *const AIRLINE_AA = R"({"AirlineCode":"AA","Name":"American Airlines"})";
const char *const AIRLINE_FM = R"({"AirlineCode":"FM","Name":"Shanghai Airline"})";
const char *const AIRLINE_MU = R"({"AirlineCode":"MU","Name":"China Eastern Airlines"})";
const char *const AIRLINE_AF = R"({"AirlineCode":"AF","Name":"Air France"})";

// Two pages of four airlines under `prefix`, addressed as space_id=<base>, asset_id=Airlines.
void ServeTwoPageAirlines(ODataTestServer &server, const std::string &prefix)
{
    const std::string entity_url = server.Url(prefix + "/Airlines");
    const std::string context = server.Url(prefix + "/$metadata") + "#Airlines";

    server.ServeMetadataFixture(prefix + "/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [prefix](const RecordedRequest &request) {
            return request.path == prefix + "/Airlines" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath(prefix + "/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    // Without a secret the readers start an interactive authorization_code flow and the
    // test hangs until "Timeout waiting for OAuth2 callback". A client_credentials secret
    // pointed at this endpoint keeps the whole exchange on loopback.
    server.OnPath("/oauth/token",
                  CannedResponse::Json(
                      R"({"access_token":"test-token","token_type":"Bearer","expires_in":3600})"));
}

// Registers a Datasphere secret whose token endpoint is the test server.
void CreateLoopbackSecret(duckdb::Connection &con, const std::string &name,
                          const ODataTestServer &server)
{
    auto created = con.Query(
        "CREATE SECRET " + name + " (TYPE datasphere, PROVIDER oauth2, "
        "TENANT_NAME 'loopback', DATA_CENTER 'eu10', "
        "CLIENT_ID 'test-client', CLIENT_SECRET 'test-secret', "
        "GRANT_TYPE 'client_credentials', SCOPE 'default', "
        "TOKEN_URL '" + server.Url("/oauth/token") + "')");
    INFO((created->HasError() ? created->GetError() : std::string()));
    REQUIRE_FALSE(created->HasError());
}

int64_t ScalarOf(duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result)
{
    return result->GetValue(0, 0).GetValue<int64_t>();
}

}  // namespace

TEST_CASE("a bound datasphere_read_relational plan returns every row on each execution",
          "[datasphere_reexec]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/dsrel");
    const std::string read =
        "datasphere_read_relational('" + server.Url("/dsrel") + "', 'Airlines', secret => 'ds')";

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    CreateLoopbackSecret(con, "ds", server);

    REQUIRE_FALSE(con.Query("PREPARE p AS SELECT * FROM " + read)->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE p");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 4);
    }
}

TEST_CASE("two datasphere_read_relational scans of one call each see every row",
          "[datasphere_reexec]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/dsreljoin");
    const std::string read =
        "datasphere_read_relational('" + server.Url("/dsreljoin") + "', 'Airlines', secret => 'ds')";

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    CreateLoopbackSecret(con, "ds", server);

    auto result = con.Query("SELECT COUNT(*) FROM " + read + " a, " + read + " b");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(ScalarOf(result) == 16);
}

TEST_CASE("a bound datasphere_read_analytical plan returns every row on each execution",
          "[datasphere_reexec]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/dsana");
    const std::string read =
        "datasphere_read_analytical('" + server.Url("/dsana") + "', 'Airlines', secret => 'ds')";

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    CreateLoopbackSecret(con, "ds", server);

    REQUIRE_FALSE(con.Query("PREPARE q AS SELECT * FROM " + read)->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE q");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 4);
    }
}

TEST_CASE("two datasphere_read_analytical scans of one call each see every row",
          "[datasphere_reexec]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/dsanajoin");
    const std::string read =
        "datasphere_read_analytical('" + server.Url("/dsanajoin") + "', 'Airlines', secret => 'ds')";

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    CreateLoopbackSecret(con, "ds", server);

    auto result = con.Query("SELECT COUNT(*) FROM " + read + " a, " + read + " b");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(ScalarOf(result) == 16);
}

// Datasphere uses a dual-URL shape: the data lives at one URL and the @odata.context names
// a DIFFERENT metadata URL. FromEntitySetRoot stores that context URL and the entity-set
// name from its fragment at bind time, gated on IsDatasphereUrl.
//
// Two places threw that state away. CloneForScan did not copy it, and
// UpdateUrlFromPredicatePushdown rebuilt the client preserving only the OData version -
// undoing the copy one statement later. Both are fixed; this case pins both.
//
// The projecting query is the load-bearing part. An earlier version of this case used only
// SELECT *, which leaves the URL unchanged and takes the early return in
// UpdateUrlFromPredicatePushdown, so it never reaches the rebuild - and I wrongly reported
// the defect as unreproducible on that basis. A projection changes the URL, rebuilds the
// client, and the reader then asks for $metadata under the DATA path, which is what the
// final loop catches.
TEST_CASE("a Datasphere dual-URL asset re-executes correctly",
          "[datasphere_reexec][dualurl]") {
    ODataTestServer server;

    // The path must contain "datasphere": ODataReadBindData::IsDatasphereUrl gates the
    // dual-URL storage on that substring, so without it the shape under test never occurs.
    const std::string data_prefix = "/datasphere/dwaas-core/api/v1/spaces/SP";
    const std::string meta_prefix = "/datasphere/api/v1/dwc/consumption/relational/SP/ASSET";
    // The data URL's last segment is the ASSET name, which is NOT the entity-set name in
    // the EDM - that comes from the @odata.context fragment. Making them differ is what
    // makes a lost fragment name observable; if the URL ended in /Airlines the fallback
    // "derive the entity set from the URL" would land on the right answer and hide it.
    const std::string entity_url = server.Url(data_prefix + "/MYASSET");
    // The context deliberately points somewhere the data URL would never derive.
    const std::string context = server.Url(meta_prefix + "/$metadata") + "#Airlines";

    // Metadata is served ONLY under the context path. If the clone derives the metadata
    // URL from the data URL instead, it asks for a document that does not exist.
    server.ServeMetadataFixture(meta_prefix + "/$metadata", "edm_trippin.xml");

    server.OnMatch(
        [data_prefix](const RecordedRequest &request) {
            return request.path == data_prefix + "/MYASSET" &&
                   request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath(data_prefix + "/MYASSET",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    // Datasphere's parameterized shape: input parameters become a key predicate on the
    // asset plus a /Set suffix.
    server.OnPath(data_prefix + "/MYASSET(P_YEAR=2026)/Set",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM,
                                                            AIRLINE_MU, AIRLINE_AF})));
    server.OnPath("/oauth/token",
                  CannedResponse::Json(
                      R"({"access_token":"test-token","token_type":"Bearer","expires_in":3600})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    CreateLoopbackSecret(con, "ds", server);

    const std::string read =
        "datasphere_read_relational('" + server.Url(data_prefix) + "', 'MYASSET', secret => 'ds')";

    REQUIRE_FALSE(con.Query("PREPARE dual AS SELECT * FROM " + read)->HasError());

    for (int execution = 1; execution <= 2; execution++) {
        auto result = con.Query("EXECUTE dual");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 4);
    }

    // GitHub #190: with INPUT PARAMETERS present, GetMetadataContextUrl() used to clear the
    // stored context URL unconditionally - treating the service's own @odata.context as a
    // cache that parameters invalidate. That made the #186 carry a no-op for exactly the
    // parameterized Datasphere reads it was written for. The parameters here are what
    // triggers that path; without them this case cannot see the defect.
    auto parameterized = con.Query(
        "SELECT Name FROM datasphere_read_relational('" + server.Url(data_prefix) +
        "', 'MYASSET', secret => 'ds', params => MAP{'P_YEAR': '2026'}) ORDER BY Name");
    INFO((parameterized->HasError() ? parameterized->GetError() : std::string()));
    REQUIRE_FALSE(parameterized->HasError());
    REQUIRE(parameterized->RowCount() == 4);

    // A PROJECTING query is the shape that matters: SELECT * leaves the URL unchanged and
    // takes the early return in UpdateUrlFromPredicatePushdown, so it never reaches the
    // client rebuild. A projection changes the URL, rebuilds the client, and any client
    // state not carried across that rebuild is lost there.
    auto projected = con.Query("SELECT Name FROM " + read + " ORDER BY Name");
    INFO((projected->HasError() ? projected->GetError() : std::string()));
    REQUIRE_FALSE(projected->HasError());
    REQUIRE(projected->RowCount() == 4);

    // Metadata must have been fetched from the CONTEXT path; an empty set here would make
    // the loop below assert nothing.
    INFO("metadata was never fetched from the context path");
    REQUIRE_FALSE(server.RequestsFor(meta_prefix + "/$metadata").empty());

    // Nothing may have asked for metadata under the DATA path - that is the shape the
    // dropped context URL produces.
    for (const auto &request : server.Requests()) {
        INFO("requested " << request.path);
        REQUIRE(request.path != data_prefix + "/$metadata");
    }
}

// The verbatim-URL hatch on space_id carries an OAuth bearer token, exactly like the
// Business Central and Dataverse ones. Raised by an agent-crew review as an unapplied fix:
// the loopback restriction went onto BC's hatch and not onto its siblings.
//
// The secret's token endpoint has to be a working loopback server: auth is resolved before
// the data URL is built, so without it the read fails on the token fetch and never reaches
// the guard under test. (That ordering is itself reassuring - the bearer token is obtained
// from the configured token endpoint, never sent to the rejected host.)
TEST_CASE("the Datasphere space_id hatch refuses plain http off loopback",
          "[datasphere_reexec][security]") {
    ODataTestServer server;
    server.OnPath("/oauth/token",
                  CannedResponse::Json(
                      R"({"access_token":"test-token","token_type":"Bearer","expires_in":3600})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    CreateLoopbackSecret(con, "dssec", server);

    auto result = con.Query(
        "SELECT * FROM datasphere_read_relational('http://evil.example/sp', 'A', secret => 'dssec')");
    REQUIRE(result->HasError());
    INFO("error was: " << result->GetError());
    REQUIRE(result->GetError().find("loopback") != std::string::npos);
    // A caller mistake, not a broken invariant of ours.
    REQUIRE(result->GetErrorObject().Type() != duckdb::ExceptionType::INTERNAL);

    // Datasphere's token audience is a fixed 'default'/'apiaccess' scope, so a non-loopback
    // https host is GATED (GitHub #199) - it is not simply accepted.
    //
    // This assertion used to read "https anywhere is still accepted", checking only that
    // the error did not mention "loopback". The gate's message does not contain that word
    // either, so that check survived the behaviour change without failing: it could no
    // longer detect anything. Asserting the gate explicitly instead.
    auto gated = con.Query(
        "SELECT * FROM datasphere_read_relational('https://tenant.datasphere.example/sp', 'A', "
        "secret => 'dssec')");
    REQUIRE(gated->HasError());
    INFO("gated error was: " << gated->GetError());
    REQUIRE(gated->GetError().find("erpl_unsafe_allow_custom_service_urls") != std::string::npos);

    // ...and permitted once the caller opts in. RAII because the policy is process-wide.
    {
        struct OptInGuard {
            OptInGuard() { erpl_web::ServiceUrlPolicy::SetCustomServiceUrlsAllowed(true); }
            ~OptInGuard() { erpl_web::ServiceUrlPolicy::SetCustomServiceUrlsAllowed(false); }
        } opt_in;

        auto allowed = con.Query(
            "SELECT * FROM datasphere_read_relational('https://tenant.datasphere.example/sp', 'A', "
            "secret => 'dssec')");
        // It will fail to CONNECT to a host that does not exist - the point is that it no
        // longer fails on the GATE.
        REQUIRE(allowed->HasError());
        INFO("opted-in error was: " << allowed->GetError());
        REQUIRE(allowed->GetError().find("erpl_unsafe_allow_custom_service_urls") ==
                std::string::npos);
    }
}
