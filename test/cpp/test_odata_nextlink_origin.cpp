// GitHub #183: a cross-origin @odata.nextLink receives the caller's Authorization header.
//
// ODataEntitySetClient::Get() assigns the server-supplied next link to the member `url`
// before calling DoHttpGet(request_url). DoHttpGet's guard reads
//
//     if (modified_url.IsSameOrigin(url)) { http_request.AuthHeadersFromParams(...); }
//
// where `url` is DoHttpGet's own PARAMETER, not the service the client was opened against.
// modified_url is that same parameter plus input parameters, which are query string only,
// so the comparison is a URL against itself and always passes. The guard cannot fire on
// the path it was written for.
//
// Server-driven paging follows whatever URL the service puts in @odata.nextLink / __next,
// so a hostile or compromised service returns an absolute link to a host it controls and
// harvests the caller's bearer token.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odata_url_helpers.hpp"

#include <string>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;

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

const char *const AIRLINE_AA = R"({"AirlineCode":"AA","Name":"American Airlines"})";
const char *const AIRLINE_FM = R"({"AirlineCode":"FM","Name":"Shanghai Airline"})";

}  // namespace

TEST_CASE("a cross-origin next link never receives the caller's credentials",
          "[odata_origin][security]") {
    // Declared before the database so both outlive every connection that talks to them.
    ODataTestServer trusted;
    ODataTestServer foreign;

    const std::string context = trusted.Url("/svc/$metadata") + "#Airlines";
    trusted.ServeMetadataFixture("/svc/$metadata", "edm_trippin.xml");

    // Page one is served by the service the user named, and points at a DIFFERENT host.
    trusted.OnPath("/svc/Airlines",
                   CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA},
                                                   foreign.Url("/steal/Airlines"))));
    foreign.OnPath("/steal/Airlines", CannedResponse::Json(MakeV4Page(context, {AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // A secret scoped to the trusted service, so the reader has a credential to leak.
    auto secret = con.Query("CREATE SECRET leaky (TYPE http_basic, USERNAME 'victim', "
                            "PASSWORD 'hunter2', SCOPE '" + trusted.BaseUrl() + "')");
    INFO((secret->HasError() ? secret->GetError() : std::string()));
    REQUIRE_FALSE(secret->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + trusted.Url("/svc/Airlines") +
                            "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    // Assert the read actually succeeded and spanned both pages. Without this the whole
    // test can pass on a build where binding or page-one parsing broke: the next link is
    // never followed, the foreign-host loop below iterates zero times, and the #183 guard
    // goes untested while the test stays green.
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 2);

    // The trusted host must have been given the credential - otherwise this test would
    // pass for the wrong reason, having never authenticated at all.
    bool trusted_was_authenticated = false;
    for (const auto &request : trusted.RequestsFor("/svc/Airlines")) {
        if (!request.Header("Authorization").empty()) {
            trusted_was_authenticated = true;
        }
    }
    INFO("the trusted service was never sent credentials, so this proves nothing");
    REQUIRE(trusted_was_authenticated);

    // The foreign host MUST have been contacted - following a next link is the documented
    // behaviour, and an empty request set would make the loop below assert nothing.
    INFO("the cross-origin next link was never followed, so the guard is untested");
    REQUIRE_FALSE(foreign.RequestsFor("/steal/Airlines").empty());

    // ...but it must never see the caller's credentials.
    for (const auto &request : foreign.Requests()) {
        INFO("foreign host " << request.method << " " << request.target
                             << " Authorization=[" << request.Header("Authorization") << "]");
        REQUIRE(request.Header("Authorization").empty());
    }
}

// GitHub #189. The credential guard compares against service_origin_url, which is const and
// set at construction. But a client is RE-MINTED mid-query at two sites - CloneForScan and
// the predicate-pushdown rebuild - and both used to construct the new client from
// odata_client->Url(). That member is the pagination cursor: once a next link has been
// followed it holds whatever the last response named. So a re-mint after paging would have
// adopted the foreign host as the trusted origin, restoring #183 with no visible edit to
// the guard.
//
// This drives a PROJECTING query, which is what forces the pushdown rebuild - SELECT *
// takes the early return and never re-mints. Raised by an agent-crew review.
TEST_CASE("the trusted origin survives a client rebuilt after paging",
          "[odata_origin][security]") {
    ODataTestServer trusted;
    ODataTestServer foreign;

    const std::string context = trusted.Url("/svc2/$metadata") + "#Airlines";
    trusted.ServeMetadataFixture("/svc2/$metadata", "edm_trippin.xml");
    trusted.OnPath("/svc2/Airlines",
                   CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA},
                                                   foreign.Url("/steal2/Airlines"))));
    foreign.OnPath("/steal2/Airlines", CannedResponse::Json(MakeV4Page(context, {AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto secret = con.Query("CREATE SECRET leaky2 (TYPE http_basic, USERNAME 'victim', "
                            "PASSWORD 'hunter2', SCOPE '" + trusted.BaseUrl() + "')");
    REQUIRE_FALSE(secret->HasError());

    // Projecting: emits $select, changes the URL, and so rebuilds the client.
    auto result = con.Query("SELECT AirlineCode FROM odata_read('" +
                            trusted.Url("/svc2/Airlines") + "') ORDER BY AirlineCode");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);

    bool trusted_was_authenticated = false;
    for (const auto &request : trusted.RequestsFor("/svc2/Airlines")) {
        if (!request.Header("Authorization").empty()) {
            trusted_was_authenticated = true;
        }
    }
    INFO("the trusted service was never sent credentials, so this proves nothing");
    REQUIRE(trusted_was_authenticated);

    INFO("the cross-origin next link was never followed, so the guard is untested");
    REQUIRE_FALSE(foreign.RequestsFor("/steal2/Airlines").empty());

    for (const auto &request : foreign.Requests()) {
        INFO("foreign host " << request.method << " " << request.target
                             << " Authorization=[" << request.Header("Authorization") << "]");
        REQUIRE(request.Header("Authorization").empty());
    }
}

// A service-supplied next link goes into the REQUEST LINE, and every OData request sets
// url_encode = false - which installs the verbatim target writer, so httplib's own escaping
// does not apply. A link carrying CR/LF keeps its host (HttpUrl's parser matches both inside
// path and query), passes the same-origin check, and would inject a header onto a request
// carrying the caller's credentials.
//
// The guard was first placed only in the Graph seam; these paths were missed. The test is
// per-seam deliberately: a shared predicate does not help a caller that never invokes it.
TEST_CASE("an OData next link with control characters is refused, not followed",
          "[odata_origin][security]") {
    ODataTestServer server;

    const std::string context = server.Url("/svc/$metadata") + "#Airlines";
    server.ServeMetadataFixture("/svc/$metadata", "edm_trippin.xml");

    // Page one points at a link on the SAME origin that smuggles a header break.
    // JSON-escaped, not raw: a literal CR/LF inside a JSON string is invalid and the body
    // would fail to parse before the link was ever reached - which is how the first version
    // of this test "passed" on an unrelated error.
    const std::string hostile =
        server.Url("/svc/Airlines") + "?$skiptoken=2\\r\\nX-Injected: 1";
    server.OnPath("/svc/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA}, hostile)));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + server.Url("/svc/Airlines") + "')");
    REQUIRE(result->HasError());

    const auto error = result->GetError();
    INFO("error was: " << error);
    // Wording only the NEXT-LINK guard produces. Asserting the generic "cannot be sent"
    // matched the choke-point guard too, so reverting the next-link guard alone left this
    // green - it did not pin the seam it ships with.
    REQUIRE(error.find("next link containing characters") != std::string::npos);

    // Page one must actually have been fetched, or the assertions below are vacuous.
    REQUIRE_FALSE(server.RequestsFor("/svc/Airlines").empty());

    // No request carried the smuggled header. Asserting over `target` could not fail:
    // httplib's request-line parser truncates it at the first CRLF, so the bytes would
    // never appear there even when they were sent.
    for (const auto &request : server.Requests()) {
        INFO("target: " << request.target);
        REQUIRE_FALSE(request.HasHeader("X-Injected"));
        REQUIRE(request.target.find("skiptoken") == std::string::npos);
    }
}

// A caller's percent-encoded query value survives every request the read makes.
//
// This case used to assert the opposite half of the story. The predicate pushdown decoded
// query values when it parsed them and re-emitted them raw, so a user's "%20" came back out
// as a literal space in the REBUILT query - and a literal space ends the request target, so
// the follow-up request never arrived intact. At the time this test pinned only the guard's
// scope (that DoHttpGet was not what refused the URL), and said so, because asserting
// success would have asserted behaviour that did not exist.
//
// GitHub #227 made the encoding survive: values are held as decoded plaintext and encoded
// exactly once on the way out. So the assertion is now the one worth having - the read
// succeeds, and every request carries the value encoded rather than split across a space.
TEST_CASE("a caller's percent-encoded query value survives into every request",
          "[odata_origin][security]") {
    ODataTestServer server;

    const std::string context = server.Url("/svc/$metadata") + "#Airlines";
    server.ServeMetadataFixture("/svc/$metadata", "edm_trippin.xml");
    server.OnPath("/svc/Airlines", CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + server.Url("/svc/Airlines") +
                            "?$orderby=Name%20desc')");
    INFO("error was: " << (result->HasError() ? result->GetError() : std::string("<none>")));
    REQUIRE_FALSE(result->HasError());

    const auto requests = server.RequestsFor("/svc/Airlines");
    REQUIRE_FALSE(requests.empty());

    // Every request - the first one built straight from the caller's URL, and any the
    // pushdown rebuilt with $select added - carries the value encoded. A rebuilt request
    // used to arrive here as "$orderby=Name desc", whose space ends the target.
    for (const auto &request : requests) {
        INFO("target: " << request.target);
        REQUIRE(request.target.find("$orderby=Name%20desc") != std::string::npos);
        REQUIRE(request.target.find("Name desc") == std::string::npos);
    }
}

// ODataClientFactory::ProbeUrl builds its own request and never enters DoHttpGet, which is
// why the claim that DoHttpGet was "the single point every OData request passes through"
// was false. It now carries its own check.
//
// That check is DEFENCE IN DEPTH and is not driven end to end here, deliberately: HttpUrl's
// parser truncates a caller URL at a raw CR before ToPathQuery() ever sees it, so the guard
// cannot observe the input it exists to refuse. An end-to-end case would assert a
// connection error and look like coverage without being any. What is testable, and what
// makes the guard safe to apply to a caller's URL at all, is the predicate itself.

// The corollary, stated because it is what makes the guard safe to apply to a caller's URL:
// a properly percent-encoded CRLF is not refused. It reaches the wire encoded, where it is
// six ordinary characters and cannot split anything.
TEST_CASE("a percent-encoded CRLF in a caller's URL is not refused",
          "[odata_origin][security]") {
    REQUIRE(erpl_web::HasNoControlCharacters(
        "http://host/svc/Airlines?$filter=A%0d%0aX-Injected:%201"));
    REQUIRE_FALSE(erpl_web::HasNoControlCharacters("http://host/svc/Airlines?$filter=A\r\nX: 1"));
}

// #228 narrowed both OData guards from ToString() to ToPathQuery() to exclude the fragment,
// and in doing so stopped checking scheme, host and port as well. The host matters here:
// it is service-chosen through @odata.context, HttpUrl's parser accepts control characters
// inside it, and httplib composes the Host header from it. The guards now cover everything
// except the fragment.
TEST_CASE("the wire-safety check covers the host, not just the path and query",
          "[odata_origin][security]") {
    // A control character in the HOST is refused...
    REQUIRE_FALSE(erpl_web::IsWireSafeUrl("https://ho\rst.example/svc/Airlines"));
    REQUIRE_FALSE(erpl_web::HasNoControlCharacters("https://ho\nst.example/svc/Airlines"));
    // ...as it is in the path and query.
    REQUIRE_FALSE(erpl_web::IsWireSafeUrl("https://host.example/svc/A\rirlines"));
    REQUIRE_FALSE(erpl_web::IsWireSafeUrl("https://host.example/svc?x=a\nb"));

    // A clean URL passes whichever part it exercises.
    REQUIRE(erpl_web::IsWireSafeUrl("https://host.example:8080/svc/Airlines?$top=1"));
}

// The message must not hand the control characters it just refused to whatever reads the
// log or the terminal.
TEST_CASE("a refused URL is sanitized before it reaches a message",
          "[odata_origin][security]") {
    const auto summary = erpl_web::SummariseUrlForMessage("https://host/x\r\nX-Injected: 1");
    REQUIRE(summary.find('\r') == std::string::npos);
    REQUIRE(summary.find('\n') == std::string::npos);
    REQUIRE(summary.find("X-Injected") != std::string::npos);  // still legible

    // And it is capped, so a pathological URL cannot flood a log line.
    const auto long_summary = erpl_web::SummariseUrlForMessage(std::string(500, 'a'));
    REQUIRE(long_summary.size() < 200);
}

// The metadata retry has three cases and they are not the same: a transient failure must
// repeat the SAME request, a 404 means the URL is wrong and falls back one level, and any
// other 4xx is a deliberate answer that must not be retried at all. The code popped the
// path on transient failures too, contradicting the comment above it.
//
// WHAT THIS TEST DOES AND DOES NOT PIN. It asserts that a 503 followed by success yields
// the right rows - real coverage of the retry working at all, which nothing had. It does
// NOT discriminate pop-from-repeat: removing the fix leaves it green. PopPath on
// "/svc/Airlines" gives "/svc/", and merging a relative "$metadata" against that lands back
// on "/svc/$metadata" - the same URL - so the first pop is a no-op at any shallow path, and
// an absolute @odata.context ignores the popped base entirely. Distinguishing the two would
// need a deeply nested service path and a relative context together. Said here rather than
// left implied, because a test that looks like it pins a fix and does not is worse than no
// test at all.
TEST_CASE("a transient metadata failure repeats the same request", "[odata_origin]") {
    ODataTestServer server;

    // 503 first, then the real metadata at the same path.
    std::vector<CannedResponse> metadata_responses;
    metadata_responses.push_back(CannedResponse::Error(503, R"({"error":"slow down"})"));
    metadata_responses.push_back(
        CannedResponse::Xml(erpl_web::test_support::ReadFixture("edm_trippin.xml")));
    server.OnPathSequence("/svc/$metadata", metadata_responses);

    // A relative @odata.context, which is the shape the header comment discusses. It does
    // not make the test discriminating - see there for why.
    server.OnPath("/svc/Airlines",
                  CannedResponse::Json(MakeV4Page("$metadata#Airlines", {AIRLINE_AA})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + server.Url("/svc/Airlines") + "')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);

    // The retry happened at all, and reached the same path.
    REQUIRE(server.RequestsFor("/svc/$metadata").size() >= 2);
}

// Every guard checks the bytes a request line actually carries, and WireTargetOf is the one
// definition of what those are. This exists because the composition was narrowed to the path
// alone twice - #229 wrote the ProbeUrl check that way and #230 widened two siblings without
// widening it - each time leaving the HOST unchecked, which is where a service-chosen
// @odata.context puts its authority and where httplib reads the Host header from.
//
// WHAT THIS PINS: the composition, and that a control character anywhere in it is refused.
// WHAT IT DOES NOT: that any particular guard calls it. HttpUrl truncates a caller URL at a
// raw CR before ProbeUrl's guard can observe it, so ProbeUrl cannot be driven end to end;
// routing every guard through this helper is what makes the composition hard to narrow
// again, not this test.
TEST_CASE("the wire target is scheme, host, port, path and query", "[odata_origin][security]") {
    const erpl_web::HttpUrl url("https://host.example:8080/svc/Airlines?$top=1#frag");
    const auto target = erpl_web::WireTargetOf(url);

    REQUIRE(target.find("host.example") != std::string::npos);
    REQUIRE(target.find("8080") != std::string::npos);
    REQUIRE(target.find("/svc/Airlines") != std::string::npos);
    REQUIRE(target.find("$top=1") != std::string::npos);
    // The fragment is never sent, so it is not part of what a guard inspects.
    REQUIRE(target.find("frag") == std::string::npos);
}

TEST_CASE("a control character anywhere in the wire target is refused",
          "[odata_origin][security]") {
    // Host, path and query in turn - narrowing the composition to any one of them would
    // let the others through.
    REQUIRE_FALSE(erpl_web::HasNoControlCharacters("https://ho\rst.example/svc"));
    REQUIRE_FALSE(erpl_web::HasNoControlCharacters("https://host.example/sv\nc"));
    REQUIRE_FALSE(erpl_web::HasNoControlCharacters("https://host.example/svc?x=a\rb"));
    REQUIRE(erpl_web::HasNoControlCharacters("https://host.example:8080/svc?x=a%0db"));
}

// The ODP __delta link is server-supplied and becomes the next credentialed request, so it
// gets both checks every sibling gets: same origin, and sendable at all. It was the last
// follower holding only the origin half, and the new half had no test.
//
// Predicate-level: reaching HandleDeltaFetch needs a live ODP service with an open delta
// queue. What is pinned here is the rule the adoption applies - a link carrying CR/LF or a
// space is not adoptable, whatever its origin.
TEST_CASE("an unsendable ODP delta link is not adoptable", "[odata_origin][security][odp]") {
    const std::string service = "https://sap.example:44300/sap/opu/odata/sap/ZODP_SRV/Facts";

    // Same origin, but unsendable: origin alone would have adopted these.
    REQUIRE_FALSE(erpl_web::IsWireSafeUrl(service + "?!deltatoken=D2026\r\nX-Injected: 1"));
    REQUIRE_FALSE(erpl_web::IsWireSafeUrl(service + "?!deltatoken=D 2026"));
    REQUIRE_FALSE(erpl_web::IsWireSafeUrl(service + "?!deltatoken=D\t2026"));

    // A real delta link is unaffected.
    REQUIRE(erpl_web::IsWireSafeUrl(service + "?!deltatoken=D20260916093000_000123000"));
}
