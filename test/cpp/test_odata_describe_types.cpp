// odata_describe() must report the type the reader will actually bind.
//
// It used to map the EDM type NAME alone, which is blind to everything the reader is
// not: an Edm.Decimal's Precision/Scale facets, Collection(...) and complex types. A
// column the reader binds as DECIMAL(38,18), VARCHAR[] or a STRUCT was described as
// "DECIMAL" or "VARCHAR", so the one function whose entire job is to answer "what will
// this column be?" disagreed with the answer. (GitHub #254, follow-up)

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

namespace {


// One entity type exercising every facet-sensitive shape at once.
const char *const DESCRIBE_EDMX = R"XML(<?xml version="1.0" encoding="utf-8"?>
<edmx:Edmx Version="4.0" xmlns:edmx="http://docs.oasis-open.org/odata/ns/edmx">
  <edmx:DataServices>
    <Schema Namespace="Facets" xmlns="http://docs.oasis-open.org/odata/ns/edm">
      <ComplexType Name="Money">
        <Property Name="Amount" Type="Edm.Decimal" Precision="16" Scale="3"/>
        <Property Name="Currency" Type="Edm.String"/>
      </ComplexType>
      <EntityType Name="Asset">
        <Key><PropertyRef Name="Id"/></Key>
        <Property Name="Id" Type="Edm.String" Nullable="false"/>
        <Property Name="NoPrecision" Type="Edm.Decimal" Scale="18"/>
        <Property Name="Declared" Type="Edm.Decimal" Precision="16" Scale="3"/>
        <Property Name="ZeroScale" Type="Edm.Decimal" Precision="19" Scale="0"/>
        <Property Name="NoScale" Type="Edm.Decimal" Precision="19"/>
        <Property Name="Tags" Type="Collection(Edm.String)"/>
        <Property Name="Cost" Type="Facets.Money"/>
      </EntityType>
      <EntityContainer Name="Container">
        <EntitySet Name="Assets" EntityType="Facets.Asset"/>
      </EntityContainer>
    </Schema>
  </edmx:DataServices>
</edmx:Edmx>)XML";

// The describe row is a single STRUCT list; pull one property's field out by name.
std::string DescribedField(duckdb::Connection &con, const std::string &url,
                           const std::string &property, const std::string &field)
{
    auto result = con.Query("SELECT p." + field + " FROM odata_describe('" + url +
                            "'), UNNEST(properties) AS t(p) WHERE p.name = '" + property + "'");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    auto value = result->GetValue(0, 0);
    return value.IsNull() ? std::string("NULL") : value.ToString();
}

}  // namespace

TEST_CASE("odata_describe reports the type the reader binds", "[odata_describe][decimal]") {
    ODataTestServer server;
    server.ServeMetadata("/desc/$metadata", DESCRIBE_EDMX);
    server.OnPath("/desc/Assets",
                  CannedResponse::Json(MakeV4Page(server.Url("/desc/$metadata") + "#Assets", {})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());
    const auto url = server.Url("/desc/Assets");

    SECTION("decimal facets decide the described type") {
        // The GitHub #254 shape: no Precision, so it widens instead of collapsing to
        // the unusable DECIMAL(18,18).
        REQUIRE(DescribedField(con, url, "NoPrecision", "duckdb_type") == "DECIMAL(38,18)");
        REQUIRE(DescribedField(con, url, "Declared", "duckdb_type") == "DECIMAL(16,3)");
        REQUIRE(DescribedField(con, url, "ZeroScale", "duckdb_type") == "DECIMAL(19,0)");
        // An absent Scale reads as DOUBLE, exactly as the reader binds it (GitHub #80).
        REQUIRE(DescribedField(con, url, "NoScale", "duckdb_type") == "DOUBLE");
    }

    SECTION("collections and complex types are not flattened to VARCHAR") {
        REQUIRE(DescribedField(con, url, "Tags", "duckdb_type") == "VARCHAR[]");
        REQUIRE(DescribedField(con, url, "Cost", "duckdb_type") ==
                "STRUCT(Amount DECIMAL(16,3), Currency VARCHAR)");
    }

    SECTION("an absent facet is NULL, which an explicit zero is not") {
        // Reporting both as 0 hid the one distinction that decides the type: Scale="0"
        // yields DECIMAL(p,0), an absent Scale yields DOUBLE.
        REQUIRE(DescribedField(con, url, "ZeroScale", "scale") == "0");
        REQUIRE(DescribedField(con, url, "NoScale", "scale") == "NULL");
        REQUIRE(DescribedField(con, url, "NoPrecision", "precision") == "NULL");
        REQUIRE(DescribedField(con, url, "Declared", "precision") == "16");
        REQUIRE(DescribedField(con, url, "Id", "max_length") == "NULL");
    }

    SECTION("plain primitives are unchanged") {
        REQUIRE(DescribedField(con, url, "Id", "duckdb_type") == "VARCHAR");
        REQUIRE(DescribedField(con, url, "Id", "edm_type") == "Edm.String");
    }
}
