// Coverage for $expand handling in the odata_read() path.
//
// History (GitHub #115): this file was never listed in CMakeLists.txt, so it was
// never compiled and rotted twice over.
//
//   1. Every case built an ODataEntitySetClient against the fake host
//      "http://host/service". Metadata used to be fetched lazily; it is fetched
//      eagerly during bind now, so every case died with
//      "Could not establish connection ... /$metadata".
//   2. More importantly, most cases asserted a behaviour that does not exist:
//      they called ODataReadBindData::SetExpandClause() and then expected
//      PredicatePushdownHelper()->ExpandClause() to return it. SetExpandClause()
//      only stores the string on the bind data; the helper learns about the
//      expand from ODataReadBindHelpers::ProcessExpandClause(), which is what
//      the bind actually calls. Those assertions could never have passed.
//
// The intent -- "an expand clause reaches the request, expanded data is parsed
// into the result, nested and multi-level expands work" -- is preserved by
// driving odata_read() against the local ODataTestServer and asserting on the
// request the extension really put on the wire. That is the assertion that
// catches a regression: an expand that silently never leaves the client still
// produces a plausible-looking (but wrong) result set.
//
// Clause-construction details of ODataPredicatePushdownHelper in isolation
// (encoding, option splitting, clause ordering) live in
// test_odata_predicate_pushdown_expand.cpp; this file is about the read path.

#include "catch.hpp"

#include "odata_url_helpers.hpp"
#include "duckdb.hpp"

#include "odata_client.hpp"
#include "odata_read_functions.hpp"
#include "odata_test_server.hpp"
#include "datazoo/oauth2/http_client.hpp"

#include <memory>
#include <string>
#include <vector>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::MakeV2Page;
using erpl_web::test_support::MakeV4Page;
using erpl_web::test_support::ODataTestServer;
using erpl_web::test_support::RecordedRequest;

namespace {


// Decoded value of the $expand the extension sent for `path`, or an empty string
// when it never sent one.
//
// Comparing the decoded value is deliberate. The raw bytes on the socket are not
// the extension's own encoding: httplib's client re-encodes every query value as
// application/x-www-form-urlencoded on its way out (see FormDecode() in
// odata_test_server.cpp), so the %20 the OData URL codec wrote arrives as '+' and
// a nested '=' arrives as %3D. Pinning a test to that spelling would pin it to a
// third-party detail that a DuckDB bump can change. Decoding undoes exactly that
// transport encoding and nothing else, so equality here still means "the service
// receives the clause the caller asked for, byte for byte".
std::string ExpandOnTheWire(const ODataTestServer &server, const std::string &path)
{
    for (const auto &request : server.RequestsFor(path)) {
        if (request.HasQueryParam("$expand")) {
            return request.DecodedQueryParam("$expand");
        }
    }
    return std::string();
}

std::string QueryParamOnTheWire(const ODataTestServer &server, const std::string &path,
                                const std::string &name)
{
    for (const auto &request : server.RequestsFor(path)) {
        if (request.HasQueryParam(name)) {
            return request.DecodedQueryParam(name);
        }
    }
    return std::string();
}

// Doubles embedded single quotes so an expand clause containing a string literal
// -- "Category($filter=Country eq 'UK')" -- can be inlined into a SQL statement.
std::string SqlLiteral(const std::string &value)
{
    std::string escaped = "'";
    for (const char character : value) {
        if (character == '\'') {
            escaped += '\'';
        }
        escaped += character;
    }
    escaped += "'";
    return escaped;
}

bool HasColumn(const duckdb::MaterializedQueryResult &result, const std::string &name)
{
    for (const auto &column_name : result.names) {
        if (column_name == name) {
            return true;
        }
    }
    return false;
}

// A Northwind v4 service whose Products entity set carries the navigation
// properties the expand cases below name (Category, Supplier, Order_Details).
// Every case that only cares about what went onto the wire shares this shape.
class NorthwindV4Service {
public:
    explicit NorthwindV4Service(std::vector<std::string> rows = {std::string(PRODUCT_CHAI)})
    {
        server.ServeMetadataFixture("/nw/$metadata", "edm_northwind.xml");
        server.OnPath("/nw/Products",
                      CannedResponse::Json(MakeV4Page(server.Url("/nw/$metadata") + "#Products",
                                                      rows)));
    }

