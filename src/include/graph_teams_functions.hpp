#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace erpl_web {

// Microsoft Graph Teams table functions
class GraphTeamsFunctions {
public:
    static void Register(duckdb::ExtensionLoader &loader);

private:
    // graph_my_teams(secret_name) - List teams the user is a member of
    static duckdb::unique_ptr<duckdb::FunctionData> MyTeamsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void MyTeamsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_teams_channels(secret_name, team_id) - List channels in a team
    static duckdb::unique_ptr<duckdb::FunctionData> TeamChannelsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void TeamChannelsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_teams_members(secret_name, team_id) - List members of a team
    static duckdb::unique_ptr<duckdb::FunctionData> TeamMembersBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void TeamMembersScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_channel_messages(secret_name, team_id, channel_id) - List messages in a channel
    static duckdb::unique_ptr<duckdb::FunctionData> ChannelMessagesBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void ChannelMessagesScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);
};

} // namespace erpl_web
