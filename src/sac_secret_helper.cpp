#include "sac_secret_helper.hpp"
#include "odata_url_helpers.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace erpl_web {

SacSecretData ResolveSacSecretData(duckdb::ClientContext& context, const std::string& secret_name) {
    auto& secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    std::unique_ptr<duckdb::SecretEntry> secret_entry;

    try {
        secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
    } catch (...) {
        secret_entry = nullptr;
    }

    if (!secret_entry) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' not found. Please create it using CREATE SECRET " +
            secret_name + " (type 'sac', provider 'access_token', tenant_name => '...', region => '...', " +
            "access_token => '...');");
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret*>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' is not a KeyValueSecret");
    }

    // Extract tenant and region
    auto tenant_it = kv_secret->secret_map.find("tenant_name");
    auto region_it = kv_secret->secret_map.find("region");
    auto access_token_it = kv_secret->secret_map.find("access_token");
    auto base_url_it = kv_secret->secret_map.find("base_url");

    SacSecretData secret_data;

    // base_url is the loopback hatch (see CreateSacSecretFunctions) and stands in for the
    // tenant coordinates when present, which is what makes a test against a local server
    // possible. Otherwise both coordinates are required, because SacUrlBuilder cannot
    // address a tenant without them.
    if (base_url_it != kv_secret->secret_map.end()) {
        secret_data.base_url = base_url_it->second.ToString();
        RequireGatedServiceUrl(secret_data.base_url, "The SAC secret's 'base_url'");
    } else if (tenant_it == kv_secret->secret_map.end() || region_it == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("SAC secret must contain 'tenant_name' and 'region' fields");
    }

    if (tenant_it != kv_secret->secret_map.end()) {
        secret_data.tenant = tenant_it->second.ToString();
    }
    if (region_it != kv_secret->secret_map.end()) {
        secret_data.region = region_it->second.ToString();
    }

    secret_data.auth_params = std::make_shared<HttpAuthParams>();

    // The token is actually attached now. This block used to be an empty `if` body with the
    // comment "Token will be used if available", so auth_params went back with bearer_token
    // unset and every SAC request went out anonymous - a 401 that read like a permissions
    // problem on the user's tenant. See GitHub #243.
    if (access_token_it == kv_secret->secret_map.end() || access_token_it->second.IsNull() ||
        access_token_it->second.ToString().empty()) {
        // Refusing here rather than sending an unauthenticated request: an anonymous GET to
        // SAC does not fail in a way that names this as the cause.
        //
        // client_credentials is deliberately NOT implemented as a silent fallback. SAC's
        // token endpoint shape is not verifiable from this repository - there is no tenant,
        // fixture or captured response to build it against - and guessing at it would put
        // the client secret on the wire against an unverified URL.
        throw duckdb::InvalidInputException(
            "SAC secret '" + secret_name + "' carries no 'access_token', so every request would be "
            "sent unauthenticated. Create it with an access token: CREATE SECRET " + secret_name +
            " (TYPE sac, PROVIDER access_token, access_token '<token>', tenant_name '<tenant>', "
            "region '<region>'). Obtaining a token through client_credentials is not implemented.");
    }

    secret_data.auth_params->bearer_token = access_token_it->second.ToString();

    return secret_data;
}

} // namespace erpl_web
