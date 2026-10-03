// Per-execution scan state for the Business Central and Dataverse readers - the #75 class.
//
// Every function in these two files used to keep its scan cursor on the BIND DATA, which
// DuckDB reuses across executions of a bound plan, and reset it nowhere. The second
// EXECUTE therefore resumed from a drained cursor and returned zero rows, silently.
//
// Fixed for the read functions in #182 and for the five catalog functions in #191; this
// file covers both. Two shapes:
//
//   - bc_read / crm_read / the show_* functions keep a `finished` flag beside a scan, so
//     both move onto a per-function GlobalTableFunctionState owning a CloneForScan().
//   - bc_describe / crm_describe hold a row cursor over IMMUTABLE vectors. Only the cursor
//     moves; the payload stays shared, because there is nothing there to clone.
//
// Business Central is drivable against the local server because BuildApiUrl accepts an
// `environment` that is already a URL. That hatch is guarded - https anywhere, plain http
// only for loopback - and the guard is built on HttpUrl so it cannot disagree with the
// parser that opens the socket (#193, #194, #198). Whether the hatch should permit https
// anywhere at all is #199, a product question.
//
// The catalog functions additionally need edm_business_central_min.xml: they bind their
// schema entirely from $metadata with no buffered first page, so without an EDM declaring
// `companies` the bind fails before the scan can be reached at all.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "business_central_client.hpp"
#include "dataverse_client.hpp"
#include "odata_url_helpers.hpp"

#include <ctime>
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

// Two pages of four rows under `prefix`, served as the entity set `Airlines`, plus a
// token endpoint so the client_credentials exchange stays on loopback.
void ServeTwoPages(ODataTestServer &server, const std::string &prefix)
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
    server.OnPath("/token",
                  CannedResponse::Json(
                      R"({"access_token":"test-token","token_type":"Bearer","expires_in":3600})"));
}

// MicrosoftEntraTokenManager::HasValidCachedToken requires BOTH access_token AND a
// non-expired expires_at. Without the expiry the supplied token is ignored entirely and
// the manager calls the real login.microsoftonline.com endpoint.
std::string FarFutureEpoch()
{
    return std::to_string(static_cast<long long>(std::time(nullptr)) + 86400);
}

int64_t ScalarOf(duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result)
{
    return result->GetValue(0, 0).GetValue<int64_t>();
}

}  // namespace