    std::string ProductsUrl() const { return server.Url("/nw/Products"); }
    std::string SentExpand() const { return ExpandOnTheWire(server, "/nw/Products"); }
    // The bytes as they actually went out, before any decoding - which is where a raw
    // space would be visible, and a decoded value never is.
    std::string SentExpandRaw() const
    {
        for (const auto &request : server.RequestsFor("/nw/Products")) {
            if (request.HasQueryParam("$expand")) {
                return request.QueryParam("$expand");
            }
        }
        return std::string();
    }
    std::string SentParam(const std::string &name) const
    {
        return QueryParamOnTheWire(server, "/nw/Products", name);
    }

    // Runs `SELECT * FROM odata_read(<products>, expand = '<expand>')` and returns
    // the result so the caller can assert on both the rows and the request.
    duckdb::unique_ptr<duckdb::MaterializedQueryResult> ReadWithExpand(const std::string &expand)
    {
        return Con().Query("SELECT * FROM odata_read('" + ProductsUrl() + "', expand = " +
                           SqlLiteral(expand) + ")");
    }

    duckdb::Connection &Con() { return database.Con(); }

    static constexpr const char *PRODUCT_CHAI =
        R"({"ProductID":1,"ProductName":"Chai","SupplierID":1,"CategoryID":1,)"
        R"("QuantityPerUnit":"10 boxes x 20 bags","UnitPrice":18.0,"UnitsInStock":39,)"
        R"("UnitsOnOrder":0,"ReorderLevel":10,"Discontinued":false})";

private:
    // The server must outlive the database so it is still accepting when the
    // connections that talk to it are torn down.
    ODataTestServer server;
    TestDatabase database;
};

// Asserts that `expand` survives the whole bind path intact and reaches the service.
// Every "is this spelling of $expand mangled on the way out?" case below is exactly this
// assertion with a different string.
//
// "Intact" means semantically, not byte for byte. These clauses carry values containing
// spaces and quotes - Supplier($filter=Country eq 'UK') - and a raw space in a request
// line ends the target, so the value has to reach the wire percent-encoded. What must
// survive is the STRUCTURE (the parentheses, commas and semicolons that separate nested
// options) and the value itself once decoded.
//
// This used to assert byte equality with the input, which passed only because the
// encoding applied by normalizeAndSanitizeExpand was silently undone: query values were
// decoded when the query was parsed and re-emitted raw, so the clause went out with
// literal spaces in it. That was GitHub #227, and asserting byte equality made the
// mangling look like the contract.
void RequireExpandReachesTheService(const std::string &expand)
{
    NorthwindV4Service service;
    REQUIRE_FALSE(service.Con().Query("LOAD erpl_odata")->HasError());

    auto result = service.ReadWithExpand(expand);
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    const std::string decoded = service.SentExpand();
    const std::string raw = service.SentExpandRaw();
    INFO("raw on the wire: " << raw);
    INFO("decoded:         " << decoded);

    // Guard against a vacuous pass: an empty raw value would satisfy the space check
    // below without anything having been sent at all.
    REQUIRE_FALSE(raw.empty());

    // What the service ends up reading must be exactly what the caller wrote.
    REQUIRE(decoded == expand);

    // When the clause contains a space, the wire form must actually differ from it -
    // otherwise the encoding is not happening and the check below proves nothing.
    if (expand.find(' ') != std::string::npos) {
        REQUIRE(raw != expand);
        REQUIRE(raw.find("%20") != std::string::npos);
    }

    // And the bytes that carried it must not contain anything that ends the request
    // target. Asserting this on the DECODED value would be vacuous - a decoded value
    // has its spaces back by definition - so it has to be checked on the raw form.
    REQUIRE(raw.find(' ') == std::string::npos);
}

}  // namespace

