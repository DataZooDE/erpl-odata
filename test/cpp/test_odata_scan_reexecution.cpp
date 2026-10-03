// GitHub #166: odata_scan_reexecution.test and odata_expand.test were the last SQL tests
// still reaching services.odata.org over the public internet, and a v1.4.5 release leg
// segfaulted in exactly that pair once, then never reproduced. A transient response - a
// truncated body, a connection dropped mid-stream - reaching a parser that assumes a
// complete document is the shape that explains a crash which appears once and never
// again, and it is untestable while the responses come from a third party.
//
// This file moves those guarantees onto ODataTestServer, where the failure shape can be
// produced on purpose. It covers what test_odata_server_e2e.cpp did not: the projecting
// re-execution plans, the equi-join and UNION ALL two-scan shapes, the ATTACHed catalog
// scans including the GitHub #132 row-id cases, and a service that dies mid-response.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"

#include <string>
#include <vector>
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

// A two-page entity set of four airlines, served under `prefix`. Paging is routed on the
// skiptoken rather than on call order, so the assertions hold however often binding
// re-fetches the first page.
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
}

int64_t ScalarOf(duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result)
{
    return result->GetValue(0, 0).GetValue<int64_t>();
}

}  // namespace

// ----------------------------------------------------------------------
// Re-execution of a bound plan (GitHub #75)

// test_odata_server_e2e.cpp covers SELECT *, which emits no $select and so takes the
// early-return path in UpdateUrlFromPredicatePushdown. A projecting plan takes the
// rebuild path instead, and the two are not the same code: the SQL test covered this
// shape and nothing in C++ did.
TEST_CASE("a bound projecting plan returns every row on each execution",
          "[odata_reexec][paging]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/reexecproj");
    const std::string entity_url = server.Url("/reexecproj/Airlines");

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    REQUIRE_FALSE(
        con.Query("PREPARE counted AS SELECT COUNT(*) FROM odata_read('" + entity_url + "')")
            ->HasError());
    REQUIRE_FALSE(con.Query("PREPARE distinct_codes AS SELECT COUNT(DISTINCT AirlineCode) FROM "
                            "odata_read('" + entity_url + "')")
                      ->HasError());

    for (int execution = 1; execution <= 3; execution++) {
        INFO("execution " << execution);

        auto counted = con.Query("EXECUTE counted");
        INFO((counted->HasError() ? counted->GetError() : std::string()));
        REQUIRE_FALSE(counted->HasError());
        REQUIRE(ScalarOf(counted) == 4);

        auto distinct_codes = con.Query("EXECUTE distinct_codes");
        INFO((distinct_codes->HasError() ? distinct_codes->GetError() : std::string()));
        REQUIRE_FALSE(distinct_codes->HasError());
        REQUIRE(ScalarOf(distinct_codes) == 4);
    }
}

// ----------------------------------------------------------------------
// Two scans of one call inside a single query

TEST_CASE("two scans of one call join, cross-join and union independently",
          "[odata_reexec][paging]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/twoscan");
    const std::string read = "odata_read('" + server.Url("/twoscan/Airlines") + "')";

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    SECTION("cross join sees every pair") {
        auto result = con.Query("SELECT COUNT(*) FROM " + read + " a, " + read + " b");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 16);
    }

    SECTION("equi-join matches every row with itself") {
        auto result = con.Query("SELECT COUNT(*) FROM " + read + " a JOIN " + read +
                                " b ON a.AirlineCode = b.AirlineCode");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 4);
    }

    SECTION("union all returns both scans in full") {
        auto result = con.Query("SELECT COUNT(*) FROM (SELECT AirlineCode FROM " + read +
                                " UNION ALL SELECT AirlineCode FROM " + read + ")");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 8);
    }
}

// ----------------------------------------------------------------------
// The same guarantees for an ATTACHed service (odata_table_scan)