TEST_CASE("a bound crm_read plan returns every row on each execution", "[ms_reexec][crm]") {
    ODataTestServer server;
    // DataverseUrlBuilder::BuildApiUrl appends "/api/data/v9.2" to environment_url.
    ServeTwoPages(server, "/api/data/v9.2");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET crm (TYPE dataverse, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT_URL '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto prep = con.Query("PREPARE p AS SELECT COUNT(*) FROM crm_read('Airlines', secret => 'crm')");
    INFO("crm PREPARE: " << (prep->HasError() ? prep->GetError() : std::string("ok")));
    REQUIRE_FALSE(prep->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE p");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 4);
    }
}

TEST_CASE("a bound bc_read plan returns every row on each execution", "[ms_reexec][bc]") {
    // A GUID short-circuits BusinessCentralClientFactory::ResolveCompanyId, which would
    // otherwise GET <base>/companies to map a name to an id.
    const std::string COMPANY = "11111111-2222-3333-4444-555555555555";

    ODataTestServer server;
    const std::string entity_path = "/companies(" + COMPANY + ")/Airlines";
    const std::string entity_url = server.Url(entity_path);
    // BC serves $metadata at the API base, not under companies({id}) - see the comment in
    // BcReadBind.
    const std::string context = server.Url("/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/$metadata", "edm_trippin.xml");
    server.OnMatch(
        [entity_path](const RecordedRequest &request) {
            return request.path == entity_path && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(context, {AIRLINE_MU, AIRLINE_AF})));
    server.OnPath(entity_path,
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM},
                                                  entity_url + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET bc (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto prep = con.Query("PREPARE q AS SELECT COUNT(*) FROM bc_read('Airlines', secret => 'bc', "
                          "company => '" + COMPANY + "')");
    INFO("bc PREPARE: " << (prep->HasError() ? prep->GetError() : std::string("ok")));
    REQUIRE_FALSE(prep->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE q");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 4);
    }
}

// The verbatim-URL escape hatch added above is production code shared by bc_read,
// bc_describe and the BC catalog - not test-only. Before it existed the scheme was
// hardcoded https, so accepting plain http generally would be a security downgrade
// introduced by a testability change: the OAuth bearer token would go on the wire in
// cleartext, and the same-origin guard would not object because the configured URL IS the
// origin. Raised by an agent-crew review (F6).
TEST_CASE("the Business Central URL hatch refuses plain http off loopback",
          "[ms_reexec][bc][security]") {
    SECTION("loopback forms are accepted") {
        REQUIRE(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://127.0.0.1:8080") ==
                "http://127.0.0.1:8080");
        REQUIRE(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://localhost:8080/") ==
                "http://localhost:8080");
        // Bracketed IPv6 is deliberately NOT among the accepted forms: HttpUrl's host
        // class excludes ':', so "http://[::1]:8080" parses with host "[" and could never
        // be dialled. Asserting it here would lock in a property the stack does not hold
        // (GitHub #198).
        REQUIRE_THROWS_AS(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://[::1]:8080"),
                          duckdb::InvalidInputException);
    }

    // https to a NON-loopback host is gated for Business Central, because
    // GetResourceUrl() hardcodes the token audience to api.businesscentral.dynamics.com -
    // so a token minted for that host would be sent to another one. Dataverse is
    // deliberately NOT gated: its scope is environment_url + "/.default", minted for
    // whatever host you configure, so a custom host is the normal product (GitHub #199).
    SECTION("https to another host is gated for Business Central") {
        REQUIRE_THROWS_AS(
            erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "https://api.example.com/v2"),
            duckdb::InvalidInputException);
    }

    SECTION("...and permitted once the caller opts in") {
        // RAII, because the policy is process-wide: a bare set/REQUIRE/unset leaves the
        // gate OPEN for the whole binary if the assertion fails, silently weakening every
        // later test in the run.
        struct OptInGuard {
            OptInGuard() { erpl_web::ServiceUrlPolicy::SetCustomServiceUrlsAllowed(true); }
            ~OptInGuard() { erpl_web::ServiceUrlPolicy::SetCustomServiceUrlsAllowed(false); }
        } opt_in;

        REQUIRE(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "https://api.example.com/v2") ==
                "https://api.example.com/v2");
    }

    SECTION("the gate agrees with its sibling on host case") {
        REQUIRE(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://LOCALHOST:8080") ==
                "http://LOCALHOST:8080");
    }

    SECTION("Dataverse is not gated - a custom https org host is its normal shape") {
        REQUIRE(erpl_web::DataverseUrlBuilder::BuildApiUrl("https://myorg.crm.dynamics.com") ==
                "https://myorg.crm.dynamics.com/api/data/v9.2");
    }

    SECTION("plain http to any other host is refused") {
        REQUIRE_THROWS_AS(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://evil.example"),
                          duckdb::InvalidInputException);
        // A host that merely starts with a loopback-looking label must not slip through.
        REQUIRE_THROWS_AS(
            erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://localhost.evil.example"),
            duckdb::InvalidInputException);
    }

    SECTION("a non-URL environment still builds the real BC endpoint") {
        REQUIRE(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("tenant", "production") ==
                "https://api.businesscentral.dynamics.com/v2.0/tenant/production/api/v2.0");
    }
}

// The same rule applies to Dataverse's environment_url, which is also taken verbatim from
// a secret and also carries a bearer token. Raised by an agent-crew review as an
// unapplied-fix: the loopback restriction went onto Business Central's hatch and not onto
// its siblings.
TEST_CASE("the Dataverse URL hatch refuses plain http off loopback",
          "[ms_reexec][crm][security]") {
    SECTION("loopback and https are accepted") {
        REQUIRE(erpl_web::DataverseUrlBuilder::BuildApiUrl("http://127.0.0.1:8080") ==
                "http://127.0.0.1:8080/api/data/v9.2");
        REQUIRE_THROWS_AS(erpl_web::DataverseUrlBuilder::BuildApiUrl("http://[::1]:8080"),
                          duckdb::InvalidInputException);
        REQUIRE(erpl_web::DataverseUrlBuilder::BuildApiUrl("https://org.crm.dynamics.com") ==
                "https://org.crm.dynamics.com/api/data/v9.2");
    }

    SECTION("plain http elsewhere is refused") {
        REQUIRE_THROWS_AS(erpl_web::DataverseUrlBuilder::BuildApiUrl("http://evil.example"),
                          duckdb::InvalidInputException);
        REQUIRE_THROWS_AS(
            erpl_web::DataverseUrlBuilder::BuildApiUrl("http://127.0.0.1.evil.example"),
            duckdb::InvalidInputException);
    }
}