// ----------------------------------------------------------------------
// Plain unit tests -- no server needed.

// ODataReadBindData::SetExpandClause/GetExpandClause is a pure accessor pair; it
// touches neither the network nor the pushdown helper, so spinning up a server
// here would only make the test slower and less clear about what it covers.
TEST_CASE("ODataReadBindData stores the expand clause verbatim", "[odata_expand][unit]") {
    auto http_client = std::make_shared<erpl_web::HttpClient>();
    erpl_web::HttpUrl url("http://127.0.0.1:1/service/Customers");
    auto auth_params = std::make_shared<erpl_web::HttpAuthParams>();
    auto odata_client =
        std::make_shared<erpl_web::ODataEntitySetClient>(http_client, url, auth_params);

    erpl_web::ODataReadBindData bind_data(odata_client);

    SECTION("A plain multi-property clause round-trips") {
        bind_data.SetExpandClause("Category,Orders");
        REQUIRE(bind_data.GetExpandClause() == "Category,Orders");
    }

    SECTION("An empty clause clears a previously set one") {
        bind_data.SetExpandClause("Category,Orders");
        bind_data.SetExpandClause("");
        REQUIRE(bind_data.GetExpandClause().empty());
    }

    SECTION("Nested options are stored byte for byte") {
        // No normalisation happens here -- the clause is handed on unchanged and
        // only sanitised when the pushdown helper builds the URL.
        const std::string clause =
            "Products($filter=DiscontinuedDate eq null),Category($select=Name)";
        bind_data.SetExpandClause(clause);
        REQUIRE(bind_data.GetExpandClause() == clause);
    }

    SECTION("The last clause set wins") {
        for (int i = 0; i < 100; ++i) {
            bind_data.SetExpandClause("Path" + std::to_string(i));
        }
        REQUIRE(bind_data.GetExpandClause() == "Path99");
    }
}

// ----------------------------------------------------------------------
// Expanded columns in the bound schema.

// Catches: expanded navigation properties silently disappearing from the bound
// schema, which would turn "expand=X" into a no-op the user cannot see.
TEST_CASE("ODataReadBindData appends expanded navigation properties to the schema",
          "[odata_expand][schema]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/nw/$metadata", "edm_northwind.xml");
    server.OnPath("/nw/Products",
                  CannedResponse::Json(MakeV4Page(server.Url("/nw/$metadata") + "#Products",
                                                  {NorthwindV4Service::PRODUCT_CHAI})));

    auto http_client = std::make_shared<erpl_web::HttpClient>();
    erpl_web::HttpUrl url(server.Url("/nw/Products"));
    auto auth_params = std::make_shared<erpl_web::HttpAuthParams>();
    auto odata_client =
        std::make_shared<erpl_web::ODataEntitySetClient>(http_client, url, auth_params);

    erpl_web::ODataReadBindData bind_data(odata_client);
    REQUIRE_FALSE(bind_data.HasExpandedData());

    const auto names_without_expand = bind_data.GetResultNames();
    REQUIRE_FALSE(names_without_expand.empty());

    bind_data.SetExpandClause("Category,Supplier");
    bind_data.SetExpandedDataSchema({"Category", "Supplier"});

    REQUIRE(bind_data.HasExpandedData());

    const auto names_with_expand = bind_data.GetResultNames();
    const auto types_with_expand = bind_data.GetResultTypes();
    // The expanded columns are appended, the EDM properties are untouched, and
    // names and types stay in lockstep -- DuckDB rejects the bind otherwise.
    REQUIRE(names_with_expand.size() == names_without_expand.size() + 2);
    REQUIRE(names_with_expand.size() == types_with_expand.size());
    REQUIRE(names_with_expand[names_with_expand.size() - 2] == "Category");
    REQUIRE(names_with_expand.back() == "Supplier");
    REQUIRE(names_with_expand.front() == names_without_expand.front());

    // The metadata really was read from the service -- the schema is not guessed.
    REQUIRE(server.RequestsFor("/nw/$metadata").size() >= 1);
}

