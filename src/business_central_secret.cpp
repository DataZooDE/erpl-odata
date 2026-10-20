#include "business_central_secret.hpp"
#include "duckdb_compat.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"

namespace erpl_web {

void CreateBusinessCentralSecretFunctions::Register(duckdb::ExtensionLoader &loader) {
    ERPL_TRACE_INFO("BC_SECRET", "Registering Business Central secret functions");

    std::string type = "business_central";

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
    duckdb_compat::AddNamedParameter(client_creds_function, "environment", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(client_creds_function);

    // Register the config provider
    duckdb::CreateSecretFunction config_function = {type, "config", CreateFromConfig, {}};
    duckdb_compat::AddNamedParameter(config_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "environment", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "access_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "refresh_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "expires_at", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(config_function);

    loader.RegisterSecretType(secret_type);
    loader.RegisterFunction(client_creds_function);
    loader.RegisterFunction(config_function);

    ERPL_TRACE_INFO("BC_SECRET", "Successfully registered Business Central secret functions");
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateBusinessCentralSecretFunctions::CreateFromClientCredentials(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("BC_SECRET", "Creating Business Central secret with client_credentials provider");

    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Copy parameters
    auto copy_param = [&](const std::string &key) {
        auto val = input.options.find(key);
        if (val != input.options.end()) {
            result->secret_map[duckdb_compat::Ident(key)] = val->second;
            ERPL_TRACE_DEBUG("BC_SECRET", "Set parameter: " + key);
        }
    };

    copy_param("tenant_id");
    copy_param("client_id");
    copy_param("client_secret");
    copy_param("environment");

    // Validate required parameters
    if (result->secret_map.find("tenant_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'tenant_id' is required for Business Central authentication");
    }
    if (result->secret_map.find("client_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_id' is required for Business Central authentication");
    }
    if (result->secret_map.find("client_secret") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_secret' is required for Business Central authentication");
    }

    // Set default environment if not provided
    if (result->secret_map.find("environment") == result->secret_map.end()) {
        result->secret_map["environment"] = duckdb::Value("production");
        ERPL_TRACE_DEBUG("BC_SECRET", "Using default environment: production");
    }

    // Set the scope for Business Central API
    result->secret_map["scope"] = duckdb::Value("https://api.businesscentral.dynamics.com/.default");

    // Store the grant type
    result->secret_map["grant_type"] = duckdb::Value("client_credentials");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("BC_SECRET", "Successfully created Business Central secret");
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateBusinessCentralSecretFunctions::CreateFromConfig(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("BC_SECRET", "Creating Business Central secret with config provider");

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
    copy_param("environment");
    copy_param("access_token");
    copy_param("refresh_token");
    copy_param("expires_at");

    // Set the scope for Business Central API
    result->secret_map["scope"] = duckdb::Value("https://api.businesscentral.dynamics.com/.default");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("BC_SECRET", "Successfully created Business Central config secret");
    return std::move(result);
}

void CreateBusinessCentralSecretFunctions::RegisterCommonSecretParameters(duckdb::CreateSecretFunction &function) {
    duckdb_compat::AddNamedParameter(function, "name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
}

void CreateBusinessCentralSecretFunctions::RedactCommonKeys(duckdb::KeyValueSecret &result) {
    result.redact_keys.insert("client_secret");
    result.redact_keys.insert("access_token");
    result.redact_keys.insert("refresh_token");
}

duckdb::unique_ptr<duckdb::KeyValueSecret> GetBusinessCentralKeyValueSecret(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);

    if (!secret_entry) {
        throw duckdb::InvalidInputException("Business Central secret '" + secret_name + "' not found. Use CREATE SECRET to create it.");
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' is not a KeyValueSecret");
    }

    // Clone to extend lifetime beyond SecretEntry unique_ptr
    return duckdb::make_uniq<duckdb::KeyValueSecret>(*kv_secret);
}

BusinessCentralAuthInfo ResolveBusinessCentralAuth(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    ERPL_TRACE_DEBUG("BC_AUTH", "Resolving Business Central authentication for secret: " + secret_name);

    auto kv_secret_up = GetBusinessCentralKeyValueSecret(context, secret_name);
    const auto *kv_secret = kv_secret_up.get();

    // Resolve tenant_id
    auto tenant_id_val = kv_secret->secret_map.find("tenant_id");
    std::string tenant_id = (tenant_id_val != kv_secret->secret_map.end())
                                ? tenant_id_val->second.ToString()
                                : "";

    // Resolve environment
    auto env_val = kv_secret->secret_map.find("environment");
    std::string environment = (env_val != kv_secret->secret_map.end())
                                  ? env_val->second.ToString()
                                  : "production";

    // Get access token using Microsoft Entra token manager
    // The BC secret has the same structure as Entra secret, so we can reuse the token manager
    std::string access_token = MicrosoftEntraTokenManager::GetToken(context, kv_secret);

    if (access_token.empty()) {
        throw duckdb::InvalidInputException("Business Central secret '" + secret_name + "' could not provide a valid access token.");
    }

    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;

    ERPL_TRACE_INFO("BC_AUTH", "Successfully resolved Business Central authentication");
    return BusinessCentralAuthInfo{tenant_id, environment, access_token, auth_params};
}

} // namespace erpl_web
