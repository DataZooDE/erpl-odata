// Per-execution scan state for the Microsoft Graph SharePoint and Excel readers, and for
// the ATTACHed SharePoint list scan - the #75 class, reported as GitHub #202.
//
// Every one of these functions kept its scan cursor - the parsed yyjson document, the
// array iterator over it, and a `done` flag - on the BIND DATA, which DuckDB reuses
// across executions of a bound plan. Worse, the parse step cleared the buffered response
// as it consumed it, so a second execution had nothing left to re-parse either. The
// second EXECUTE returned zero rows, with no error.
//
// The cursor now lives on GraphJsonArrayScanState (and ExcelRangeScanState for
// graph_excel_range), both GlobalTableFunctionState, created fresh per execution.
//
// WHAT THIS FILE DOES AND DOES NOT COVER
//
// GraphClient::BaseUrl() is a hardcoded https://graph.microsoft.com/v1.0 with no
// override, so - unlike Business Central, whose `environment` may already be a URL -
// these functions cannot be driven against the loopback ODataTestServer without adding a
// new base-URL escape hatch. Such a hatch decides where an OAuth bearer token is sent and
// is a security change well beyond the scope of a state-lifetime fix, so it is not made
// here.
//
// So this file asserts the structural property the fix consists of: every Graph scan
// function declares an init_global, which is the ONLY way a DuckDB scan can hold
// per-execution state. A function whose cursor is back on the bind data has no
// init_global to declare. This is not a substitute for an end-to-end test, but it is
// falsifiable and it is driven by a name table, so a new Graph reader must be added to it
// - an agent-crew review found excel_table_scan missed by the first cut of this fix
// precisely because nothing asserted the wiring. It is paired with direct coverage of the
// state objects themselves below. The full end-to-end re-execution behaviour is covered
// against a real server for the readers that can reach one
// (test_odata_scan_reexecution, test_microsoft_scan_reexecution,
// test_delta_share_scan_reexecution).
//
// Note on the include set: Catalog::GetEntry<TableFunctionCatalogEntry> odr-uses that
// entry type's static `Name`, which the linker then reports as duplicated against
// libduckdb_static.a. The untemplated GetEntry plus Cast<> avoids that and is why the
// lookup below is written the long way.

#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"

#include "graph_json_scan.hpp"

#include <set>
#include <string>
#include <vector>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;