TEST_CASE("an ATTACHed OData table re-executes and self-joins", "[odata_reexec][attach]") {
    ODataTestServer server;
    ServeTwoPageAirlines(server, "/attached");

    // ATTACH resolves the metadata context from the service document at the service root,
    // which a plain entity-set read never requests.
    server.OnPath("/attached/",
                  CannedResponse::Json(
                      R"({"@odata.context":")" + server.Url("/attached/$metadata") +
                      R"(","value":[{"name":"Airlines","kind":"EntitySet","url":"Airlines"}]})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto attach = con.Query("ATTACH '" + server.Url("/attached/") + "' AS svc (TYPE odata)");
    INFO((attach->HasError() ? attach->GetError() : std::string()));
    REQUIRE_FALSE(attach->HasError());

    SECTION("self-join over a projected column sees the whole table on both sides") {
        auto result = con.Query(
            "SELECT COUNT(a.AirlineCode) FROM svc.Airlines a "
            "JOIN svc.Airlines b ON a.AirlineCode = b.AirlineCode");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 4);
    }

    SECTION("a bound catalog scan returns every row on each execution") {
        REQUIRE_FALSE(
            con.Query("PREPARE airlines AS SELECT COUNT(AirlineCode) FROM svc.Airlines")
                ->HasError());
        for (int execution = 1; execution <= 3; execution++) {
            INFO("execution " << execution);
            auto result = con.Query("EXECUTE airlines");
            INFO((result->HasError() ? result->GetError() : std::string()));
            REQUIRE_FALSE(result->HasError());
            REQUIRE(ScalarOf(result) == 4);
        }
    }

    // GitHub #132. COUNT(*) makes DuckDB ask for COLUMN_IDENTIFIER_ROW_ID alone. Row ids
    // take no part in $select but still occupy a slot in the output chunk, and dropping
    // them from the projection without remembering WHICH slots they held made the emit
    // path fall back to "output slot j is data column j" - writing the first column's
    // string into a BIGINT row-id vector.
    SECTION("COUNT(*) over an ATTACHed table counts rows") {
        auto result = con.Query("SELECT COUNT(*) FROM svc.Airlines");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(ScalarOf(result) == 4);
    }

    SECTION("a row-id slot beside a data column does not shift either") {
        auto result = con.Query(
            "SELECT COUNT(*), COUNT(DISTINCT AirlineCode) FROM svc.Airlines");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 4);
        REQUIRE(result->GetValue(1, 0).GetValue<int64_t>() == 4);
    }
}

// The row-id projection has to survive alongside predicate pushdown, which is a different
// code path again: the scan then owns the filter outright, because DuckDB removes a
// predicate from the plan once it becomes a TableFilter.
//
// This needs its own service. A route that answers any $filter would also catch the
// dynamic join filter DuckDB pushes into the self-join above, and quietly shrink that
// scan to one row.
TEST_CASE("COUNT(*) over an ATTACHed table coexists with a pushed-down filter",
          "[odata_reexec][attach]") {
    ODataTestServer server;

    // Routes match in registration order, so the filtered page is declared before the
    // catch-all that ServeTwoPageAirlines installs. The canned server does not implement
    // $filter; it only has to answer the request the reader actually sends.
    server.OnMatch(
        [](const RecordedRequest &request) {
            return request.path == "/filtered/Airlines" && request.HasQueryParam("$filter");
        },
        CannedResponse::Json(
            MakeV4Page(server.Url("/filtered/$metadata") + "#Airlines", {AIRLINE_AA})));

    ServeTwoPageAirlines(server, "/filtered");
    server.OnPath("/filtered/",
                  CannedResponse::Json(
                      R"({"@odata.context":")" + server.Url("/filtered/$metadata") +
                      R"(","value":[{"name":"Airlines","kind":"EntitySet","url":"Airlines"}]})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto attach = con.Query("ATTACH '" + server.Url("/filtered/") + "' AS svc (TYPE odata)");
    INFO((attach->HasError() ? attach->GetError() : std::string()));
    REQUIRE_FALSE(attach->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM svc.Airlines WHERE AirlineCode = 'AA'");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(ScalarOf(result) == 1);

    // The count alone would not prove the filter was pushed: DuckDB would apply it itself
    // if the scan had not taken it. So assert it reached the wire.
    bool filtered = false;
    for (const auto &request : server.RequestsFor("/filtered/Airlines")) {
        if (request.DecodedQueryParam("$filter").find("AirlineCode") != std::string::npos) {
            filtered = true;
        }
    }
    REQUIRE(filtered);
}

// ----------------------------------------------------------------------
// A service that dies mid-response - the shape GitHub #166 suspects

// The whole point of #166: a truncated body must surface as an error, not a crash, and
// must leave the database usable. A plain SELECT afterwards is the strongest evidence,
// since it shares nothing with OData but fails anyway once the instance is invalidated.
TEST_CASE("a body truncated mid-stream fails cleanly and spares the database",
          "[odata_reexec][truncation]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/cut/Airlines");
    const std::string context = server.Url("/cut/$metadata") + "#Airlines";

    server.ServeMetadataFixture("/cut/$metadata", "edm_trippin.xml");
    const auto page = MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM, AIRLINE_MU, AIRLINE_AF});
    server.OnPath("/cut/Airlines",
                  CannedResponse::Json(page).TruncatedAfter(page.size() / 2));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "')");
    INFO("truncated read reported: "
         << (result->HasError() ? result->GetError() : std::string("no error")));
    // Two outcomes are acceptable: refuse the half document, or deliver all four rows.
    // A SILENT PREFIX is not - reporting success over two of four rows is a wrong answer
    // presented as a right one, which is a worse failure than the crash #166 started from.
    //
    // The earlier version of this assertion was `ScalarOf(result) >= 0`, which no result
    // can fail. It was written to say "we do not mind which outcome" and instead said
    // nothing at all - the exact assert-values-not-absence-of-a-crash trap this file was
    // supposed to close.
    if (result->HasError()) {
        REQUIRE(result->GetErrorObject().Type() != duckdb::ExceptionType::INTERNAL);
    } else {
        INFO("a truncated body was reported as success with " << ScalarOf(result)
             << " of 4 rows - a silent partial extraction");
        REQUIRE(ScalarOf(result) == 4);
    }

    auto after = con.Query("SELECT 42");
    INFO((after->HasError() ? after->GetError() : std::string()));
    REQUIRE_FALSE(after->HasError());
    REQUIRE(after->GetValue(0, 0).GetValue<int32_t>() == 42);
}

