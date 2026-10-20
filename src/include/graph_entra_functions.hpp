#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace erpl_web {

// Microsoft Graph Entra ID Directory table functions
class GraphEntraFunctions {
public:
    static void Register(duckdb::ExtensionLoader &loader);

private:
    // graph_users(secret_name) - List users in directory
    static duckdb::unique_ptr<duckdb::FunctionData> UsersBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void UsersScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_groups(secret_name) - List groups in directory
    static duckdb::unique_ptr<duckdb::FunctionData> GroupsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void GroupsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_devices(secret_name) - List devices in directory
    static duckdb::unique_ptr<duckdb::FunctionData> DevicesBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void DevicesScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_signin_logs(secret_name) - List sign-in logs (requires Azure AD Premium)
    static duckdb::unique_ptr<duckdb::FunctionData> SignInLogsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void SignInLogsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);
};

} // namespace erpl_web
