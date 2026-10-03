// A service response chooses the metadata URL, through @odata.context. DoMetadataHttpGet
// attached the caller's Authorization header to whatever host that named, with no origin
// check - so a compromised or hostile service could steer a bearer token or basic
// credential to a host of its choosing simply by answering with an absolute context URL.
//
// The generic request path already guards this (see the comment at odata_client.hpp:250,
// added for the @odata.nextLink case); the metadata path was missed.

#include "catch.hpp"
#include "duckdb.hpp"

#include "odata_client.hpp"
#include "odata_test_server.hpp"

#include <string>
#include "test_database.hpp"

using erpl_web::test_support::TestDatabase;

using erpl_web::test_support::CannedResponse;
using erpl_web::test_support::ODataTestServer;
using erpl_web::HttpAuthParams;
using erpl_web::HttpClient;
using erpl_web::HttpParams;
using erpl_web::HttpUrl;
using erpl_web::ODataEntitySetClient;
using erpl_web::ODataEntitySetResponse;
using erpl_web::ODataVersion;
using erpl_web::HttpMethod;
using erpl_web::HttpResponse;


TEST_CASE("credentials are not sent to a host named by @odata.context",
          "[odata_metadata][security]") {
    ODataTestServer service;    // the host the caller pointed at, and trusts
    ODataTestServer elsewhere;  // the host the service names in its context URL

    service.ServeMetadataFixture("/svc/$metadata", "edm_northwind.xml");
    elsewhere.ServeMetadataFixture("/evil/$metadata", "edm_northwind.xml");

    // The client is opened against `service`, with credentials for it.
    HttpParams params;
    auto http_client = std::make_shared<HttpClient>(params);
    auto auth = std::make_shared<HttpAuthParams>();
    auth->basic_credentials = std::make_pair(std::string("user"), std::string("secret-password"));

    auto client = std::make_shared<ODataEntitySetClient>(
        http_client, HttpUrl(service.Url("/svc/Products")), auth);
    client->SetODataVersionDirectly(ODataVersion::V4);

    // The service answers with a context URL pointing at the OTHER host. This is the input
    // the attacker controls: the response body of the service you are reading.
    const std::string foreign_context = elsewhere.Url("/evil/$metadata") + "#Products";
    const std::string body = std::string(R"({"@odata.context":")") + foreign_context +
                             R"(","value":[{"ProductID":1,"ProductName":"Chai"}]})";
    client->AdoptResponse(std::make_shared<ODataEntitySetResponse>(
        std::make_unique<HttpResponse>(HttpMethod::GET, HttpUrl(service.Url("/svc/Products")), 200,
                                       "application/json", body),
        ODataVersion::V4));

    try {
        client->GetMetadata();
    } catch (const std::exception &) {
        // Refusing outright is an acceptable outcome; sending the credential is not.
    }

    bool foreign_was_contacted = false;
    for (const auto &request : elsewhere.Requests()) {
        foreign_was_contacted = true;
        INFO("foreign host received " << request.method << " " << request.target);
        REQUIRE_FALSE(request.HasHeader("Authorization"));
    }

    // The test is only meaningful if the foreign host was actually reached; otherwise it
    // would pass for the wrong reason.
    INFO("the context URL must have been followed for this test to mean anything");
    CHECK(foreign_was_contacted);
}
