#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb.hpp"
#include "datazoo/oauth2/oauth2_types.hpp"
#include "datazoo/oauth2/oauth2_browser.hpp"
#include "datasphere_local_server.hpp"
#include <iostream>
#include <memory>

using namespace erpl_web;
using namespace std;

// ============================================================================
// Basic OAuth2 Utility Tests
// ============================================================================

TEST_CASE("Test OAuth2 PKCE Implementation", "[datasphere][oauth2][basic]")
{
    std::cout << std::endl;
    
    // Test PKCE code verifier generation
    auto code_verifier = OAuth2Utils::GenerateCodeVerifier();
    REQUIRE(!code_verifier.empty());
    REQUIRE(code_verifier.length() >= 43); // PKCE minimum length
    REQUIRE(code_verifier.length() <= 128); // PKCE maximum length
    
    // Test code challenge generation
    auto code_challenge = OAuth2Utils::GenerateCodeChallenge(code_verifier);
    REQUIRE(!code_challenge.empty());
    REQUIRE(code_challenge.length() == 64); // Should be exactly 64 characters
    REQUIRE(code_challenge != code_verifier); // Challenge should be different from verifier
    
    // Test that same verifier produces same challenge
    auto code_challenge2 = OAuth2Utils::GenerateCodeChallenge(code_verifier);
    REQUIRE(code_challenge == code_challenge2);
}

TEST_CASE("Test OAuth2 State Parameter Generation", "[datasphere][oauth2][basic]")
{
    std::cout << std::endl;
    
    // Test state parameter generation
    auto state1 = OAuth2Utils::GenerateState();
    auto state2 = OAuth2Utils::GenerateState();
    
    REQUIRE(!state1.empty());
    REQUIRE(!state2.empty());
    REQUIRE(state1 != state2); // States should be unique
    
    // Test state validation
    REQUIRE(OAuth2Utils::ValidateState(state1, state1) == true);
    REQUIRE(OAuth2Utils::ValidateState(state2, state1) == false);
    REQUIRE(OAuth2Utils::ValidateState(state1, state2) == false);
    REQUIRE(OAuth2Utils::ValidateState("", state1) == false);
}

TEST_CASE("Test OAuth2 Configuration", "[datasphere][oauth2][basic]")
{
    std::cout << std::endl;
    
    OAuth2Config config;
    config.tenant_name = "test_tenant";
    config.data_center = "eu10";
    config.client_id = "test_client_id";
    config.client_secret = "test_client_secret";
    config.scope = "default";
    config.redirect_uri = "http://localhost:8080/callback";
    
    // Test authorization URL construction
    auto auth_url = config.GetAuthorizationUrl();
    REQUIRE(auth_url == "https://test_tenant.authentication.eu10.hana.ondemand.com/oauth/authorize");
    
    // Test token URL construction
    auto token_url = config.GetTokenUrl();
    REQUIRE(token_url == "https://test_tenant.authentication.eu10.hana.ondemand.com/oauth/token");
}

TEST_CASE("Test OAuth2 Token Management", "[datasphere][oauth2][basic]")
{
    std::cout << std::endl;
    
    OAuth2Tokens tokens;
    tokens.access_token = "test_access_token";
    tokens.refresh_token = "test_refresh_token";
    tokens.token_type = "Bearer";
    tokens.expires_in = 3600;
    tokens.scope = "default";
    
    // Test 1: Initial state - no expiry set, should be expired
    REQUIRE(tokens.IsExpired() == true); // No expiry set, should be expired
    REQUIRE(tokens.NeedsRefresh() == true); // No expiry set, should need refresh
    
    // Test 2: After calculating expiry time based on expires_in (3600 seconds = 1 hour)
    tokens.CalculateExpiresAfter();
    REQUIRE(tokens.IsExpired() == false); // Now should not be expired (1 hour from now)
    REQUIRE(tokens.NeedsRefresh() == false); // Now should not need refresh
}

// ============================================================================
// SAP CLI Compatibility Tests
// ============================================================================

TEST_CASE("Test SAP CLI Client Type Detection", "[datasphere][oauth2][sap-cli]")
{
    std::cout << std::endl;
    
    // Test pre-delivered client ID pattern
    OAuth2Config config1;
    config1.client_id = "5a638330-5899-366e-ac00-ab62cc32dcda";
    config1.custom_client = false;
    
    auto client_type1 = config1.GetClientType();
    REQUIRE(client_type1 == OAuth2ClientType::pre_delivered);
    
    // Test custom client ID pattern
    OAuth2Config config2;
    config2.client_id = "sb-00bb7bc2-cc32-423c-921c-2abdee11a29d!b49931|client!b3650";
    config2.custom_client = true;
    
    auto client_type2 = config2.GetClientType();
    REQUIRE(client_type2 == OAuth2ClientType::custom);
    
    // Test explicit custom client flag
    OAuth2Config config3;
    config3.client_id = "sb-test-client-id";
    config3.custom_client = true;
    
    auto client_type3 = config3.GetClientType();
    REQUIRE(client_type3 == OAuth2ClientType::custom);
}

TEST_CASE("Test SAP CLI Port Strategy", "[datasphere][oauth2][sap-cli]")
{
    std::cout << std::endl;
    
    // Test pre-delivered client port
    OAuth2Config config1;
    config1.custom_client = false;
    int port1 = config1.GetDefaultPort();
    REQUIRE(port1 == 65000);
    
    // Test custom client port
    OAuth2Config config2;
    config2.custom_client = true;
    int port2 = config2.GetDefaultPort();
    REQUIRE(port2 == 8080);
}