// The same for metadata, which is parsed by a different (XML) path and happens before any
// row is read, so a short document lands somewhere else entirely.
TEST_CASE("a truncated $metadata document fails cleanly and spares the database",
          "[odata_reexec][truncation]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/cutmeta/Airlines");
    const std::string context = server.Url("/cutmeta/$metadata") + "#Airlines";

    const auto edmx = erpl_web::test_support::ReadFixture("edm_trippin.xml");
    server.OnPath("/cutmeta/$metadata",
                  CannedResponse::Xml(edmx).TruncatedAfter(edmx.size() / 4));
    server.OnPath("/cutmeta/Airlines",
                  CannedResponse::Json(MakeV4Page(context, {AIRLINE_AA, AIRLINE_FM})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // The reader does not fail here, and that is deliberate rather than accidental:
    // ODataReadBindData::MetadataColumnNames() catches an unavailable EDM and falls back
    // to inferring the columns from the first row of the payload. So a half-delivered
    // $metadata degrades to a JSON-inferred schema instead of taking the query down.
    //
    // What this pins is the boundary that matters for #166 - whatever the reader decides
    // to do with a short document, it must not be an internal error, and the database must
    // still be usable afterwards.
    auto result = con.Query("SELECT COUNT(*) FROM odata_read('" + entity_url + "')");
    INFO("truncated metadata reported: "
         << (result->HasError() ? result->GetError() : std::string("no error")));
    if (result->HasError()) {
        REQUIRE(result->GetErrorObject().Type() != duckdb::ExceptionType::INTERNAL);
    } else {
        REQUIRE(ScalarOf(result) == 2);
    }

    auto after = con.Query("SELECT 42");
    INFO((after->HasError() ? after->GetError() : std::string()));
    REQUIRE_FALSE(after->HasError());
    REQUIRE(after->GetValue(0, 0).GetValue<int32_t>() == 42);
}

// ----------------------------------------------------------------------
// Predicate pushdown column mapping
//
// Ported from odata_expand.test's SECTION 8, which had no C++ equivalent. A WHERE on a
// column whose position in the OUTPUT differs from its position in the SCHEMA used to be
// translated through the output index, naming the wrong OData property in $filter. The
// service then matched nothing and the query returned zero rows with no error at all -
// the silent kind. Filtering on a late column while projecting an earlier one is exactly
// the shape that makes the two indices disagree.
TEST_CASE("a filter on a late schema column names that column in $filter",
          "[odata_reexec][pushdown]") {
    ODataTestServer server;
    const std::string entity_url = server.Url("/mapping/People");
    const std::string context = server.Url("/mapping/$metadata") + "#People";

    server.ServeMetadataFixture("/mapping/$metadata", "edm_trippin.xml");
    server.OnPath("/mapping/People",
                  CannedResponse::Json(MakeV4Page(
                      context,
                      {R"({"UserName":"russellwhyte","FirstName":"Russell","LastName":"Whyte"})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // LastName is the third property; UserName the first. Projecting UserName alone while
    // filtering on LastName makes the output index (0) and the schema index (2) disagree.
    auto result = con.Query("SELECT UserName FROM odata_read('" + entity_url +
                            "') WHERE LastName = 'Whyte'");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);

    const auto requests = server.RequestsFor("/mapping/People");
    bool named_the_right_column = false;
    bool named_a_wrong_column = false;
    for (const auto &request : requests) {
        const auto filter = request.DecodedQueryParam("$filter");
        if (filter.empty()) {
            continue;
        }
        INFO("$filter on the wire: " << filter);
        if (filter.find("LastName") != std::string::npos) {
            named_the_right_column = true;
        } else {
            named_a_wrong_column = true;
        }
    }
    REQUIRE(named_the_right_column);
    REQUIRE_FALSE(named_a_wrong_column);

    // $select must carry every column the query needs - both the projected one and the
    // filtered one - or the service returns rows the filter cannot be evaluated against.
    bool select_has_both = false;
    for (const auto &request : requests) {
        const auto select = request.DecodedQueryParam("$select");
        if (select.find("UserName") != std::string::npos &&
            select.find("LastName") != std::string::npos) {
            select_has_both = true;
        }
    }
    INFO("no request carried a $select naming both columns");
    REQUIRE(select_has_both);
}
