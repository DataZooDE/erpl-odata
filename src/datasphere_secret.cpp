#include "odata_url_helpers.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_argument_helper.hpp"
#include "datasphere_secret.hpp"
#include "datazoo/oauth2/oauth2_flow_v2.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/exception.hpp"
#include <fstream>
#include <sstream>
#include <chrono>

namespace erpl_web {

namespace {
static std::string UrlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << '+';
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return escaped.str();
}
}

// DatasphereSecretData implementation
bool DatasphereSecretData::HasValidToken() const {
    return !access_token.empty() && !IsTokenExpired();
}

bool DatasphereSecretData::IsTokenExpired() const {
    if (expires_at.empty()) {
        return true;
    }
    
    try {
        auto expiration_time = GetExpirationTime();
        auto now = std::chrono::system_clock::now();
        return now >= expiration_time;
    } catch (...) {
        return true;
    }
}

std::chrono::time_point<std::chrono::system_clock> DatasphereSecretData::GetExpirationTime() const {
    if (expires_at.empty()) {
        return std::chrono::system_clock::now();
    }
    
    try {
        std::time_t expiration_timestamp = std::stoll(expires_at);
        return std::chrono::system_clock::from_time_t(expiration_timestamp);
    } catch (...) {
        return std::chrono::system_clock::now();
    }
}

// Secret creation functions implementation

