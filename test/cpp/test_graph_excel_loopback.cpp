#include "catch.hpp"
#include "duckdb_compat.hpp"
#include "graph_client.hpp"
#include "odata_test_server.hpp"
#include "test_database.hpp"
#include "test_extension_load_sql.hpp"
#include "yyjson.hpp"

using namespace erpl_web;
using namespace erpl_web::test_support;

namespace {
struct ScopedGraphLoopback {
    explicit ScopedGraphLoopback(const ODataTestServer &server) {
        GraphClient::SetLoopbackBaseUrlForTest(server.Url("/v1.0"));
    }
    ~ScopedGraphLoopback() { GraphClient::SetLoopbackBaseUrlForTest(""); }
};
void InstallTestSecret(duckdb::Connection &con) {
    auto result = con.Query("CREATE SECRET graph_loopback (TYPE microsoft_graph, PROVIDER config, "
                            "tenant_id 'test-tenant', client_id 'test-client', client_secret 'test-secret', access_token 'test-token', expires_at '4102444800')");
    REQUIRE_FALSE(result->HasError());
}
const std::string workbook_path = "/v1.0/me/drive/root:/round5-test.xlsx:/workbook";
}

TEST_CASE("Graph loopback seam rejects malformed or credentialed URLs", "[graph][loopback]") {
    REQUIRE_THROWS(GraphClient::SetLoopbackBaseUrlForTest("http://127.0.0.1:1234.evil/v1.0"));
    REQUIRE_THROWS(GraphClient::SetLoopbackBaseUrlForTest("http://user@127.0.0.1:1234/v1.0"));
    REQUIRE_THROWS(GraphClient::SetLoopbackBaseUrlForTest("http://127.0.0.1:0/v1.0"));
    REQUIRE_THROWS(GraphClient::SetLoopbackBaseUrlForTest("http://127.0.0.1:1234/v1.0?next=evil"));
    REQUIRE(GraphClient::BaseUrl() == "https://graph.microsoft.com/v1.0");
}

TEST_CASE("Excel DELETE is refused before a Graph write", "[graph][loopback]") {
    ODataTestServer server;
    ScopedGraphLoopback base(server);
    server.OnPath(workbook_path + "/tables",
                  CannedResponse::Json(R"({"value":[{"name":"SalesTable"}]})"));
    server.OnPath(workbook_path + "/tables/SalesTable/columns",
                  CannedResponse::Json(R"({"value":[{"name":"Name","index":0}]})"));
    TestDatabase db;
    auto &con = db.Con();
    REQUIRE_FALSE(con.Query(ErplOdataLoadSql())->HasError());
    InstallTestSecret(con);
    auto attached = con.Query("ATTACH 'round5-test.xlsx' AS wb "
                              "(TYPE excel_workbook, SECRET 'graph_loopback')");
    INFO((attached->HasError() ? attached->GetError() : std::string()));
    REQUIRE_FALSE(attached->HasError());
    auto deleted = con.Query("DELETE FROM wb.SalesTable WHERE Name = 'one'");
    REQUIRE(deleted->HasError());
    INFO(deleted->GetError());
    REQUIRE(deleted->GetError().find("DELETE is not supported on Excel workbook catalogs") != std::string::npos);
    for (const auto &request : server.Requests()) {
        REQUIRE(request.method != "POST");
        REQUIRE(request.method != "PATCH");
        REQUIRE(request.method != "DELETE");
    }
}

TEST_CASE("Excel COPY sends every row from multiple chunks", "[graph][loopback]") {
    ODataTestServer server;
    ScopedGraphLoopback base(server);
    server.OnPath(workbook_path + "/createSession",
                  CannedResponse::Json(R"({"id":"session-1"})"));
    server.OnPath(workbook_path + "/tables/SalesTable/rows/add",
                  CannedResponse::Json(R"({"value":[]})"));
    server.OnPath(workbook_path + "/closeSession",
                  CannedResponse::Json(R"({})"));
    TestDatabase db;
    auto &con = db.Con();
    REQUIRE_FALSE(con.Query(ErplOdataLoadSql())->HasError());
    InstallTestSecret(con);
    auto copied = con.Query("COPY (SELECT i::VARCHAR AS Name FROM range(2300) t(i)) "
                            "TO 'round5-test.xlsx' (FORMAT graph_excel_table, "
                            "table 'SalesTable', secret 'graph_loopback')");
    INFO((copied->HasError() ? copied->GetError() : std::string()));
    REQUIRE_FALSE(copied->HasError());
    size_t posted = 0;
    size_t rows = 0;
    for (const auto &request : server.RequestsFor(workbook_path + "/tables/SalesTable/rows/add")) {
        REQUIRE(request.method == "POST");
        posted++;
        auto *doc = duckdb_yyjson::yyjson_read(request.body.c_str(), request.body.size(), 0);
        REQUIRE(doc != nullptr);
        auto *array = duckdb_yyjson::yyjson_obj_get(duckdb_yyjson::yyjson_doc_get_root(doc), "values");
        REQUIRE(array != nullptr);
        rows += duckdb_yyjson::yyjson_arr_size(array);
        duckdb_yyjson::yyjson_doc_free(doc);
    }
    REQUIRE(posted == 1);
    REQUIRE(rows == 2300);
}
