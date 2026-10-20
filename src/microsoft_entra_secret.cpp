#include "microsoft_entra_secret.hpp"
#include "duckdb_compat.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "tracing.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/exception.hpp"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace erpl_web {

namespace {
struct MicrosoftTokenResponse {
    std::string access_token;
    std::string refresh_token;
    int expires_in = 0;
};

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

static MicrosoftTokenResponse ParseMicrosoftTokenResponse(const std::string &content, const std::string &endpoint_name) {
    auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
    if (!doc) {
        throw duckdb::IOException("Failed to parse %s token response", endpoint_name.c_str());
    }

    MicrosoftTokenResponse result;
    auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
    auto access_token_json = duckdb_yyjson::yyjson_obj_get(root, "access_token");
    if (!access_token_json || !duckdb_yyjson::yyjson_is_str(access_token_json)) {
        duckdb_yyjson::yyjson_doc_free(doc);
        throw duckdb::IOException("access_token missing in %s token response", endpoint_name.c_str());
    }

    result.access_token = duckdb_yyjson::yyjson_get_str(access_token_json);

    auto refresh_token_json = duckdb_yyjson::yyjson_obj_get(root, "refresh_token");
    if (refresh_token_json && duckdb_yyjson::yyjson_is_str(refresh_token_json)) {
        result.refresh_token = duckdb_yyjson::yyjson_get_str(refresh_token_json);
    }

    auto expires_in_json = duckdb_yyjson::yyjson_obj_get(root, "expires_in");
    if (expires_in_json && duckdb_yyjson::yyjson_is_num(expires_in_json)) {
        result.expires_in = static_cast<int>(duckdb_yyjson::yyjson_get_int(expires_in_json));
    }

    duckdb_yyjson::yyjson_doc_free(doc);
    return result;
}

static std::string ParseMicrosoftError(const std::string &content) {
    if (content.empty()) {
        return "";
    }
    auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
    if (!doc) {
        return "";
    }
    std::string result;
    auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
    auto error_desc = duckdb_yyjson::yyjson_obj_get(root, "error_description");
    if (error_desc && duckdb_yyjson::yyjson_is_str(error_desc)) {
        result = duckdb_yyjson::yyjson_get_str(error_desc);
    }
    duckdb_yyjson::yyjson_doc_free(doc);
    return result;
}

static MicrosoftTokenResponse AcquireTokenWithRefreshToken(
    const std::string &tenant_id,
    const std::string &client_id,
    const std::string &client_secret,
    const std::string &scope,
    const std::string &refresh_token) {

    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Refreshing token for tenant: " + tenant_id);

    std::string body = "grant_type=refresh_token";
    body += "&client_id=" + UrlEncode(client_id);
    body += "&refresh_token=" + UrlEncode(refresh_token);
    if (!client_secret.empty()) {
        body += "&client_secret=" + UrlEncode(client_secret);
    }
    if (!scope.empty()) {
        body += "&scope=" + UrlEncode(scope);
    }

    HttpRequest request(HttpMethod::POST, MicrosoftEntraTokenManager::GetTokenUrl(tenant_id),
                        "application/x-www-form-urlencoded", body);
    request.headers.emplace("Accept", "application/json");

    HttpClient http;
    auto resp = http.SendRequest(request);
    if (!resp) {
        throw duckdb::IOException("No response from Microsoft Entra refresh token endpoint");
    }
    if (resp->Code() != 200) {
        std::string error_msg = "Microsoft Entra refresh token endpoint returned HTTP " + std::to_string(resp->Code());
        auto details = ParseMicrosoftError(resp->Content());
        if (!details.empty()) {
            error_msg += ": " + details;
        }
        throw duckdb::IOException(error_msg);
    }

    return ParseMicrosoftTokenResponse(resp->Content(), "Microsoft Entra refresh");
}
} // anonymous namespace

// MicrosoftEntraSecretData implementation
bool MicrosoftEntraSecretData::HasValidToken() const {
    return !access_token.empty() && !IsTokenExpired();
}

