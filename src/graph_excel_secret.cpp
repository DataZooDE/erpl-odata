#include "graph_excel_secret.hpp"
#include "duckdb_compat.hpp"
#include "datazoo/oauth2/oauth2_flow_v2.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include <chrono>

namespace erpl_web {

void CreateGraphSecretFunctions::Register(duckdb::ExtensionLoader &loader) {
    ERPL_TRACE_INFO("GRAPH_SECRET", "Registering Microsoft Graph secret functions");

    std::string type = "microsoft_graph";

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
    duckdb_compat::AddNamedParameter(client_creds_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(client_creds_function);

    // Register the config provider
    duckdb::CreateSecretFunction config_function = {type, "config", CreateFromConfig, {}};
    duckdb_compat::AddNamedParameter(config_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "access_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "refresh_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "expires_at", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(config_function);

    // Register the authorization_code provider (interactive browser login)
    duckdb::CreateSecretFunction auth_code_function = {type, "authorization_code", CreateFromAuthorizationCode, {}};
    duckdb_compat::AddNamedParameter(auth_code_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(auth_code_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(auth_code_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(auth_code_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(auth_code_function, "redirect_uri", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(auth_code_function);

    loader.RegisterSecretType(secret_type);
    loader.RegisterFunction(client_creds_function);
    loader.RegisterFunction(config_function);
    loader.RegisterFunction(auth_code_function);

    ERPL_TRACE_INFO("GRAPH_SECRET", "Successfully registered Microsoft Graph secret functions");
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateGraphSecretFunctions::CreateFromClientCredentials(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("GRAPH_SECRET", "Creating Microsoft Graph secret with client_credentials provider");

    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Copy parameters
    auto copy_param = [&](const std::string &key) {
        auto val = input.options.find(key);
        if (val != input.options.end()) {
            result->secret_map[duckdb_compat::Ident(key)] = val->second;
            ERPL_TRACE_DEBUG("GRAPH_SECRET", "Set parameter: " + key);
        }
    };

    copy_param("tenant_id");
    copy_param("client_id");
    copy_param("client_secret");

    // Validate required parameters
    if (result->secret_map.find("tenant_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'tenant_id' is required for Microsoft Graph authentication");
    }
    if (result->secret_map.find("client_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_id' is required for Microsoft Graph authentication");
    }
    if (result->secret_map.find("client_secret") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_secret' is required for Microsoft Graph authentication");
    }

    // Set the scope for Microsoft Graph API (use custom scope if provided, otherwise default)
    auto scope_val = input.options.find("scope");
    if (scope_val != input.options.end()) {
        result->secret_map["scope"] = scope_val->second;
    } else {
        // Default scope for Microsoft Graph
        result->secret_map["scope"] = duckdb::Value("https://graph.microsoft.com/.default");
    }

    // Store the grant type
    result->secret_map["grant_type"] = duckdb::Value("client_credentials");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("GRAPH_SECRET", "Successfully created Microsoft Graph secret");
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateGraphSecretFunctions::CreateFromConfig(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("GRAPH_SECRET", "Creating Microsoft Graph secret with config provider");

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
    copy_param("access_token");
    copy_param("refresh_token");
    copy_param("expires_at");
    copy_param("scope");

    // Set default scope if not provided
    if (result->secret_map.find("scope") == result->secret_map.end()) {
        result->secret_map["scope"] = duckdb::Value("https://graph.microsoft.com/.default");
    }

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("GRAPH_SECRET", "Successfully created Microsoft Graph config secret");
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateGraphSecretFunctions::CreateFromAuthorizationCode(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("GRAPH_SECRET", "Creating Microsoft Graph secret with authorization_code provider (interactive login)");

    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Get required parameters
    auto tenant_id_val = input.options.find("tenant_id");
    auto client_id_val = input.options.find("client_id");
    auto client_secret_val = input.options.find("client_secret");
    auto scope_val = input.options.find("scope");
    auto redirect_uri_val = input.options.find("redirect_uri");

    if (tenant_id_val == input.options.end()) {
        throw duckdb::InvalidInputException("'tenant_id' is required for Microsoft Graph authorization_code flow");
    }
    if (client_id_val == input.options.end()) {
        throw duckdb::InvalidInputException("'client_id' is required for Microsoft Graph authorization_code flow");
    }

    std::string tenant_id = tenant_id_val->second.ToString();
    std::string client_id = client_id_val->second.ToString();
    std::string client_secret = (client_secret_val != input.options.end())
        ? client_secret_val->second.ToString()
        : "";

    // Default scopes for delegated access (user permissions)
    std::string scopes = (scope_val != input.options.end())
        ? scope_val->second.ToString()
        : "openid profile offline_access User.Read Files.ReadWrite.All Sites.ReadWrite.All Mail.Read Calendars.Read Contacts.Read Team.ReadBasic.All Channel.ReadBasic.All";

    // Default redirect URI
    std::string redirect_uri = (redirect_uri_val != input.options.end())
        ? redirect_uri_val->second.ToString()
        : "http://localhost:8080/callback";

    // Build Microsoft Entra ID OAuth2 URLs
    std::string auth_url = "https://login.microsoftonline.com/" + tenant_id + "/oauth2/v2.0/authorize";
    std::string token_url = "https://login.microsoftonline.com/" + tenant_id + "/oauth2/v2.0/token";

    ERPL_TRACE_INFO("GRAPH_SECRET", "Starting interactive OAuth2 login for Microsoft Graph");
    ERPL_TRACE_DEBUG("GRAPH_SECRET", "Authorization URL: " + auth_url);
    ERPL_TRACE_DEBUG("GRAPH_SECRET", "Token URL: " + token_url);

    // Configure OAuth2 flow with custom Microsoft URLs
    OAuth2Config config;
    config.client_id = client_id;
    config.client_secret = client_secret;  // Optional for public clients, required for confidential clients
    config.scope = scopes;
    config.redirect_uri = redirect_uri;
    config.authorization_flow = GrantType::authorization_code;
    config.custom_client = true;  // Use port 8080 and include client credentials in token exchange
    config.custom_auth_url = auth_url;
    config.custom_token_url = token_url;

    // Execute interactive OAuth2 flow (opens browser)
    OAuth2FlowV2 oauth2_flow;
    OAuth2Tokens tokens = oauth2_flow.ExecuteFlow(config);

    if (tokens.access_token.empty()) {
        throw duckdb::InvalidInputException("Failed to acquire access token via interactive login");
    }

    ERPL_TRACE_INFO("GRAPH_SECRET", "Successfully acquired tokens via interactive login");

    // Store parameters in secret
    result->secret_map["tenant_id"] = duckdb::Value(tenant_id);
    result->secret_map["client_id"] = duckdb::Value(client_id);
    if (!client_secret.empty()) {
        result->secret_map["client_secret"] = duckdb::Value(client_secret);
    }
    result->secret_map["scope"] = duckdb::Value(scopes);
    result->secret_map["access_token"] = duckdb::Value(tokens.access_token);

    if (!tokens.refresh_token.empty()) {
        result->secret_map["refresh_token"] = duckdb::Value(tokens.refresh_token);
    }

    // Store expiration as Unix timestamp
    if (tokens.expires_after > 0) {
        result->secret_map["expires_at"] = duckdb::Value(std::to_string(tokens.expires_after));
    }

    // Store grant type for token refresh
    result->secret_map["grant_type"] = duckdb::Value("authorization_code");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("GRAPH_SECRET", "Successfully created Microsoft Graph secret with delegated tokens");
    return std::move(result);
}

void CreateGraphSecretFunctions::RegisterCommonSecretParameters(duckdb::CreateSecretFunction &function) {
    duckdb_compat::AddNamedParameter(function, "name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
}

void CreateGraphSecretFunctions::RedactCommonKeys(duckdb::KeyValueSecret &result) {
    result.redact_keys.insert("client_secret");
    result.redact_keys.insert("access_token");
    result.redact_keys.insert("refresh_token");
}

duckdb::unique_ptr<duckdb::KeyValueSecret> GetGraphKeyValueSecret(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);

    std::unique_ptr<duckdb::SecretEntry> secret_entry;

    if (secret_name.empty()) {
        // No secret name provided - look up first secret of type microsoft_graph
        auto match = secret_manager.LookupSecret(transaction, "", "microsoft_graph");
        if (match.HasMatch()) {
            // Clone the secret entry
            secret_entry = duckdb::make_uniq<duckdb::SecretEntry>(*match.secret_entry);
        }
        if (!secret_entry) {
            throw duckdb::InvalidInputException(
                "No Microsoft Graph secret found. Create one using:\n"
                "  CREATE SECRET (TYPE microsoft_graph, PROVIDER authorization_code, tenant_id '...', client_id '...', client_secret '...');\n"
                "Or specify a secret name explicitly using the 'secret' parameter.");
        }
    } else {
        // Secret name provided - look up by name
        secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
        if (!secret_entry) {
            throw duckdb::InvalidInputException("Microsoft Graph secret '" + secret_name + "' not found. Use CREATE SECRET to create it.");
        }
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret is not a KeyValueSecret");
    }

    // Clone to extend lifetime beyond SecretEntry unique_ptr
    return duckdb::make_uniq<duckdb::KeyValueSecret>(*kv_secret);
}

GraphAuthInfo ResolveGraphAuth(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    ERPL_TRACE_DEBUG("GRAPH_AUTH", "Resolving Microsoft Graph authentication for secret: " + secret_name);

    auto kv_secret_up = GetGraphKeyValueSecret(context, secret_name);
    const auto *kv_secret = kv_secret_up.get();

    // Get access token using Microsoft Entra token manager
    // The Graph secret has the same structure as Entra secret, so we can reuse the token manager
    std::string access_token = MicrosoftEntraTokenManager::GetToken(context, kv_secret);

    if (access_token.empty()) {
        throw duckdb::InvalidInputException("Microsoft Graph secret '" + secret_name + "' could not provide a valid access token.");
    }

    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;

    ERPL_TRACE_INFO("GRAPH_AUTH", "Successfully resolved Microsoft Graph authentication");
    return GraphAuthInfo{access_token, auth_params};
}

} // namespace erpl_web
