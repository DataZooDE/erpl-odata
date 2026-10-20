#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_compat/filter.hpp"
#include "odata_test_server.hpp"
#include "test_database.hpp"
#include "test_extension_load_sql.hpp"

#include <iostream>
#include <string>
#include <vector>

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;
using erpl_web::test_support::TestDatabase;

TEST_CASE("expression table filters keep OData results and never fail a scan", "[odata_expression]") {
    const std::string metadata = R"(<?xml version="1.0" encoding="utf-8"?>
<edmx:Edmx Version="4.0" xmlns:edmx="http://docs.oasis-open.org/odata/ns/edmx">
<edmx:DataServices><Schema Namespace="Test" xmlns="http://docs.oasis-open.org/odata/ns/edm">
<EntityType Name="Item"><Key><PropertyRef Name="c"/></Key>
<Property Name="c" Type="Edm.Int32" Nullable="false"/>
<Property Name="Name" Type="Edm.String"/></EntityType>
<EntityContainer Name="Container"><EntitySet Name="Items" EntityType="Test.Item"/></EntityContainer>
</Schema></edmx:DataServices></edmx:Edmx>)";
    const std::vector<std::string> all = {
        R"({"c":1,"Name":null})", R"({"c":6,"Name":"Alpha"})",
        R"({"c":7,"Name":"Beta"})", R"({"c":8,"Name":"Gamma"})"};
    struct Case {
        const char *name;
        const char *query;
        std::vector<std::string> filtered;
        int64_t count;
        std::string expected_filter;
    };
    const std::string in_filter = duckdb_compat::detail::HasLegacyFilterKind<duckdb::TableFilterType>::value
                                      ? "" : "(c eq 1 or c eq 7)";
    const std::vector<Case> cases = {
        {"is_null", "Name IS NULL", {all[0]}, 1, ""},
        {"is_not_null", "Name IS NOT NULL", {all[1], all[2], all[3]}, 3, ""},
        {"like", "Name LIKE 'A%'", {all[1]}, 1, "(Name ge 'A' and Name lt 'B')"},
        {"not_equal", "NOT (c = 1)", {all[1], all[2], all[3]}, 3, "c ne 1"},
        {"in", "c IN (1, 7)", {all[0], all[2]}, 2, in_filter},
        {"length", "length(Name) > 4", {all[1], all[3]}, 2, ""},
    };
    for (const auto &test : cases) {
        INFO(test.name);
        ODataTestServer server;
        const auto context = server.Url("/expr/$metadata") + "#Items";
        server.ServeMetadata("/expr/$metadata", metadata);
        if (!test.expected_filter.empty()) {
            server.OnMatch([&](const RecordedRequest &request) {
                return request.path == "/expr/Items" &&
                       request.DecodedQueryParam("$filter") == test.expected_filter;
            }, CannedResponse::Json(MakeV4Page(context, test.filtered)));
        }
        server.OnPath("/expr/Items", CannedResponse::Json(MakeV4Page(context, all)));
        TestDatabase db;
        auto &con = db.Con();
        REQUIRE_FALSE(con.Query(ErplOdataLoadSql())->HasError());
        const auto sql = "SELECT COUNT(*) FROM odata_read('" + server.Url("/expr/Items") +
                         "') WHERE " + test.query;
        auto result = con.Query(sql);
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
        const auto requests = server.RequestsFor("/expr/Items");
        REQUIRE_FALSE(requests.empty());
        for (const auto &request : requests) {
            std::cout << "expression wire " << test.name << ": "
                      << request.DecodedQueryParam("$filter") << std::endl;
        }
        REQUIRE(requests.back().DecodedQueryParam("$filter") == test.expected_filter);
        REQUIRE(duckdb_compat::ResultValue(*result, 0, 0).GetValue<int64_t>() == test.count);
    }
}

TEST_CASE("required comparison survives an optional Top-N filter", "[odata_expression]") {
    ODataTestServer server;
    const std::string metadata = R"(<?xml version="1.0" encoding="utf-8"?>
<edmx:Edmx Version="4.0" xmlns:edmx="http://docs.oasis-open.org/odata/ns/edmx">
<edmx:DataServices><Schema Namespace="Test" xmlns="http://docs.oasis-open.org/odata/ns/edm">
<EntityType Name="Item"><Key><PropertyRef Name="c"/></Key><Property Name="c" Type="Edm.Int32" Nullable="false"/></EntityType>
<EntityContainer Name="Container"><EntitySet Name="Items" EntityType="Test.Item"/></EntityContainer>
</Schema></edmx:DataServices></edmx:Edmx>)";
    const auto context = server.Url("/top/$metadata") + "#Items";
    server.ServeMetadata("/top/$metadata", metadata);
    server.OnMatch([](const RecordedRequest &request) {
        return request.path == "/top/Items" && request.DecodedQueryParam("$filter") == "(c gt 5)";
    }, CannedResponse::Json(MakeV4Page(context, {R"({"c":6})", R"({"c":7})"})));
    server.OnPath("/top/Items", CannedResponse::Json(MakeV4Page(context, {R"({"c":1})", R"({"c":6})", R"({"c":7})"})));
    TestDatabase db;
    auto &con = db.Con();
    REQUIRE_FALSE(con.Query(ErplOdataLoadSql())->HasError());
    auto result = con.Query("SELECT c FROM odata_read('" + server.Url("/top/Items") +
                            "') WHERE c > 5 ORDER BY c LIMIT 2");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(duckdb_compat::ResultValue(*result, 0, 0).GetValue<int32_t>() == 6);
    REQUIRE(duckdb_compat::ResultValue(*result, 0, 1).GetValue<int32_t>() == 7);
    const auto requests = server.RequestsFor("/top/Items");
    REQUIRE_FALSE(requests.empty());
    for (const auto &request : requests) {
        const auto filter = request.DecodedQueryParam("$filter");
        std::cout << "top filter wire: " << filter << std::endl;
    }
    REQUIRE(requests.back().DecodedQueryParam("$filter") == "(c gt 5)");
}