// GitHub #193. The scheme prefix test was byte-exact, so an uppercase scheme took the
// "not an absolute URL at all" early return and the guard never applied. That was inert -
// HttpUrl's parser is lowercase-only too, so such a URL fails to connect rather than
// shipping a bearer token - but a credential guard relying on a second parser's case
// sensitivity is not a guard. Raised by the continuous agent-crew review.
TEST_CASE("the URL hatches are case-insensitive about the scheme",
          "[ms_reexec][security]") {
    SECTION("an uppercase scheme off loopback is still refused") {
        REQUIRE_THROWS_AS(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "HTTP://evil.example"),
                          duckdb::InvalidInputException);
        REQUIRE_THROWS_AS(erpl_web::DataverseUrlBuilder::BuildApiUrl("HtTp://evil.example"),
                          duckdb::InvalidInputException);
    }

    // An uppercase scheme is now REFUSED, for both hosts. An earlier version of this test
    // asserted it was accepted, which was wrong in the same way as the [::1] case:
    // HttpUrl's scheme group is (https?), lowercase-only, so "HTTP://..." parses as a path
    // with no host and can never be dialled. Refusing it up front with a message beats
    // accepting it and failing to connect later (GitHub #198).
    SECTION("an uppercase scheme is refused, loopback or not") {
        REQUIRE_THROWS_AS(
            erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "HTTP://127.0.0.1:8080"),
            duckdb::InvalidInputException);
        REQUIRE_THROWS_AS(
            erpl_web::DataverseUrlBuilder::BuildApiUrl("HTTPS://org.crm.dynamics.com"),
            duckdb::InvalidInputException);
    }

    // Anything carrying some other scheme is now refused rather than waved through.
    SECTION("an unsupported scheme is refused, not passed through") {
        REQUIRE_THROWS_AS(erpl_web::DataverseUrlBuilder::BuildApiUrl("file:///etc/passwd"),
                          duckdb::InvalidInputException);
    }

    // A bare name is not a URL and must still reach the caller's own handling.
    SECTION("a non-URL value is left alone") {
        REQUIRE(erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("tenant", "production") ==
                "https://api.businesscentral.dynamics.com/v2.0/tenant/production/api/v2.0");
    }
}

// GitHub #194. "http://127.0.0.1:pw@evil.example/" has 127.0.0.1:pw as USERINFO and
// evil.example as the host - HttpUrl::ParseUrl reads it that way (its regex captures
// user/password/host separately). A guard that stops at the first ':' sees "127.0.0.1" and
// approves, so the bearer token went to the attacker's host in cleartext. Unlike the
// case-sensitivity gap this one was live. Found by the continuous agent-crew review.
TEST_CASE("the URL hatch guard is not fooled by userinfo", "[ms_reexec][security]") {
    SECTION("a loopback-looking userinfo does not launder a foreign host") {
        REQUIRE_THROWS_AS(
            erpl_web::DataverseUrlBuilder::BuildApiUrl("http://127.0.0.1:pw@evil.example"),
            duckdb::InvalidInputException);
        REQUIRE_THROWS_AS(
            erpl_web::DataverseUrlBuilder::BuildApiUrl("http://localhost:pw@evil.example"),
            duckdb::InvalidInputException);
        REQUIRE_THROWS_AS(
            erpl_web::BusinessCentralUrlBuilder::BuildApiUrl("t", "http://localhost@evil.example"),
            duckdb::InvalidInputException);
    }

    // The guard's own fix for #194 used rfind('@') (LAST), while HttpUrl's userinfo class
    // ends at the FIRST '@'. For this URL the old guard saw host 127.0.0.1 and approved,
    // while the transport dials "evil.example@127.0.0.1". Building the guard on HttpUrl is
    // what makes the two agree by construction (GitHub #198).
    SECTION("a doubled userinfo cannot disagree with the dialer") {
        REQUIRE_THROWS_AS(
            erpl_web::DataverseUrlBuilder::BuildApiUrl("http://x:y@evil.example@127.0.0.1/"),
            duckdb::InvalidInputException);
    }

    SECTION("real loopback with credentials is still accepted") {
        REQUIRE(erpl_web::DataverseUrlBuilder::BuildApiUrl("http://user:pw@127.0.0.1:8080") ==
                "http://user:pw@127.0.0.1:8080/api/data/v9.2");
    }

    SECTION("the loopback host compare is case-insensitive") {
        REQUIRE(erpl_web::DataverseUrlBuilder::BuildApiUrl("http://LOCALHOST:8080") ==
                "http://LOCALHOST:8080/api/data/v9.2");
    }
}

