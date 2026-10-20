#include "sac_catalog_bind_helper.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace erpl_web {

std::string SacCatalogBindHelper::ExtractSecretName(const duckdb::TableFunctionBindInput &input) {
    return GetOptionalNamedString(input, "secret", "sac");
}

std::string SacCatalogBindHelper::ExtractPositionalString(
    const duckdb::TableFunctionBindInput &input,
    size_t index,
    const std::string &param_name) {

    if (input.inputs.size() <= index) {
        throw duckdb::InvalidInputException(
            "Missing required parameter '" + param_name + "' at position " + std::to_string(index));
    }

    return input.inputs[index].GetValue<std::string>();
}

SacSecretData SacCatalogBindHelper::ResolveSacCredentials(
    duckdb::ClientContext &context,
    const std::string &secret_name) {

    return ResolveSacSecretData(context, secret_name);
}

std::shared_ptr<SacCatalogService> SacCatalogBindHelper::CreateCatalogService(
    const SacSecretData &secret_data) {

    return std::make_shared<SacCatalogService>(
        secret_data.tenant,
        secret_data.region,
        secret_data.auth_params
    );
}

duckdb::vector<duckdb::LogicalType> SacCatalogBindHelper::CreateVarcharReturnTypes(size_t column_count) {
    duckdb::vector<duckdb::LogicalType> return_types;
    for (size_t i = 0; i < column_count; ++i) {
        return_types.push_back(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
    }
    return return_types;
}

std::string SacCatalogBindHelper::GetOptionalNamedString(
    const duckdb::TableFunctionBindInput &input,
    const std::string &param_name,
    const std::string &default_value) {

    if (const auto value = duckdb_compat::FindNamedParameter(input.named_parameters, param_name)) {
        return value->GetValue<std::string>();
    }

    return default_value;
}

} // namespace erpl_web