// Walks the catalog rather than a hand-written list of names. A list is the mechanism
// that already failed once: the first cut of the #202 fix missed excel_table_scan because
// nothing enumerated what existed. Every table function this extension registers must
// either declare an init_global - the only way a DuckDB scan can hold per-execution state
// - or appear in the exemption list below with a reason.
//
// init_global is necessary, not sufficient: a function could declare one and still read a
// cursor out of bind data. It is asserted because its absence is decisive.
TEST_CASE("every registered erpl_odata table function declares per-execution state",
          "[graph][reexec]") {
    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // One-shot writers: the write happens in the scan, so latching on the bind data is
    // what stops a second EXECUTE from repeating it against the user's data. Deliberate,
    // and stated at each scan.
    const std::set<std::string> exempt = {
        "graph_excel_add_rows",
        "graph_excel_delete_rows",
        // Side-effecting DDL: creates a view per entity set with replace = Overwrite(),
        // which defaults to false, so a second EXECUTE would fail with "already exists".
        "odata_attach",
        // The mutating HTTP verbs issue their request in the scan, so repeating it per
        // execution would repeat the write. http_get / http_head do re-issue.
        "http_post",
        "http_put",
        "http_patch",
        "http_delete",
    };

    // The extension's own functions, by registered prefix. Anything DuckDB itself
    // registers is out of scope.
    const std::vector<std::string> prefixes = {
        "graph_", "sac_", "bc_", "crm_", "odata_", "odp_", "datasphere_", "delta_share_",
        "sap_", "erpl_", "http_",
    };
    const auto is_ours = [&prefixes](const std::string &name) {
        for (const auto &prefix : prefixes) {
            if (name.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    };

    auto &context = *con.context;
    context.transaction.BeginTransaction();
    // Extension functions are registered in the SYSTEM catalog, not in the default
    // database's schema - a walk of the latter matches nothing, which is what the
    // seen.size() guard below exists to catch.
    auto &schema = duckdb::Catalog::GetSystemCatalog(context).GetSchema(context, DEFAULT_SCHEMA);

    std::vector<std::string> missing;
    std::vector<std::string> seen;
    schema.Scan(context, duckdb::CatalogType::TABLE_FUNCTION_ENTRY, [&](duckdb::CatalogEntry &entry) {
        const auto name = entry.name;
        if (!is_ours(name) || exempt.count(name) > 0) {
            return;
        }
        seen.push_back(name);
        for (const auto &overload : entry.Cast<duckdb::TableFunctionCatalogEntry>().functions.functions) {
            if (overload.init_global == nullptr) {
                missing.push_back(name);
                return;
            }
        }
    });
    context.transaction.Commit();

    // Guards against the walk silently matching nothing, which would make the assertion
    // below vacuous.
    REQUIRE(seen.size() > 20);

    std::string report;
    for (const auto &name : missing) {
        report += name + " ";
    }
    INFO("scanned " << seen.size() << " erpl_odata table functions");
    INFO("table functions with no init_global: " << report);
    REQUIRE(missing.empty());
}

// The two ATTACHed scans (sharepoint_list_scan, excel_table_scan) are never registered as
// named functions - each is built on demand by its table entry's GetScanFunction - so the
// walk above cannot reach them, and there is no assertion here that would catch a revert
// of either wiring: constructing the TableFunction needs a live catalog entry, which needs
// credentials. A first attempt asserted `GraphJsonArrayScanState::Init != nullptr`, which
// is the address of a static member function and therefore always true - it was removed
// rather than left looking like coverage. excel_table_scan, which the name-list version of
// this test could not have caught either, is the reason this gap is called out.

TEST_CASE("a fresh GraphJsonArrayScanState re-reads the whole payload", "[graph][reexec]") {
    const std::string payload = R"({"value":[{"id":"1"},{"id":"2"},{"id":"3"}]})";

    // Two states over the same response - which is what two executions of one bound plan
    // now get - each walk all three items. Before the fix there was one iterator, shared,
    // and the payload was destroyed by the first parse.
    for (int execution = 1; execution <= 2; execution++) {
        INFO("execution " << execution);
        erpl_web::GraphJsonArrayScanState state;
        REQUIRE(state.NeedsFetch());
        state.SeedFrom(payload);
        REQUIRE_FALSE(state.NeedsFetch());
        REQUIRE(state.InitIterator());

        int seen = 0;
        while (duckdb_yyjson::yyjson_arr_iter_next(&state.item_iter) != nullptr) {
            seen++;
        }
        REQUIRE(seen == 3);
    }
}

TEST_CASE("GraphJsonArrayScanState reports a payload it cannot use", "[graph][reexec]") {
    SECTION("unparseable JSON") {
        erpl_web::GraphJsonArrayScanState state;
        state.SeedFrom("{not json");
        REQUIRE_FALSE(state.InitIterator());
    }

    SECTION("the array key is absent") {
        erpl_web::GraphJsonArrayScanState state;
        state.SeedFrom(R"({"error":{"code":"itemNotFound"}})");
        REQUIRE_FALSE(state.InitIterator());
    }

    SECTION("the array key holds something other than an array") {
        erpl_web::GraphJsonArrayScanState state;
        state.SeedFrom(R"({"value":"not-an-array"})");
        REQUIRE_FALSE(state.InitIterator());
    }
}

// GitHub #202 review, F1: the Teams and Outlook readers followed @odata.nextLink exactly
// one page on an exhausted iterator. A page whose array is empty while still advertising a
// next link therefore ended the scan, silently dropping every page after it. That is data
// loss on a FIRST execution, not only on a re-execution.
TEST_CASE("GraphPagedScanState follows nextLink across empty pages", "[graph][reexec]") {
    const std::string page_one =
        R"({"value":[{"id":"1"}],"@odata.nextLink":"http://svc/p2"})";
    const std::string page_two_empty =
        R"({"value":[],"@odata.nextLink":"http://svc/p3"})";
    const std::string page_three =
        R"({"value":[{"id":"2"},{"id":"3"}]})";

    erpl_web::GraphPagedScanState state;
    REQUIRE(state.LoadPage(page_one));

    std::vector<std::string> fetched;
    const auto fetch = [&](const std::string &url) {
        fetched.push_back(url);
        if (url == "http://svc/p2") {
            return state.LoadPage(page_two_empty);
        }
        if (url == "http://svc/p3") {
            return state.LoadPage(page_three);
        }
        return false;
    };

    int seen = 0;
    while (state.NextItem(fetch) != nullptr) {
        seen++;
    }

    REQUIRE(seen == 3);
    REQUIRE(fetched.size() == 2);
    REQUIRE(fetched[0] == "http://svc/p2");
    REQUIRE(fetched[1] == "http://svc/p3");
}

// A service that returns the link it was just called with must end the scan rather than
// spin forever.
TEST_CASE("GraphPagedScanState stops on a self-referential nextLink", "[graph][reexec]") {
    const std::string looping = R"({"value":[],"@odata.nextLink":"http://svc/same"})";

    erpl_web::GraphPagedScanState state;
    REQUIRE(state.LoadPage(R"({"value":[],"@odata.nextLink":"http://svc/same"})"));

    int fetches = 0;
    const auto fetch = [&](const std::string &) {
        fetches++;
        return state.LoadPage(looping);
    };

    REQUIRE(state.NextItem(fetch) == nullptr);
    REQUIRE(fetches == 1);
}

// A page that cannot be used must leave the cursor where it was rather than installing a
// half-loaded document and the failed page's next link.
TEST_CASE("GraphPagedScanState keeps its position when a page is unusable",
          "[graph][reexec]") {
    erpl_web::GraphPagedScanState state;
    REQUIRE(state.LoadPage(R"({"value":[{"id":"1"},{"id":"2"}],"@odata.nextLink":"http://svc/p2"})"));
    REQUIRE(duckdb_yyjson::yyjson_arr_iter_next(&state.item_iter) != nullptr);

    REQUIRE_FALSE(state.LoadPage(R"({"error":{"code":"throttled"}})"));
    REQUIRE(state.next_url == "http://svc/p2");
    // The surviving page still yields its second item.
    REQUIRE(duckdb_yyjson::yyjson_arr_iter_next(&state.item_iter) != nullptr);
}

// GitHub #202 third review, F4: following links across empty pages (which the reader must
// do, see above) makes an unbounded authenticated request loop reachable. The
// self-reference check catches A->A; a two-element cycle needs the page cap.
TEST_CASE("GraphPagedScanState stops on a multi-element nextLink cycle", "[graph][reexec]") {
    erpl_web::GraphPagedScanState state;
    REQUIRE(state.LoadPage(R"({"value":[],"@odata.nextLink":"http://svc/a"})"));

    // Alternates a -> b -> a -> ..., so no single step ever repeats its own URL.
    size_t fetches = 0;
    const auto fetch = [&](const std::string &url) {
        fetches++;
        const std::string other = (url == "http://svc/a") ? "http://svc/b" : "http://svc/a";
        return state.LoadPage(R"({"value":[],"@odata.nextLink":")" + other + R"("})");
    };

    REQUIRE_THROWS_AS(state.NextItem(fetch), duckdb::IOException);
    REQUIRE(fetches <= erpl_web::GraphPagedScanState::MAX_PAGES + 1);
    REQUIRE(fetches > 1);
}
