// GitHub #179: odp_odata_read returns NULL for every explicitly projected column, while
// SELECT * returns the values. No error, no warning, and the row count is correct - so an
// aggregate over a named column returns a confidently wrong answer, and COUNT(*) agreeing
// with reality while COUNT(col) reads 0 is a disagreement nobody checks for.
//
// Cause: UpdateODataClientWithResponse replaces odata_bind_data_ wholesale with a fresh
// instance that carries no column selection. FetchAndLoadNextPage re-applies the
// selection afterwards and says why; HandleInitialLoad and HandleDeltaFetch call the same
// replacement and do not. So the FIRST page of every scan runs in all-columns mode, and
// only page two onwards was ever correct.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_test_server.hpp"
#include "odp_test_db.hpp"

#include <sstream>
#include <string>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;

namespace {

// D_NW_DIV and ODQ_CHANGEMODE are real properties of edm_sap_odp_bw_fact.xml; the schema
// comes from the EDM, so an invented name binds to nothing.
std::string MakeOdpPage(std::size_t row_count)
{
    std::ostringstream out;
    out << R"({"d":{"results":[)";
    for (std::size_t i = 0; i < row_count; i++) {
        if (i > 0) {
            out << ",";
        }
        out << R"({"CALMONTH":"CM)" << i << R"(","D_NW_DIV":"DV)" << i
            << R"(","ODQ_CHANGEMODE":"C"})";
    }
    out << "]}}";
    return out.str();
}

const char *const ODP_PATH = "/sap/opu/odata/sap/Z_TEST_SRV/FactsOf0D_NW_C01";

void ServeOdpEntitySet(ODataTestServer &server, std::size_t rows)
{
    server.ServeMetadataFixture("/sap/opu/odata/sap/Z_TEST_SRV/$metadata",
                                "edm_sap_odp_bw_fact.xml");
    server.ServeMetadataFixture(std::string(ODP_PATH) + "/$metadata",
                                "edm_sap_odp_bw_fact.xml");
    server.OnPath(ODP_PATH, CannedResponse::Json(MakeOdpPage(rows)));
}

}  // namespace

TEST_CASE("odp_odata_read projects the column the query asked for", "[odp_projection]") {
    constexpr std::size_t ROW_COUNT = 90;

    ODataTestServer server;
    ServeOdpEntitySet(server, ROW_COUNT);

    // ODP state is refused an in-memory catalog on purpose (GitHub #92).
    odp_test::TempDatabase db("odp_projection");
    auto &con = db.Conn();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const auto url = server.Url(ODP_PATH);

    // Every assertion here is on VALUES, not on nullness. The defect does not produce
    // NULLs when the payload carries the schema's leading properties - it produces the
    // WRONG column's data under the requested name, with the right row count and the right
    // type. A "not null" assertion passes straight through it, which is how it survived:
    // against the live A4H service, SELECT ItemName returned ItemId's values and looked
    // entirely plausible. CALMONTH is schema column 0 and D_NW_DIV is not, so the two are
    // told apart by their distinct value prefixes.
    SECTION("a single projected column carries its own values") {
        auto result = con.Query("SELECT D_NW_DIV FROM odp_odata_read('" + url + "') LIMIT 3");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 3);
        for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
            const auto value = result->GetValue(0, row).ToString();
            INFO("row " << row << " D_NW_DIV = " << value
                        << " (CM* would mean CALMONTH's data arrived instead)");
            REQUIRE(value == "DV" + std::to_string(row));
        }
    }

    SECTION("a projected column that is not first in the schema is not shifted") {
        auto result = con.Query("SELECT ODQ_CHANGEMODE FROM odp_odata_read('" + url +
                                "') LIMIT 3");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        // Without a row-count assertion the loop below asserts nothing on an empty result -
        // which is exactly the drained-buffer shape these tests exist to catch.
        REQUIRE(result->RowCount() == 3);
        for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
            INFO("row " << row << " = " << result->GetValue(0, row).ToString());
            REQUIRE(result->GetValue(0, row).ToString() == "C");
        }
    }

    SECTION("two projected columns each carry their own values") {
        auto result = con.Query("SELECT D_NW_DIV, ODQ_CHANGEMODE FROM odp_odata_read('" + url +
                                "') LIMIT 3");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 3);
        for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
            INFO("row " << row << ": D_NW_DIV=" << result->GetValue(0, row).ToString()
                        << " ODQ_CHANGEMODE=" << result->GetValue(1, row).ToString());
            REQUIRE(result->GetValue(0, row).ToString() == "DV" + std::to_string(row));
            REQUIRE(result->GetValue(1, row).ToString() == "C");
        }
    }

    // Reversing the order relative to the schema catches a fix that merely re-maps by
    // position rather than by name.
    SECTION("projected columns in non-schema order keep their own values") {
        auto result = con.Query("SELECT ODQ_CHANGEMODE, CALMONTH FROM odp_odata_read('" + url +
                                "') LIMIT 3");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 3);
        for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
            INFO("row " << row << ": ODQ_CHANGEMODE=" << result->GetValue(0, row).ToString()
                        << " CALMONTH=" << result->GetValue(1, row).ToString());
            REQUIRE(result->GetValue(0, row).ToString() == "C");
            REQUIRE(result->GetValue(1, row).ToString() == "CM" + std::to_string(row));
        }
    }

    // The unprojected path was always correct, and is the only reason this stayed hidden.
    SECTION("SELECT * is unaffected") {
        auto result = con.Query("SELECT CALMONTH, D_NW_DIV FROM (SELECT * FROM odp_odata_read('" +
                                url + "')) LIMIT 3");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 3);
        for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
            REQUIRE(result->GetValue(0, row).ToString() == "CM" + std::to_string(row));
            REQUIRE(result->GetValue(1, row).ToString() == "DV" + std::to_string(row));
        }
    }
}
