#include "dataverse_secret.hpp"
#include "duckdb_compat.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"

namespace erpl_web {

void CreateDataverseSecretFunctions::Register(duckdb::ExtensionLoader &loader) {
    ERPL_TRACE_INFO("DATAVERSE_SECRET", "Registering Dataverse secret functions");

    std::string type = "dataverse";

    // Register the secret type
    duckdb::SecretType secret_type;
    secret_type.name = duckdb_compat::Ident(type);
    secret_type.deserializer = duckdb::KeyValueSecret::Deserialize<duckdb::KeyValueSecret>;
    secret_type.default_provider = "client_credentials";

    // Register the client_credentials provider
    duckdb::CreateSecretFunction client_creds_function = {type, "client_credentials", CreateFromClientCredentials, {}};
    duckdb_compat::AddNamedParameter(client_creds_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "environment_url", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(client_creds_function);

    // Register the config provider
    duckdb::CreateSecretFunction config_function = {type, "config", CreateFromConfig, {}};
    duckdb_compat::AddNamedParameter(config_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "environment_url", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "access_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "refresh_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "expires_at", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(config_function);

    loader.RegisterSecretType(secret_type);
    loader.RegisterFunction(client_creds_function);
    loader.RegisterFunction(config_function);

    ERPL_TRACE_INFO("DATAVERSE_SECRET", "Successfully registered Dataverse secret functions");
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateDataverseSecretFunctions::CreateFromClientCredentials(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("DATAVERSE_SECRET", "Creating Dataverse secret with client_credentials provider");

    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Copy parameters
    auto copy_param = [&](const std::string &key) {
        auto val = input.options.find(key);
        if (val != input.options.end()) {
            result->secret_map[duckdb_compat::Ident(key)] = val->second;
            ERPL_TRACE_DEBUG("DATAVERSE_SECRET", "Set parameter: " + key);
        }
    };

    copy_param("tenant_id");
    copy_param("client_id");
    copy_param("client_secret");
    copy_param("environment_url");

    // Validate required parameters
    if (result->secret_map.find("tenant_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'tenant_id' is required for Dataverse authentication");
    }
    if (result->secret_map.find("client_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_id' is required for Dataverse authentication");
    }
    if (result->secret_map.find("client_secret") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_secret' is required for Dataverse authentication");
    }
    if (result->secret_map.find("environment_url") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'environment_url' is required for Dataverse authentication (e.g., https://myorg.crm.dynamics.com)");
    }

    // Get environment URL to construct scope
    auto env_url = result->secret_map["environment_url"].ToString();

    // Remove trailing slash if present
    if (!env_url.empty() && env_url.back() == '/') {
        env_url.pop_back();
    }

    // Set the scope for Dataverse API (use custom scope if provided, otherwise default)
    auto scope_val = input.options.find("scope");
    if (scope_val != input.options.end()) {
        result->secret_map["scope"] = scope_val->second;
    } else {
        // Default scope for Dataverse
        result->secret_map["scope"] = duckdb::Value(env_url + "/.default");
    }

    // Store the grant type
    result->secret_map["grant_type"] = duckdb::Value("client_credentials");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("DATAVERSE_SECRET", "Successfully created Dataverse secret");
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateDataverseSecretFunctions::CreateFromConfig(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("DATAVERSE_SECRET", "Creating Dataverse secret with config provider");

    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Copy all provided parameters
    auto copy_param = [&](const std::string &key) {
        auto val = input.options.find(key);
        if (val != input.options.end()) {
            result->secret_map[duckdb_compat::Ident(key)] = val->second;
        }
    };

    copy_param("tenant_id");
    copy_param("client_id");
    copy_param("client_secret");
    copy_param("environment_url");
    copy_param("access_token");
    copy_param("refresh_token");
    copy_param("expires_at");
    copy_param("scope");

    // Validate environment_url is provided
    if (result->secret_map.find("environment_url") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'environment_url' is required for Dataverse config provider");
    }

    // Set default scope if not provided
    if (result->secret_map.find("scope") == result->secret_map.end()) {
        auto env_url = result->secret_map["environment_url"].ToString();
        if (!env_url.empty() && env_url.back() == '/') {
            env_url.pop_back();
        }
        result->secret_map["scope"] = duckdb::Value(env_url + "/.default");
    }

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("DATAVERSE_SECRET", "Successfully created Dataverse config secret");
    return std::move(result);
}

void CreateDataverseSecretFunctions::RegisterCommonSecretParameters(duckdb::CreateSecretFunction &function) {
    duckdb_compat::AddNamedParameter(function, "name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
}

void CreateDataverseSecretFunctions::RedactCommonKeys(duckdb::KeyValueSecret &result) {
    result.redact_keys.insert("client_secret");
    result.redact_keys.insert("access_token");
    result.redact_keys.insert("refresh_token");
}

duckdb::unique_ptr<duckdb::KeyValueSecret> GetDataverseKeyValueSecret(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);

    if (!secret_entry) {
        throw duckdb::InvalidInputException("Dataverse secret '" + secret_name + "' not found. Use CREATE SECRET to create it.");
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' is not a KeyValueSecret");
    }

    // Clone to extend lifetime beyond SecretEntry unique_ptr
    return duckdb::make_uniq<duckdb::KeyValueSecret>(*kv_secret);
}

DataverseAuthInfo ResolveDataverseAuth(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    ERPL_TRACE_DEBUG("DATAVERSE_AUTH", "Resolving Dataverse authentication for secret: " + secret_name);

    auto kv_secret_up = GetDataverseKeyValueSecret(context, secret_name);
    const auto *kv_secret = kv_secret_up.get();

    // Resolve environment_url
    auto env_url_val = kv_secret->secret_map.find("environment_url");
    std::string environment_url = (env_url_val != kv_secret->secret_map.end())
                                      ? env_url_val->second.ToString()
                                      : "";

    if (environment_url.empty()) {
        throw duckdb::InvalidInputException("Dataverse secret '" + secret_name + "' is missing 'environment_url'");
    }

    // Remove trailing slash if present
    if (!environment_url.empty() && environment_url.back() == '/') {
        environment_url.pop_back();
    }

    // Get access token using Microsoft Entra token manager
    // The Dataverse secret has the same structure as Entra secret, so we can reuse the token manager
    std::string access_token = MicrosoftEntraTokenManager::GetToken(context, kv_secret);

    if (access_token.empty()) {
        throw duckdb::InvalidInputException("Dataverse secret '" + secret_name + "' could not provide a valid access token.");
    }

    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;

    ERPL_TRACE_INFO("DATAVERSE_AUTH", "Successfully resolved Dataverse authentication for: " + environment_url);
    return DataverseAuthInfo{environment_url, access_token, auth_params};
}

} // namespace erpl_web