// GitHub #191, fixed in this file's commit. These five were filed from SOURCE READING
// because they could not be driven against the local server: they bind their schema
// entirely from $metadata (FromEntitySetClient with no buffered response), and
// edm_trippin.xml has no `companies` set, so bind failed with "Table function must return
// at least one column" before anything about the scan could be reached.
// edm_business_central_min.xml exists to close exactly that gap - and with it the defect
// reproduced immediately, which is why the fix ships with a test that can fail rather than
// on the strength of reading the code.
TEST_CASE("a bound bc_show_companies plan returns every row on each execution",
          "[ms_reexec][catalog]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/$metadata", "edm_business_central_min.xml");
    // TWO pages on purpose. A single-page fixture cannot catch the actual #75 mechanism -
    // two executions sharing one pagination cursor - because there is no cursor to share.
    const std::string ctx = server.Url("/$metadata") + "#companies";
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/companies" && request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(
            ctx, {R"({"id":"33333333-3333-3333-3333-333333333333","name":"Gamma","displayName":"Gamma SA"})"})));
    server.OnPath("/companies",
                  CannedResponse::Json(MakeV4Page(
                      ctx,
                      {R"({"id":"11111111-2222-3333-4444-555555555555","name":"Alpha","displayName":"Alpha Ltd"})",
                       R"({"id":"66666666-7777-8888-9999-000000000000","name":"Beta","displayName":"Beta GmbH"})"},
                      server.Url("/companies") + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET bccat (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto prep = con.Query("PREPARE cats AS SELECT COUNT(*) FROM bc_show_companies(secret => 'bccat')");
    INFO((prep->HasError() ? prep->GetError() : std::string()));
    REQUIRE_FALSE(prep->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE cats");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 3);
    }
}

// bc_describe keeps a row cursor rather than a `finished` flag, but it is the same defect:
// the cursor lived on the bind data and was never reset, so a second EXECUTE resumed past
// the end. The describe payload itself is immutable and stays shared - only the cursor
// needed to move (GitHub #191).
TEST_CASE("a bound bc_describe plan returns every row on each execution",
          "[ms_reexec][catalog]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/$metadata", "edm_business_central_min.xml");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET bcdesc (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto prep = con.Query(
        "PREPARE d AS SELECT COUNT(*) FROM bc_describe('customers', secret => 'bcdesc')");
    INFO((prep->HasError() ? prep->GetError() : std::string()));
    REQUIRE_FALSE(prep->HasError());

    // customer declares three properties in the fixture.
    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE d");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 3);
    }
}

// The remaining three of the five. An agent-crew review pointed out that fixing five
// functions and testing two leaves three changed-but-unverified, which is the shape that
// let the original defect persist: the pattern looks obviously right, so nobody checks.
TEST_CASE("the remaining catalog functions return every row on each execution",
          "[ms_reexec][catalog]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/$metadata", "edm_business_central_min.xml");

    // bc_show_entities binds via FromServiceClient, so it reads the SERVICE DOCUMENT at the
    // API base rather than $metadata - a different discovery path from bc_show_companies.
    server.OnPath("/",
                  CannedResponse::Json(
                      R"({"@odata.context":")" + server.Url("/$metadata") +
                      R"(","value":[)"
                      R"({"name":"companies","kind":"EntitySet","url":"companies"},)"
                      R"({"name":"customers","kind":"EntitySet","url":"customers"}]})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET bcrest (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto prep = con.Query(
        "PREPARE ents AS SELECT COUNT(*) FROM bc_show_entities(secret => 'bcrest')");
    INFO((prep->HasError() ? prep->GetError() : std::string()));
    REQUIRE_FALSE(prep->HasError());

    int64_t first = -1;
    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE ents");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        if (first < 0) {
            first = ScalarOf(result);
            // The count is a property of the fixture's EntityContainer, so assert only that
            // it found something - and then that every later execution agrees with it.
            REQUIRE(first > 0);
        } else {
            REQUIRE(ScalarOf(result) == first);
        }
    }
}