// ----------------------------------------------------------------------
// The expand named parameter on the wire.

// Catches: an expand named parameter that is accepted but never sent. The scan
// still returns rows, so only the recorded request can tell the difference.
TEST_CASE("odata_read sends the expand named parameter as $expand", "[odata_expand][e2e]") {
    NorthwindV4Service service;
    REQUIRE_FALSE(service.Con().Query("LOAD erpl_odata")->HasError());

    auto result = service.ReadWithExpand("Category");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);

    REQUIRE(service.SentExpand() == "Category");
    // The expanded navigation property surfaces as a column of the result.
    REQUIRE(HasColumn(*result, "Category"));
    REQUIRE(HasColumn(*result, "ProductName"));
}

// Catches: expanded payload that reaches the client but is dropped during
// parsing, leaving the user with an all-NULL column.
TEST_CASE("odata_read parses inline expanded data into the result", "[odata_expand][e2e]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/nw/$metadata", "edm_northwind.xml");
    server.OnPath(
        "/nw/Products",
        CannedResponse::Json(MakeV4Page(
            server.Url("/nw/$metadata") + "#Products",
            {R"({"ProductID":1,"ProductName":"Chai","SupplierID":1,"CategoryID":1,)"
             R"("QuantityPerUnit":"10 boxes x 20 bags","UnitPrice":18.0,"UnitsInStock":39,)"
             R"("UnitsOnOrder":0,"ReorderLevel":10,"Discontinued":false,)"
             R"("Category":{"CategoryID":1,"CategoryName":"Beverages",)"
             R"("Description":"Soft drinks, coffees, teas"}})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // Cast to VARCHAR so the assertion is about the expanded VALUES and not about
    // whether the EDM type resolver produced a STRUCT or the LIST(VARCHAR)
    // fallback for the navigation property.
    auto result = con.Query("SELECT ProductName, CAST(Category AS VARCHAR) AS category_text "
                            "FROM odata_read('" +
                            server.Url("/nw/Products") + "', expand = 'Category')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    REQUIRE(result->GetValue(0, 0).GetValue<std::string>() == "Chai");

    const auto category_text = result->GetValue(1, 0).GetValue<std::string>();
    INFO(category_text);
    REQUIRE(category_text.find("Beverages") != std::string::npos);
    REQUIRE(category_text.find("Soft drinks") != std::string::npos);

    REQUIRE(ExpandOnTheWire(server, "/nw/Products") == "Category");
}

// GitHub #156: $expand written into the URL must behave exactly like the expand= named
// parameter. It did not. FromEntitySetClient buffers the bind-time probe page BEFORE
// ProcessExpandClause has set the expand schema, so page one is buffered with no expanded
// data extracted. The named-parameter spelling hides this: it ADDS $expand to the URL, so
// the URL changes, the buffer is discarded and page one is refetched once the schema is
// known. With $expand already in the URL nothing changes, the un-extracted buffer survives,
// and the expanded column comes back NULL for the whole scan.
TEST_CASE("odata_read expands identically whether $expand is in the URL or a parameter",
          "[odata_expand][e2e]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/nw/$metadata", "edm_northwind.xml");
    server.OnPath(
        "/nw/Products",
        CannedResponse::Json(MakeV4Page(
            server.Url("/nw/$metadata") + "#Products",
            {R"({"ProductID":1,"ProductName":"Chai","SupplierID":1,"CategoryID":1,)"
             R"("QuantityPerUnit":"10 boxes x 20 bags","UnitPrice":18.0,"UnitsInStock":39,)"
             R"("UnitsOnOrder":0,"ReorderLevel":10,"Discontinued":false,)"
             R"("Category":{"CategoryID":1,"CategoryName":"Beverages",)"
             R"("Description":"Soft drinks, coffees, teas"}})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto by_parameter = con.Query("SELECT CAST(Category AS VARCHAR) FROM odata_read('" +
                                  server.Url("/nw/Products") + "', expand = 'Category')");
    INFO((by_parameter->HasError() ? by_parameter->GetError() : std::string()));
    REQUIRE_FALSE(by_parameter->HasError());
    REQUIRE(by_parameter->RowCount() == 1);

    auto by_url = con.Query("SELECT CAST(Category AS VARCHAR) FROM odata_read('" +
                            server.Url("/nw/Products") + "?$expand=Category')");
    INFO((by_url->HasError() ? by_url->GetError() : std::string()));
    REQUIRE_FALSE(by_url->HasError());
    REQUIRE(by_url->RowCount() == 1);

    const auto parameter_text = by_parameter->GetValue(0, 0).ToString();
    const auto url_text = by_url->GetValue(0, 0).ToString();
    INFO("by parameter: " << parameter_text);
    INFO("by url:       " << url_text);
    REQUIRE(url_text.find("Beverages") != std::string::npos);
    REQUIRE(url_text == parameter_text);
}

// A filter that the pushdown layer cannot resolve to a column must not take the database
// down with it. #153 replaced a silent `continue` with `throw InternalException`, and
// DuckDB treats ExceptionType::INTERNAL as fatal: client_context.cpp calls
// ValidChecker::Invalidate(db_inst), after which EVERY later statement on that instance
// fails. The column resolver only searches the base result names, so a filter on an
// expanded column resolves to "" and takes exactly that path.
//
// Silently dropping the filter was wrong (it returned rows that did not match). Killing
// the instance is a different and worse kind of wrong.
TEST_CASE("a filter on an expanded column does not invalidate the database",
          "[odata_expand][e2e]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/nw/$metadata", "edm_northwind.xml");
    server.OnPath(
        "/nw/Products",
        CannedResponse::Json(MakeV4Page(
            server.Url("/nw/$metadata") + "#Products",
            {R"({"ProductID":1,"ProductName":"Chai","SupplierID":1,"CategoryID":1,)"
             R"("QuantityPerUnit":"10 boxes x 20 bags","UnitPrice":18.0,"UnitsInStock":39,)"
             R"("UnitsOnOrder":0,"ReorderLevel":10,"Discontinued":false,)"
             R"("Category":{"CategoryID":1,"CategoryName":"Beverages",)"
             R"("Description":"Soft drinks, coffees, teas"}})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    // Whether this particular filter is pushed, translated or errors is not the point.
    auto result = con.Query("SELECT ProductName FROM odata_read('" + server.Url("/nw/Products") +
                            "', expand = 'Category') WHERE Category IS NOT NULL");
    (void)result;

    // The point: the connection must still be usable afterwards.
    auto after = con.Query("SELECT 1");
    INFO((after->HasError() ? after->GetError() : std::string()));
    REQUIRE_FALSE(after->HasError());
    REQUIRE(after->GetValue(0, 0).GetValue<int32_t>() == 1);
}

// Catches: an expand that is dropped as soon as another query option is present.
// $top/$skip and $expand are assembled from the same helper, so a clause that
// overwrites rather than adds is a plausible regression.
TEST_CASE("odata_read sends $expand alongside $top and $skip", "[odata_expand][e2e]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/nw/$metadata", "edm_northwind.xml");
    server.OnPath("/nw/Products",
                  CannedResponse::Json(MakeV4Page(server.Url("/nw/$metadata") + "#Products",
                                                  {NorthwindV4Service::PRODUCT_CHAI})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM odata_read('" + server.Url("/nw/Products") +
                            "', expand = 'Category,Supplier', top = 10, skip = 20)");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(ExpandOnTheWire(server, "/nw/Products") == "Category,Supplier");
    REQUIRE(QueryParamOnTheWire(server, "/nw/Products", "$top") == "10");
    REQUIRE(QueryParamOnTheWire(server, "/nw/Products", "$skip") == "20");
}

// ----------------------------------------------------------------------
// Nested and multi-level expands.

// Catches: a nested expand collapsed to its first segment. Sending
// "$expand=Trips" where "$expand=Trips($expand=PlanItems)" was asked for returns
// rows that look fine but are missing a whole level of the graph.
TEST_CASE("odata_read sends a multi-level $expand unchanged", "[odata_expand][e2e][nested]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/tp/$metadata", "edm_trippin.xml");
    server.OnPath("/tp/People",
                  CannedResponse::Json(MakeV4Page(
                      server.Url("/tp/$metadata") + "#People",
                      {R"({"UserName":"russellwhyte","FirstName":"Russell","LastName":"Whyte",)"
                       R"("Trips":[{"TripId":1001,"Name":"Trip in US",)"
                       R"("PlanItems":[{"PlanItemId":11,"ConfirmationCode":"JH58494"}]}]})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT UserName, CAST(Trips AS VARCHAR) AS trips_text "
                            "FROM odata_read('" +
                            server.Url("/tp/People") + "', expand = 'Trips($expand=PlanItems)')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    REQUIRE(result->GetValue(0, 0).GetValue<std::string>() == "russellwhyte");

    // The nested level survives into the parsed value, not just onto the wire.
    const auto trips_text = result->GetValue(1, 0).GetValue<std::string>();
    INFO(trips_text);
    REQUIRE(trips_text.find("Trip in US") != std::string::npos);

    REQUIRE(ExpandOnTheWire(server, "/tp/People") == "Trips($expand=PlanItems)");
}

// Catches: a slash-separated expand path being split or reordered. SAP Gateway
// services use this spelling, so mangling it breaks them specifically.
TEST_CASE("odata_read sends a slash-separated expand path unchanged",
          "[odata_expand][e2e][nested]") {
    RequireExpandReachesTheService("Category/Products/Supplier");
}

// Catches: only the first of several expand paths reaching the service.
TEST_CASE("odata_read sends every expand path with its own options",
          "[odata_expand][e2e][nested]") {
    RequireExpandReachesTheService(
        "Category($select=CategoryName),Supplier($filter=Country eq 'UK'),"
        "Order_Details($top=10)");
}

// ----------------------------------------------------------------------
// OData version handling.
//
// $expand is spelled the same in v2 and v4, but the pushdown helper is version
// aware and assembles the rest of the query differently, so both need covering.

// Catches: $expand being suppressed on v2 services, where the helper takes a
// different branch for $inlinecount and friends.
TEST_CASE("odata_read sends $expand against an OData v2 service", "[odata_expand][e2e][v2]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/v2/$metadata", "edm_northwind_v2.xml");
    server.OnPath("/v2/Customers",
                  CannedResponse::Json(MakeV2Page(
                      {R"({"CustomerID":"ALFKI","CompanyName":"Alfreds Futterkiste"})"})));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM odata_read('" + server.Url("/v2/Customers") +
                            "', expand = 'Orders')");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    REQUIRE(HasColumn(*result, "Orders"));

    REQUIRE(ExpandOnTheWire(server, "/v2/Customers") == "Orders");
}

// Catches: $expand and the v2 spelling of inline count knocking each other out.
// v2 uses "$inlinecount=allpages" where v4 uses "$count=true"; both live next to
// $expand in the same query string.
TEST_CASE("odata_read sends $expand next to the v2 $inlinecount", "[odata_expand][e2e][v2]") {
    ODataTestServer server;
    server.ServeMetadataFixture("/v2/$metadata", "edm_northwind_v2.xml");
    server.OnPath("/v2/Customers",
                  CannedResponse::Json(
                      R"({"d":{"__count":"1","results":[)"
                      R"({"CustomerID":"ALFKI","CompanyName":"Alfreds Futterkiste"}]}})"));

    TestDatabase database;
    duckdb::Connection &con = database.Con();
    REQUIRE_FALSE(con.Query("LOAD erpl_odata")->HasError());

    auto result = con.Query("SELECT * FROM odata_read('" + server.Url("/v2/Customers") +
                            "', expand = 'Orders', count = true)");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(ExpandOnTheWire(server, "/v2/Customers") == "Orders");
    REQUIRE(QueryParamOnTheWire(server, "/v2/Customers", "$inlinecount") == "allpages");
}

// ----------------------------------------------------------------------
// Spellings that must survive the trip to the service.
//
// Each of these used to be an "edge case" asserted against a clause the bind path
// never actually produced. Asserting them on the wire is what proves the option
// splitter and the URL encoder hand the service back exactly what was asked for.

TEST_CASE("odata_read sends nested $expand options unchanged", "[odata_expand][e2e][edge]") {
    RequireExpandReachesTheService(
        "Category($filter=CategoryName eq 'Beverages';$select=CategoryName;$top=5;$skip=10)");
}

TEST_CASE("odata_read does not mistake a semicolon inside a string literal for an option "
          "separator",
          "[odata_expand][e2e][edge]") {
    RequireExpandReachesTheService("Category($filter=CategoryName eq 'Soft;Drinks')");
}

TEST_CASE("odata_read sends a nested filter with parentheses unchanged",
          "[odata_expand][e2e][edge]") {
    RequireExpandReachesTheService("Category($filter=(CategoryID gt 1) and (CategoryID lt 5))");
}

TEST_CASE("odata_read sends an empty option list unchanged", "[odata_expand][e2e][edge]") {
    RequireExpandReachesTheService("Category()");
}

TEST_CASE("odata_read sends a comma inside a function call unchanged",
          "[odata_expand][e2e][edge]") {
    RequireExpandReachesTheService("Category($filter=startswith(CategoryName,'B') eq true)");
}

// Catches: a malformed clause crashing the bind or being silently swallowed.
// The sanitizer normalizes an unbalanced option group by closing it, rather than
// forwarding the malformed text or dropping the clause. That is deliberate and is
// asserted here so the behaviour cannot change unnoticed: the clause still reaches
// the service, and it reaches it as something the service can parse.
TEST_CASE("odata_read closes an unbalanced expand option group rather than dropping the clause",
          "[odata_expand][e2e][edge]") {
    NorthwindV4Service service;
    REQUIRE_FALSE(service.Con().Query("LOAD erpl_odata")->HasError());

    auto result = service.ReadWithExpand("Category($filter=CategoryID gt 1");
    INFO((result->HasError() ? result->GetError() : std::string()));
    REQUIRE_FALSE(result->HasError());

    REQUIRE(service.SentExpand() == "Category($filter=CategoryID gt 1)");
}

// ----------------------------------------------------------------------
// Robustness.

// Catches: truncation of a long expand clause, either by a fixed-size buffer or
// by an over-eager URL length guard.
TEST_CASE("odata_read sends a long list of expand paths in full", "[odata_expand][e2e][edge]") {
    std::string expand = "Path1";
    for (int i = 2; i <= 10; ++i) {
        expand += ",Path" + std::to_string(i);
    }
    RequireExpandReachesTheService(expand);
}

TEST_CASE("odata_read sends a very long nested filter in full", "[odata_expand][e2e][edge]") {
    const std::string long_property(1000, 'a');
    RequireExpandReachesTheService("Category($filter=" + long_property + " eq 'test')");
}

// ----------------------------------------------------------------------
// Real-world shapes taken from the services this extension is used against.

TEST_CASE("odata_read sends the SAP Gateway catalog expand shape unchanged",
          "[odata_expand][e2e][realworld]") {
    RequireExpandReachesTheService("Category($expand=Products())");
}

TEST_CASE("odata_read sends the Northwind orders expand shape unchanged",
          "[odata_expand][e2e][realworld]") {
    RequireExpandReachesTheService(
        "Order_Details($filter=Quantity gt 10;$select=ProductID,Quantity)");
}

TEST_CASE("odata_read sends a multi-level business expand shape unchanged",
          "[odata_expand][e2e][realworld]") {
    RequireExpandReachesTheService(
        "Supplier($select=SupplierID,CompanyName)/Products($filter=Discontinued eq false;$top=10)"
        "/Order_Details($select=ProductID,Quantity,UnitPrice)");
}