bool MicrosoftEntraSecretData::IsTokenExpired() const {
    if (expires_at.empty()) {
        return true;
    }

    try {
        auto expiration_time = GetExpirationTime();
        auto now = std::chrono::system_clock::now();
        // Add 5-minute buffer to avoid using tokens that are about to expire
        return now >= (expiration_time - std::chrono::minutes(5));
    } catch (...) {
        return true;
    }
}

std::chrono::time_point<std::chrono::system_clock> MicrosoftEntraSecretData::GetExpirationTime() const {
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
void CreateMicrosoftEntraSecretFunctions::Register(duckdb::ExtensionLoader &loader) {
    ERPL_TRACE_INFO("MS_ENTRA_SECRET", "Registering Microsoft Entra ID secret functions");

    std::string type = "microsoft_entra";

    // Register the secret type
    duckdb::SecretType secret_type;
    secret_type.name = duckdb_compat::Ident(type);
    secret_type.deserializer = duckdb::KeyValueSecret::Deserialize<duckdb::KeyValueSecret>;
    secret_type.default_provider = "client_credentials";

    // Register the client_credentials provider (most common for service-to-service)
    duckdb::CreateSecretFunction client_creds_function = {type, "client_credentials", CreateMicrosoftEntraSecretFromClientCredentials, {}};
    duckdb_compat::AddNamedParameter(client_creds_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(client_creds_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(client_creds_function);

    // Register the config provider (for reading credentials from file)
    duckdb::CreateSecretFunction config_function = {type, "config", CreateMicrosoftEntraSecretFromConfig, {}};
    duckdb_compat::AddNamedParameter(config_function, "tenant_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "access_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(config_function);

    loader.RegisterSecretType(secret_type);
    loader.RegisterFunction(client_creds_function);
    loader.RegisterFunction(config_function);

    ERPL_TRACE_INFO("MS_ENTRA_SECRET", "Successfully registered Microsoft Entra ID secret functions");
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateMicrosoftEntraSecretFunctions::CreateMicrosoftEntraSecretFromClientCredentials(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("MS_ENTRA_SECRET", "Creating Microsoft Entra secret with client_credentials provider");

    auto scope = input.scope;
    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Copy parameters
    auto copy_param = [&](const std::string &key) {
        auto val = input.options.find(key);
        if (val != input.options.end()) {
            result->secret_map[duckdb_compat::Ident(key)] = val->second;
            ERPL_TRACE_DEBUG("MS_ENTRA_SECRET", "Set parameter: " + key);
        }
    };

    copy_param("tenant_id");
    copy_param("client_id");
    copy_param("client_secret");
    copy_param("scope");

    // Validate required parameters
    if (result->secret_map.find("tenant_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'tenant_id' is required for Microsoft Entra authentication");
    }
    if (result->secret_map.find("client_id") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_id' is required for Microsoft Entra authentication");
    }
    if (result->secret_map.find("client_secret") == result->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_secret' is required for Microsoft Entra authentication");
    }

    // Set default scope if not provided (Graph API default scope)
    if (result->secret_map.find("scope") == result->secret_map.end()) {
        result->secret_map["scope"] = duckdb::Value("https://graph.microsoft.com/.default");
        ERPL_TRACE_DEBUG("MS_ENTRA_SECRET", "Using default scope: https://graph.microsoft.com/.default");
    }

    // Store the grant type
    result->secret_map["grant_type"] = duckdb::Value("client_credentials");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("MS_ENTRA_SECRET", "Successfully created Microsoft Entra secret");
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateMicrosoftEntraSecretFunctions::CreateMicrosoftEntraSecretFromConfig(
    duckdb::ClientContext &context,
    duckdb::CreateSecretInput &input) {

    ERPL_TRACE_DEBUG("MS_ENTRA_SECRET", "Creating Microsoft Entra secret with config provider");

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
    copy_param("scope");
    copy_param("access_token");

    // Redact sensitive keys
    RedactCommonKeys(*result);

    ERPL_TRACE_INFO("MS_ENTRA_SECRET", "Successfully created Microsoft Entra config secret");
    return std::move(result);
}

void CreateMicrosoftEntraSecretFunctions::RegisterCommonSecretParameters(duckdb::CreateSecretFunction &function) {
    duckdb_compat::AddNamedParameter(function, "name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
}

void CreateMicrosoftEntraSecretFunctions::RedactCommonKeys(duckdb::KeyValueSecret &result) {
    result.redact_keys.insert("client_secret");
    result.redact_keys.insert("access_token");
    result.redact_keys.insert("refresh_token");
}

// Token management implementation
std::string MicrosoftEntraTokenManager::GetToken(duckdb::ClientContext &context, const duckdb::KeyValueSecret *kv_secret) {
    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Getting Microsoft Entra token");

    // Check if we have a valid cached token
    if (HasValidCachedToken(kv_secret)) {
        ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Using cached token");
        return GetCachedToken(kv_secret);
    }

    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Cached token invalid or expired, acquiring new token");

    // Get credentials from secret
    auto tenant_id_val = kv_secret->secret_map.find("tenant_id");
    auto client_id_val = kv_secret->secret_map.find("client_id");
    auto client_secret_val = kv_secret->secret_map.find("client_secret");
    auto scope_val = kv_secret->secret_map.find("scope");
    auto refresh_token_val = kv_secret->secret_map.find("refresh_token");
    auto grant_type_val = kv_secret->secret_map.find("grant_type");

    if (tenant_id_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'tenant_id' not found in Microsoft Entra secret");
    }
    if (client_id_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'client_id' not found in Microsoft Entra secret");
    }

    std::string tenant_id = tenant_id_val->second.ToString();
    std::string client_id = client_id_val->second.ToString();
    std::string client_secret = (client_secret_val != kv_secret->secret_map.end())
                                    ? client_secret_val->second.ToString()
                                    : "";
    std::string scope = (scope_val != kv_secret->secret_map.end())
                            ? scope_val->second.ToString()
                            : "https://graph.microsoft.com/.default";
    std::string grant_type = (grant_type_val != kv_secret->secret_map.end())
                                 ? grant_type_val->second.ToString()
                                 : "";

    if (grant_type == "authorization_code" ||
        (refresh_token_val != kv_secret->secret_map.end() && !refresh_token_val->second.ToString().empty())) {
        if (refresh_token_val == kv_secret->secret_map.end() || refresh_token_val->second.ToString().empty()) {
            throw duckdb::InvalidInputException(
                "Delegated Microsoft Entra token is expired and no refresh_token is available. Recreate the secret with offline_access scope.");
        }

        auto refreshed = AcquireTokenWithRefreshToken(
            tenant_id, client_id, client_secret, scope, refresh_token_val->second.ToString());
        if (refreshed.access_token.empty()) {
            throw duckdb::IOException("Microsoft Entra refresh token response did not contain an access token");
        }

        const std::string updated_refresh = refreshed.refresh_token.empty()
                                                ? refresh_token_val->second.ToString()
                                                : refreshed.refresh_token;
        if (refreshed.expires_in > 0) {
            UpdateSecretWithTokens(context, kv_secret, refreshed.access_token, refreshed.expires_in, updated_refresh);
        }
        return refreshed.access_token;
    }

    if (client_secret.empty()) {
        throw duckdb::InvalidInputException("'client_secret' not found in Microsoft Entra secret");
    }

    // Acquire token using client credentials flow
    std::string access_token = AcquireTokenWithClientCredentials(tenant_id, client_id, client_secret, scope);

    // Note: Token is acquired but not stored in secret for now (stateless approach)
    // In production, you'd want to call UpdateSecretWithTokens to persist the token

    return access_token;
}

void MicrosoftEntraTokenManager::RefreshTokens(duckdb::ClientContext &context, const duckdb::KeyValueSecret *kv_secret) {
    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Refreshing Microsoft Entra tokens");

    // For client_credentials flow, we just acquire a new token (no refresh token)
    GetToken(context, kv_secret);
}

bool MicrosoftEntraTokenManager::IsTokenValid(const duckdb::KeyValueSecret *kv_secret) {
    return HasValidCachedToken(kv_secret);
}

std::string MicrosoftEntraTokenManager::GetTokenUrl(const std::string &tenant_id) {
    return "https://login.microsoftonline.com/" + tenant_id + "/oauth2/v2.0/token";
}

std::string MicrosoftEntraTokenManager::GetAuthorizationUrl(const std::string &tenant_id) {
    return "https://login.microsoftonline.com/" + tenant_id + "/oauth2/v2.0/authorize";
}

std::string MicrosoftEntraTokenManager::AcquireTokenWithClientCredentials(
    const std::string &tenant_id,
    const std::string &client_id,
    const std::string &client_secret,
    const std::string &scope) {

    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Acquiring token with client credentials for tenant: " + tenant_id);

    std::string token_url = GetTokenUrl(tenant_id);

    // Build the request body
    std::string body = "grant_type=client_credentials";
    body += "&client_id=" + UrlEncode(client_id);
    body += "&client_secret=" + UrlEncode(client_secret);
    body += "&scope=" + UrlEncode(scope);

    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Token URL: " + token_url);

    HttpRequest request(HttpMethod::POST, token_url, "application/x-www-form-urlencoded", body);
    request.headers.emplace("Accept", "application/json");

    HttpClient http;
    auto resp = http.SendRequest(request);

    if (!resp) {
        ERPL_TRACE_ERROR("MS_ENTRA_TOKEN", "No response from token endpoint");
        throw duckdb::IOException("No response from Microsoft Entra token endpoint");
    }

    if (resp->Code() != 200) {
        ERPL_TRACE_ERROR("MS_ENTRA_TOKEN", "Token endpoint returned HTTP " + std::to_string(resp->Code()));
        std::string error_msg = "Microsoft Entra token endpoint returned HTTP " + std::to_string(resp->Code());

        // Try to parse error details from response
        auto content = resp->Content();
        if (!content.empty()) {
            auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
            if (doc) {
                auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
                auto error_desc = duckdb_yyjson::yyjson_obj_get(root, "error_description");
                if (error_desc && duckdb_yyjson::yyjson_is_str(error_desc)) {
                    error_msg += ": " + std::string(duckdb_yyjson::yyjson_get_str(error_desc));
                }
                duckdb_yyjson::yyjson_doc_free(doc);
            }
        }

        throw duckdb::IOException(error_msg);
    }

    // Parse the token response
    auto content = resp->Content();
    auto doc = duckdb_yyjson::yyjson_read(content.c_str(), content.size(), 0);
    if (!doc) {
        ERPL_TRACE_ERROR("MS_ENTRA_TOKEN", "Failed to parse token response JSON");
        throw duckdb::IOException("Failed to parse Microsoft Entra token response");
    }

    auto root = duckdb_yyjson::yyjson_doc_get_root(doc);
    auto access_token_json = duckdb_yyjson::yyjson_obj_get(root, "access_token");

    if (!access_token_json || !duckdb_yyjson::yyjson_is_str(access_token_json)) {
        duckdb_yyjson::yyjson_doc_free(doc);
        ERPL_TRACE_ERROR("MS_ENTRA_TOKEN", "access_token missing in token response");
        throw duckdb::IOException("access_token missing in Microsoft Entra token response");
    }

    std::string access_token = duckdb_yyjson::yyjson_get_str(access_token_json);
    duckdb_yyjson::yyjson_doc_free(doc);

    ERPL_TRACE_INFO("MS_ENTRA_TOKEN", "Successfully acquired Microsoft Entra token");
    return access_token;
}

bool MicrosoftEntraTokenManager::HasValidCachedToken(const duckdb::KeyValueSecret *kv_secret) {
    // Check if token exists
    auto token_val = kv_secret->secret_map.find("access_token");
    if (token_val == kv_secret->secret_map.end()) {
        return false;
    }

    auto token_str = token_val->second.ToString();
    if (token_str.empty()) {
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
        // Add 5-minute buffer
        return expiration_time > (current_time + 300);
    } catch (...) {
        return false;
    }
}

std::string MicrosoftEntraTokenManager::GetCachedToken(const duckdb::KeyValueSecret *kv_secret) {
    auto token_val = kv_secret->secret_map.find("access_token");
    if (token_val == kv_secret->secret_map.end()) {
        throw duckdb::InvalidInputException("'access_token' not found in Microsoft Entra secret");
    }
    return token_val->second.ToString();
}

void MicrosoftEntraTokenManager::UpdateSecretWithTokens(
    duckdb::ClientContext &context,
    const duckdb::KeyValueSecret *kv_secret,
    const std::string &access_token,
    int expires_in,
    const std::string &refresh_token) {

    ERPL_TRACE_DEBUG("MS_ENTRA_TOKEN", "Updating secret with new token");

    // Get the secret manager
    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto secret_name = duckdb_compat::Str(kv_secret->GetName());
    if (secret_name.empty()) {
        ERPL_TRACE_WARN("MS_ENTRA_TOKEN", "Secret has no name; refreshed token will not be persisted");
        return;
    }

    // Get the current secret to preserve metadata
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto old_secret = secret_manager.GetSecretByName(transaction, secret_name);
    if (!old_secret) {
        ERPL_TRACE_WARN("MS_ENTRA_TOKEN", "Secret not found while persisting refreshed token: " + secret_name);
        return;
    }
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

    // Add the new token
    updated_secret.secret_map["access_token"] = duckdb::Value(access_token);
    if (!refresh_token.empty()) {
        updated_secret.secret_map["refresh_token"] = duckdb::Value(refresh_token);
    }

    // Calculate expiration time
    std::time_t expires_at = std::time(nullptr) + expires_in;
    updated_secret.secret_map["expires_at"] = duckdb::Value(std::to_string(expires_at));

    // Register the updated secret
    secret_manager.RegisterSecret(transaction, duckdb::make_uniq<duckdb::KeyValueSecret>(updated_secret),
                                  duckdb::OnCreateConflict::REPLACE_ON_CONFLICT, persist_type, storage_mode);

    ERPL_TRACE_INFO("MS_ENTRA_TOKEN", "Successfully updated secret with new token");
}

// Unified secret helpers
duckdb::unique_ptr<duckdb::KeyValueSecret> GetMicrosoftEntraKeyValueSecret(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    auto &secret_manager = duckdb::SecretManager::Get(context);
    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);

    if (!secret_entry) {
        throw duckdb::InvalidInputException("Microsoft Entra secret '" + secret_name + "' not found. Use CREATE SECRET to create it.");
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (!kv_secret) {
        throw duckdb::InvalidInputException("Secret '" + secret_name + "' is not a KeyValueSecret");
    }

    // Clone to extend lifetime beyond SecretEntry unique_ptr
    return duckdb::make_uniq<duckdb::KeyValueSecret>(*kv_secret);
}

MicrosoftEntraAuthInfo ResolveMicrosoftEntraAuth(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    ERPL_TRACE_DEBUG("MS_ENTRA_AUTH", "Resolving Microsoft Entra authentication for secret: " + secret_name);

    auto kv_secret_up = GetMicrosoftEntraKeyValueSecret(context, secret_name);
    const auto *kv_secret = kv_secret_up.get();

    // Resolve tenant_id
    auto tenant_id_val = kv_secret->secret_map.find("tenant_id");
    std::string tenant_id = (tenant_id_val != kv_secret->secret_map.end())
                                ? tenant_id_val->second.ToString()
                                : "";

    // Get access token (handles caching and refresh automatically)
    std::string access_token = MicrosoftEntraTokenManager::GetToken(context, kv_secret);

    if (access_token.empty()) {
        throw duckdb::InvalidInputException("Microsoft Entra secret '" + secret_name + "' could not provide a valid access token.");
    }

    auto auth_params = std::make_shared<HttpAuthParams>();
    auth_params->bearer_token = access_token;

    ERPL_TRACE_INFO("MS_ENTRA_AUTH", "Successfully resolved Microsoft Entra authentication");
    return MicrosoftEntraAuthInfo{tenant_id, access_token, auth_params};
}

} // namespace erpl_web