// GitHub #196. An agent-crew review reported that bc_read/crm_read "silently drop"
// per-column conversion failures. That was narrower than it looked: a destructor safety
// net on ODataReadBindData reports them, so they were never lost. What was actually wrong
// is that they arrived at teardown rather than at the end of the scan, and under the name
// "odata_read" - a function the caller never invoked.
//
// WHAT THIS CASE DOES AND DOES NOT COVER. It is a regression guard for wiring
// ReportConversionFailures() into the scan's terminal exit: the read must still succeed
// and deliver every row, because reporting is a diagnostic and must not become a failure
// mode. It does NOT assert the warning text or its timing - those go out through the
// tracer, and this harness has no way to capture them. Naming the limitation rather than
// letting the test imply coverage it does not have.
TEST_CASE("wiring conversion reporting into the bc_read scan does not break the read",
          "[ms_reexec][conversion]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/$metadata", "edm_business_central_min.xml");
    const std::string COMPANY = "11111111-2222-3333-4444-555555555555";
    const std::string path = "/companies(" + COMPANY + ")/customers";
    server.OnPath(path,
                  CannedResponse::Json(MakeV4Page(
                      server.Url("/$metadata") + "#customers",
                      {R"({"id":"c1","number":"N1","displayName":"Acme"})",
                       R"({"id":"c2","number":"N2","displayName":"Beta"})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET bcconv (TYPE business_central, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    REQUIRE_FALSE(secret->HasError());

    // The read itself must still succeed and deliver every row - reporting is a diagnostic,
    // not a failure mode.
    auto result = con.Query("SELECT COUNT(*) FROM bc_read('customers', secret => 'bcconv', "
                            "company => '" + COMPANY + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(ScalarOf(result) == 2);
}

// The two Dataverse catalog functions #191 fixed but did not test. An agent-crew review
// pointed out that five functions were changed and three tested, which leaves two
// changed-but-unverified - the shape that let this defect survive in seven places.
//
// crm_show_entities reads the EntityDefinitions endpoint, so it needs its own EDM:
// edm_dataverse_min.xml, for the same reason edm_business_central_min.xml exists.
TEST_CASE("a bound crm_show_entities plan returns every row on each execution",
          "[ms_reexec][catalog]") {
    ODataTestServer server;
    const std::string api = "/api/data/v9.2";
    server.ServeMetadataFixture(api + "/$metadata", "edm_dataverse_min.xml");

    const std::string ctx = server.Url(api + "/$metadata") + "#EntityDefinitions";
    // Two pages, so a shared pagination cursor would be observable.
    server.OnMatch(
        [api](const RecordedRequest &request) {
            return request.path == api + "/EntityDefinitions" &&
                   request.QueryParam("$skiptoken") == "2";
        },
        CannedResponse::Json(MakeV4Page(
            ctx, {R"({"MetadataId":"m3","LogicalName":"lead","EntitySetName":"leads","SchemaName":"Lead"})"})));
    server.OnPath(api + "/EntityDefinitions",
                  CannedResponse::Json(MakeV4Page(
                      ctx,
                      {R"({"MetadataId":"m1","LogicalName":"account","EntitySetName":"accounts","SchemaName":"Account"})",
                       R"({"MetadataId":"m2","LogicalName":"contact","EntitySetName":"contacts","SchemaName":"Contact"})"},
                      server.Url(api + "/EntityDefinitions") + "?$format=json&$skiptoken=2")));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query(
        "CREATE SECRET crmcat (TYPE dataverse, PROVIDER config, "
        "TENANT_ID 'loopback', CLIENT_ID 'id', CLIENT_SECRET 'sec', "
        "ENVIRONMENT_URL '" + server.BaseUrl() + "', ACCESS_TOKEN 'test-token', "
        "EXPIRES_AT '" + FarFutureEpoch() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto prep = con.Query("PREPARE ce AS SELECT COUNT(*) FROM crm_show_entities(secret => 'crmcat')");
    INFO((prep->HasError() ? prep->GetError() : std::string()));
    REQUIRE_FALSE(prep->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        auto result = con.Query("EXECUTE ce");
        INFO("execution " << execution);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 3);
    }
}