void CreateDatasphereSecretFunctions::Register(duckdb::ExtensionLoader &loader) {
    std::string type = "datasphere";
    
    // Register the new type
    duckdb::SecretType secret_type;
    secret_type.name = duckdb_compat::Ident(type);
    secret_type.deserializer = duckdb::KeyValueSecret::Deserialize<duckdb::KeyValueSecret>;
    secret_type.default_provider = "oauth2";
    
    // Register the oauth2 secret provider
    duckdb::CreateSecretFunction oauth2_function = {type, "oauth2", CreateDatasphereSecretFromOAuth2, {}};
    duckdb_compat::AddNamedParameter(oauth2_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(oauth2_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(oauth2_function, "tenant_name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(oauth2_function, "data_center", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(oauth2_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(oauth2_function, "redirect_uri", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    // Optional for explicit grant/endpoints
    duckdb_compat::AddNamedParameter(oauth2_function, "grant_type", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(oauth2_function, "token_url", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(oauth2_function);
    
    // Register the config secret provider
    duckdb::CreateSecretFunction config_function = {type, "config", CreateDatasphereSecretFromConfig, {}};
    duckdb_compat::AddNamedParameter(config_function, "config_file", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(config_function);
    
    // Register the file secret provider
    duckdb::CreateSecretFunction file_function = {type, "file", CreateDatasphereSecretFromFile, {}};
    duckdb_compat::AddNamedParameter(file_function, "filepath", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(file_function);
    
    loader.RegisterSecretType(secret_type);
    loader.RegisterFunction(oauth2_function);
    loader.RegisterFunction(config_function);
    loader.RegisterFunction(file_function);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateDatasphereSecretFunctions::CreateDatasphereSecretFromOAuth2(
    duckdb::ClientContext &context, 
    duckdb::CreateSecretInput &input) {
    
    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);
    
    // Copy OAuth2 configuration parameters
    auto copy_param = [&](const std::string &key) {
        auto val = input.options.find(key);
        if (val != input.options.end()) {
            result->secret_map[duckdb_compat::Ident(key)] = val->second;
        }
    };
    
    copy_param("client_id");
    copy_param("client_secret");
    copy_param("tenant_name");
    copy_param("data_center");
    copy_param("scope");
    copy_param("redirect_uri");
    copy_param("grant_type");
    copy_param("token_url");
    
    // Set default values if not provided
    if (result->secret_map.find("scope") == result->secret_map.end()) {
        result->secret_map["scope"] = duckdb::Value("default");
    }
    if (result->secret_map.find("redirect_uri") == result->secret_map.end()) {
        result->secret_map["redirect_uri"] = duckdb::Value("http://localhost:65000");
    }
    
    // Redact sensitive keys
    RedactCommonKeys(*result);
    result->redact_keys.insert("client_secret");
    
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateDatasphereSecretFunctions::CreateDatasphereSecretFromConfig(
    duckdb::ClientContext &context, 
    duckdb::CreateSecretInput &input) {
    
    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);
    
    // Get config file path
    auto config_file_val = input.options.find("config_file");
    if (config_file_val == input.options.end()) {
        throw duckdb::InvalidInputException("'config_file' parameter is required for config provider");
    }
    
    std::string config_file = config_file_val->second.ToString();
    
    // Read and parse config file
    std::ifstream ifs(config_file);
    if (!ifs.is_open()) {
        throw duckdb::IOException("Could not open config file at: " + config_file);
    }
    
    // For now, implement basic config file parsing
    // This can be enhanced to support JSON, INI, or other formats
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            result->secret_map[duckdb_compat::Ident(key)] = duckdb::Value(value);
        }
    }
    
    // Redact sensitive keys
    RedactCommonKeys(*result);
    result->redact_keys.insert("client_secret");
    
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateDatasphereSecretFunctions::CreateDatasphereSecretFromFile(
    duckdb::ClientContext &context, 
    duckdb::CreateSecretInput &input) {
    
    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);
    
    // Get file path
    auto filepath_val = input.options.find("filepath");
    if (filepath_val == input.options.end()) {
        throw duckdb::InvalidInputException("'filepath' parameter is required for file provider");
    }
    
    std::string filepath = filepath_val->second.ToString();
    
    // Store filepath for reference
    result->secret_map["filepath"] = duckdb::Value(filepath);
    
    // For now, just store the filepath
    // In a full implementation, you might want to read and parse the file
    // similar to how gsheets handles service account JSON files
    
    // Redact sensitive keys
    RedactCommonKeys(*result);
    result->redact_keys.insert("filepath");
    
    return std::move(result);
}

void CreateDatasphereSecretFunctions::RegisterCommonSecretParameters(duckdb::CreateSecretFunction &function) {
    // Register common parameters for all Datasphere secret providers
    duckdb_compat::AddNamedParameter(function, "name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
}

void CreateDatasphereSecretFunctions::RedactCommonKeys(duckdb::KeyValueSecret &result) {
    // Redact common sensitive keys
    result.redact_keys.insert("client_secret");
    result.redact_keys.insert("access_token");
    result.redact_keys.insert("refresh_token");
}

// Token management implementation
std::string DatasphereTokenManager::GetToken(duckdb::ClientContext &context, const duckdb::KeyValueSecret *kv_secret) {
    // Check if we have a valid cached token
    if (HasValidCachedToken(kv_secret)) {
        return GetCachedToken(kv_secret);
    }
    
    // Perform OAuth2 flow to get new tokens
    OAuth2Tokens new_tokens = PerformOAuth2Flow(context, kv_secret);
    
    // Update the secret with new tokens
    UpdateSecretWithTokens(context, kv_secret, new_tokens);
    
    // Return the new token directly instead of trying to read from potentially stale secret
    return new_tokens.access_token;
}

void DatasphereTokenManager::RefreshTokens(duckdb::ClientContext &context, const duckdb::KeyValueSecret *kv_secret) {
    // Perform OAuth2 flow to get new tokens
    OAuth2Tokens new_tokens = PerformOAuth2Flow(context, kv_secret);
    
    // Update the secret with new tokens
    UpdateSecretWithTokens(context, kv_secret, new_tokens);
}

bool DatasphereTokenManager::IsTokenValid(const duckdb::KeyValueSecret *kv_secret) {
    return HasValidCachedToken(kv_secret);
}

void DatasphereTokenManager::UpdateSecretWithTokens(
    duckdb::ClientContext &context, 
    const duckdb::KeyValueSecret *kv_secret,
    const OAuth2Tokens &tokens) {
    
    // Get the secret manager
    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto secret_name = duckdb_compat::Str(kv_secret->GetName());
    
    // Get the current secret to preserve metadata
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto old_secret = secret_manager.GetSecretByName(transaction, secret_name);
    auto persist_type = old_secret->persist_type;
    auto storage_mode = old_secret->storage_mode;
    
    // Clone the old secret to get metadata
    auto new_secret = old_secret->secret->Clone();
    auto new_secret_kv = dynamic_cast<const duckdb::KeyValueSecret*>(new_secret.get());
    
    if (!new_secret_kv) {
        throw duckdb::InvalidInputException("Failed to clone secret as KeyValueSecret");
    }
    
    // Create a new KeyValueSecret by copying the existing one
    duckdb::KeyValueSecret updated_secret(*new_secret_kv);
    
    // Add the new tokens
    updated_secret.secret_map["access_token"] = duckdb::Value(tokens.access_token);
    updated_secret.secret_map["refresh_token"] = duckdb::Value(tokens.refresh_token);
    updated_secret.secret_map["expires_at"] = duckdb::Value(std::to_string(tokens.expires_after));
    updated_secret.secret_map["token_type"] = duckdb::Value(tokens.token_type);
    updated_secret.secret_map["scope"] = duckdb::Value(tokens.scope);
    
    // Register the updated secret
    secret_manager.RegisterSecret(transaction, duckdb::make_uniq<duckdb::KeyValueSecret>(updated_secret),
                                  duckdb::OnCreateConflict::REPLACE_ON_CONFLICT, persist_type, storage_mode);
}

bool DatasphereTokenManager::HasValidCachedToken(const duckdb::KeyValueSecret *kv_secret) {
    // Check if token exists
    auto token_val = kv_secret->secret_map.find("access_token");
    if (token_val == kv_secret->secret_map.end()) {
        return false;
    }
    
    // Check if token is expired
    auto expires_at_val = kv_secret->secret_map.find("expires_at");
    if (expires_at_val == kv_secret->secret_map.end()) {
        return false;
    }
    
    try {
        std::time_t expiration_time = std::stoll(expires_at_val->second.ToString());
        std::time_t current_time = std::time(nullptr);
        return expiration_time > current_time;
    } catch (...) {
        return false;
    }
}

std::string DatasphereTokenManager::GetCachedToken(const duckdb::KeyValueSecret *kv_secret) {
    auto token_val = kv_secret->secret_map.find("access_token");
    if (token_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'access_token' not found in 'datasphere' secret");
    }
    return token_val->second.ToString();
}

OAuth2Tokens DatasphereTokenManager::PerformOAuth2Flow(duckdb::ClientContext &context, const duckdb::KeyValueSecret *kv_secret) {
    // Extract OAuth2 configuration from secret
    auto client_id_val = kv_secret->secret_map.find("client_id");
    auto client_secret_val = kv_secret->secret_map.find("client_secret");
    auto tenant_name_val = kv_secret->secret_map.find("tenant_name");
    auto data_center_val = kv_secret->secret_map.find("data_center");
    auto scope_val = kv_secret->secret_map.find("scope");
    auto redirect_uri_val = kv_secret->secret_map.find("redirect_uri");
    auto grant_type_val = kv_secret->secret_map.find("grant_type");
    auto token_url_val = kv_secret->secret_map.find("token_url");

    // Branch for client_credentials (no refresh token, renew on expiry)
    std::string grant_type = (grant_type_val != kv_secret->secret_map.end()) ? duckdb::StringUtil::Lower(grant_type_val->second.ToString()) : std::string("authorization_code");
    if (grant_type == "client_credentials") {
        if (client_id_val == kv_secret->secret_map.end()) {
            throw duckdb::InvalidInputException("'client_id' not found in 'datasphere' secret");
        }
        if (client_secret_val == kv_secret->secret_map.end()) {
            throw duckdb::InvalidInputException("'client_secret' not found in 'datasphere' secret");
        }

        // Determine token URL from secret or tenant/data_center
        std::string token_url;
        if (token_url_val != kv_secret->secret_map.end() && !token_url_val->second.IsNull()) {
            token_url = token_url_val->second.ToString();
        } else {
            if (tenant_name_val == kv_secret->secret_map.end() || data_center_val == kv_secret->secret_map.end()) {
                throw duckdb::InvalidInputException("For client_credentials, either 'token_url' must be provided or both 'tenant_name' and 'data_center' must be set in the secret");
            }
            OAuth2Config tmp_cfg; tmp_cfg.tenant_name = tenant_name_val->second.ToString(); tmp_cfg.data_center = data_center_val->second.ToString();
            token_url = tmp_cfg.GetTokenUrl();
        }

        // This URL is about to receive the client secret in an Authorization: Basic header,
        // so it gets the same gate as every other place a Datasphere credential is sent -
        // the sibling seam BuildDataUrl (datasphere_read.cpp) already does exactly this.
        //
        // It matters most for the values we did not build: 'token_url' is taken from the
        // secret, and the `config` provider copies every key=value line out of a file into
        // the secret map, so a tampered config can supply a token_url the user never typed.
        // Without the gate that POSTs a long-lived client secret, in cleartext, to whatever
        // host the file named.
        RequireGatedServiceUrl(token_url, "The Datasphere 'token_url'");

        // Build request body
        std::string body = "grant_type=client_credentials";
        // Determine effective scope: default to 'apiaccess' when missing or set to 'default'
        std::string effective_scope;
        if (scope_val != kv_secret->secret_map.end()) {
            effective_scope = scope_val->second.ToString();
        }
        if (effective_scope.empty() || duckdb::StringUtil::Lower(effective_scope) == "default") {
            effective_scope = "openid uaa.resource dmi-api-proxy-sac-saceu10!t3650.apiaccess";
        }
        body += "&scope=" + UrlEncode(effective_scope);

        HttpRequest request(HttpMethod::POST, token_url, "application/x-www-form-urlencoded", body);
        request.headers.emplace("Accept", "application/json");
        // Use Basic auth
        auto basic = HttpAuthParams::Base64Encode(client_id_val->second.ToString() + ":" + client_secret_val->second.ToString());
        request.headers.emplace("Authorization", "Basic " + basic);

        HttpClient http;
        auto resp = http.SendRequest(request);
        if (!resp) {
            throw duckdb::IOException("No response from token endpoint");
        }
        if (resp->Code() != 200) {
            throw duckdb::IOException(duckdb::StringUtil::Format("Token endpoint returned HTTP %d: %s", resp->Code(), resp->Content().c_str()));
        }

        OAuth2Tokens tokens;
        auto content = resp->Content();
        auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
        if (!doc) {
            throw duckdb::IOException("Failed to parse token JSON");
        }
        auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
        auto access_token_json = duckdb_yyjson::yyjson_obj_get(root, "access_token");
        if (access_token_json && duckdb_yyjson::yyjson_is_str(access_token_json)) {
            tokens.access_token = duckdb_yyjson::yyjson_get_str(access_token_json);
        } else {
            duckdb_yyjson::yyjson_doc_free(doc);
            throw duckdb::IOException("access_token missing in token response");
        }
        auto token_type_json = duckdb_yyjson::yyjson_obj_get(root, "token_type");
        if (token_type_json && duckdb_yyjson::yyjson_is_str(token_type_json)) {
            tokens.token_type = duckdb_yyjson::yyjson_get_str(token_type_json);
        } else {
            tokens.token_type = "Bearer";
        }
        auto scope_json = duckdb_yyjson::yyjson_obj_get(root, "scope");
        if (scope_json && duckdb_yyjson::yyjson_is_str(scope_json)) {
            tokens.scope = duckdb_yyjson::yyjson_get_str(scope_json);
        } else if (scope_val != kv_secret->secret_map.end()) {
            tokens.scope = scope_val->second.ToString();
        }
        auto expires_in_json = duckdb_yyjson::yyjson_obj_get(root, "expires_in");
        if (expires_in_json && duckdb_yyjson::yyjson_is_int(expires_in_json)) {
            tokens.expires_in = static_cast<int>(duckdb_yyjson::yyjson_get_int(expires_in_json));
            tokens.CalculateExpiresAfter();
        }
        // Client credentials does not usually return refresh_token
        tokens.refresh_token.clear();
        duckdb_yyjson::yyjson_doc_free(doc);
        return tokens;
    }

    // Interactive authorization_code (existing behavior)
    if (client_id_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_id' not found in 'datasphere' secret");
    }
    if (client_secret_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_secret' not found in 'datasphere' secret");
    }
    if (tenant_name_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'tenant_name' not found in 'datasphere' secret");
    }
    if (data_center_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'data_center' not found in 'datasphere' secret");
    }

    OAuth2Config config;
    config.client_id = client_id_val->second.ToString();
    config.client_secret = client_secret_val->second.ToString();
    config.tenant_name = tenant_name_val->second.ToString();
    config.data_center = data_center_val->second.ToString();
    config.scope = (scope_val != kv_secret->secret_map.end()) ? scope_val->second.ToString() : std::string("default");
    config.redirect_uri = (redirect_uri_val != kv_secret->secret_map.end()) ? redirect_uri_val->second.ToString() : std::string("http://localhost:65000");

    OAuth2FlowV2 oauth2_flow;
    return oauth2_flow.ExecuteFlow(config);
}

// ----------------------------------------------------------------------------
// Unified secret helpers
// ----------------------------------------------------------------------------
duckdb::unique_ptr<duckdb::KeyValueSecret> GetDatasphereKeyValueSecret(duckdb::ClientContext &context, const std::string &secret_name) {
    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
    if (!secret_entry) {
        throw duckdb::InvalidInputException("Datasphere secret '" + secret_name + "' not found. Use CREATE SECRET to create it.");
    }
    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' is not a KeyValueSecret");
    }
    // Clone to extend lifetime beyond SecretEntry unique_ptr
    return duckdb::make_uniq<duckdb::KeyValueSecret>(*kv_secret);
}

DatasphereAuthInfo ResolveDatasphereAuth(duckdb::ClientContext &context, const std::string &secret_name) {
    auto kv_secret_up = GetDatasphereKeyValueSecret(context, secret_name);
    const auto *kv_secret = kv_secret_up.get();

    // Resolve tenant and data center
    // Missing keys are reported as bad input naming the secret and the field. TryGetValue
    // with error_on_missing throws InternalException, which invalidates the database
    // instance over a typo in a CREATE SECRET (GitHub #243).
    auto tenant = RequireSecretValue(*kv_secret, "tenant_name", secret_name);
    auto data_center = RequireSecretValue(*kv_secret, "data_center", secret_name);

    // Token: use the public GetToken method which handles caching and refresh automatically
    std::string access_token = DatasphereTokenManager::GetToken(context, kv_secret);

    if (access_token.empty()) {
        throw duckdb::InvalidInputException("Datasphere secret '" + secret_name + "' does not contain a valid access_token. Please authenticate first using the OAuth2 flow.");
    }

    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;

    return DatasphereAuthInfo{tenant, data_center, access_token, auth_params};
}

} // namespace erpl_web