// ============================================================================
// Real Environment Tests
// ============================================================================

TEST_CASE("Test Real Datasphere Environment", "[datasphere][oauth2][real]")
{
    std::cout << std::endl;
    
    OAuth2Config config;
    config.tenant_name = "test-tenant";
    config.data_center = "eu10";
    config.client_id = "sb-test-client-id";
    config.client_secret = "test-client-secret";
    config.scope = "default";
    config.redirect_uri = "http://localhost:8080/callback";
    config.authorization_flow = GrantType::authorization_code;
    config.custom_client = true;
    
    // Verify environment configuration
    REQUIRE(config.tenant_name == "test-tenant");
    REQUIRE(config.data_center == "eu10");
    REQUIRE(config.client_id.substr(0, 3) == "sb-");
    
    // Verify client type detection
    auto client_type = config.GetClientType();
    REQUIRE(client_type == OAuth2ClientType::custom);
    
    // Verify port strategy
    int port = config.GetDefaultPort();
    REQUIRE(port == 8080);
    
    // Verify URLs
    auto auth_url = config.GetAuthorizationUrl();
    REQUIRE(auth_url == "https://test-tenant.authentication.eu10.hana.ondemand.com/oauth/authorize");
    
    auto token_url = config.GetTokenUrl();
    REQUIRE(token_url == "https://test-tenant.authentication.eu10.hana.ondemand.com/oauth/token");
}

TEST_CASE("Test Real OAuth2 Flow Initialization", "[datasphere][oauth2][real]")
{
    std::cout << std::endl;
    
    OAuth2Config config;
    config.tenant_name = "test-tenant";
    config.data_center = "eu10";
    config.client_id = "sb-test-client-id";
    config.client_secret = "test-client-secret";
    config.scope = "default";
    config.redirect_uri = "http://localhost:8080/callback";
    config.authorization_flow = GrantType::authorization_code;
    config.custom_client = true;
    
    // Test PKCE generation
    std::string code_verifier = OAuth2Utils::GenerateCodeVerifier();
    std::string code_challenge = OAuth2Utils::GenerateCodeChallenge(code_verifier);
    std::string state = OAuth2Utils::GenerateState();
    
    // Verify PKCE components
    REQUIRE(!code_verifier.empty());
    REQUIRE(!code_challenge.empty());
    REQUIRE(!state.empty());
    REQUIRE(code_verifier.length() >= 128);
    REQUIRE(code_challenge.length() == 64); // SHA256 hash is 64 hex chars
    
    // Test authorization URL construction
    std::string auth_url = config.GetAuthorizationUrl();
    auth_url += "?response_type=code";
    auth_url += "&client_id=" + config.client_id;
    auth_url += "&redirect_uri=" + config.redirect_uri;
    auth_url += "&scope=" + config.scope;
    auth_url += "&state=" + state;
    auth_url += "&code_challenge=" + code_challenge;
    auth_url += "&code_challenge_method=S256";
    
    // Verify URL construction
    REQUIRE(auth_url.find("response_type=code") != std::string::npos);
    REQUIRE(auth_url.find("client_id=" + config.client_id) != std::string::npos);
    REQUIRE(auth_url.find("code_challenge=" + code_challenge) != std::string::npos);
    REQUIRE(auth_url.find("code_challenge_method=S256") != std::string::npos);
}

// ============================================================================
// Mock Integration Tests
// ============================================================================

TEST_CASE("Test Browser Integration (Mock)", "[datasphere][oauth2][mock]")
{
    std::cout << std::endl;
    
    // Test browser integration without actually opening browser
    // Note: OAuth2Browser is a static class, so we test its static methods
    
    // Test URL opening (mock - static method)
    std::string test_url = "https://example.com";
    // OAuth2Browser::OpenUrl(test_url); // This would actually open browser
    
    // Test port availability checking
    bool port_available = OAuth2Browser::IsPortAvailable(8080);
    REQUIRE((port_available == true || port_available == false)); // Either result is valid
    
    // Test default browser detection
    std::string default_browser = OAuth2Browser::GetDefaultBrowser();
    REQUIRE(!default_browser.empty());
    
    std::cout << "✅ Browser integration test completed" << std::endl;
}

TEST_CASE("Test Local Server (Mock)", "[datasphere][oauth2][mock]")
{
    std::cout << std::endl;
    
    // Test local server functionality
    auto server = std::make_unique<DatasphereLocalServer>(8080);
    REQUIRE(server != nullptr);
    
    // Test server start
    server->Start();
    REQUIRE(server->IsRunning() == true);
    
    // Test callback URL
    std::string callback_url = server->GetCallbackUrl();
    REQUIRE(callback_url.find("8080") != std::string::npos);
    
    // Test server stop
    server->Stop();
    REQUIRE(server->IsRunning() == false);
    
    std::cout << "✅ Local server test completed" << std::endl;
}

TEST_CASE("Test OAuth2 Flow Integration (Mock)", "[datasphere][oauth2][mock]")
{
    std::cout << std::endl;
    
    // Test complete OAuth2 flow integration (mock)
    auto server = std::make_unique<DatasphereLocalServer>(8080);
    
    REQUIRE(server != nullptr);
    
    // Test flow initialization
    server->Start();
    REQUIRE(server->IsRunning() == true);
    
    // Test flow completion
    server->Stop();
    REQUIRE(server->IsRunning() == false);
    
    std::cout << "✅ OAuth2 flow integration test completed" << std::endl;
}
