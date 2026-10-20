#include "sac_secret.hpp"
#include "duckdb_compat.hpp"
#include "tracing.hpp"

#include "duckdb/common/exception.hpp"

namespace erpl_web {

namespace {

// Copies a named parameter into the secret map when the caller supplied it.
void CopyParameter(duckdb::CreateSecretInput &input, duckdb::KeyValueSecret &secret, const std::string &key) {
    auto value = input.options.find(key);
    if (value != input.options.end()) {
        secret.secret_map[duckdb_compat::Ident(key)] = value->second;
        ERPL_TRACE_DEBUG("SAC_SECRET", "Set parameter: " + key);
    }
}

// `tenant_name` and `region` are what SacUrlBuilder needs to address a tenant at all, so a
// secret without them cannot produce a URL. Rejecting them here gives the user the missing
// key by name, at CREATE SECRET time, rather than at the first read.
void RequireTenantCoordinates(const duckdb::KeyValueSecret &secret, const std::string &provider) {
    for (const char *key : {"tenant_name", "region"}) {
        if (secret.secret_map.find(key) == secret.secret_map.end()) {
            throw duckdb::InvalidInputException(
                "A SAC secret created with provider '%s' requires '%s'. SAC addresses a tenant as "
                "https://<tenant_name>.<region>.sapanalytics.cloud, so both are needed before any "
                "request can be built.",
                provider.c_str(), key);
        }
    }
}

} // namespace

void CreateSacSecretFunctions::Register(duckdb::ExtensionLoader &loader) {
    ERPL_TRACE_INFO("SAC_SECRET", "Registering SAC secret functions");

    const std::string type = "sac";

    duckdb::SecretType secret_type;
    secret_type.name = duckdb_compat::Ident(type);
    secret_type.deserializer = duckdb::KeyValueSecret::Deserialize<duckdb::KeyValueSecret>;
    secret_type.default_provider = "access_token";

    // access_token: the caller already holds a token and passes it in. This is the only
    // provider that can currently produce a usable SAC secret - see the note on the
    // client_credentials gap in ResolveSacSecretData.
    duckdb::CreateSecretFunction access_token_function = {type, "access_token", CreateFromAccessToken, {}};
    duckdb_compat::AddNamedParameter(access_token_function, "access_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(access_token_function);

    duckdb::CreateSecretFunction config_function = {type, "config", CreateFromConfig, {}};
    duckdb_compat::AddNamedParameter(config_function, "access_token", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_id", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "client_secret", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(config_function, "scope", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    RegisterCommonSecretParameters(config_function);

    loader.RegisterSecretType(secret_type);
    loader.RegisterFunction(access_token_function);
    loader.RegisterFunction(config_function);

    ERPL_TRACE_INFO("SAC_SECRET", "Successfully registered SAC secret functions");
}

void CreateSacSecretFunctions::RegisterCommonSecretParameters(duckdb::CreateSecretFunction &function) {
    duckdb_compat::AddNamedParameter(function, "tenant_name", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    duckdb_compat::AddNamedParameter(function, "region", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));

    // The loopback hatch, matching the one Datasphere already has: it is what makes an
    // end-to-end SAC test against a local server possible at all. Gated at use with
    // RequireGatedServiceUrl, so it cannot silently redirect a real tenant's token.
    duckdb_compat::AddNamedParameter(function, "base_url", duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateSacSecretFunctions::CreateFromAccessToken(
    duckdb::ClientContext &context, duckdb::CreateSecretInput &input) {
    ERPL_TRACE_DEBUG("SAC_SECRET", "Creating SAC secret with access_token provider");

    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(input.scope, input.type, input.provider, input.name);

    for (const char *key : {"access_token", "tenant_name", "region", "base_url"}) {
        CopyParameter(input, *result, key);
    }

    // base_url stands in for the tenant coordinates, and is the shape a local test server
    // takes, so it is accepted on its own.
    if (result->secret_map.find("base_url") == result->secret_map.end()) {
        RequireTenantCoordinates(*result, duckdb_compat::Str(input.provider));
    }

    if (result->secret_map.find("access_token") == result->secret_map.end()) {
        throw duckdb::InvalidInputException(
            "A SAC secret created with provider 'access_token' requires 'access_token'. Without it "
            "every SAC request would go out with no Authorization header.");
    }

    result->redact_keys.insert("access_token");
    return std::move(result);
}

duckdb::unique_ptr<duckdb::BaseSecret> CreateSacSecretFunctions::CreateFromConfig(
    duckdb::ClientContext &context, duckdb::CreateSecretInput &input) {
    ERPL_TRACE_DEBUG("SAC_SECRET", "Creating SAC secret with config provider");

    auto result = duckdb::make_uniq<duckdb::KeyValueSecret>(input.scope, input.type, input.provider, input.name);

    for (const char *key : {"access_token", "client_id", "client_secret", "scope", "tenant_name", "region",
                            "base_url"}) {
        CopyParameter(input, *result, key);
    }

    if (result->secret_map.find("base_url") == result->secret_map.end()) {
        RequireTenantCoordinates(*result, duckdb_compat::Str(input.provider));
    }

    result->redact_keys.insert("access_token");
    result->redact_keys.insert("client_secret");
    return std::move(result);
}

} // namespace erpl_web
