#include "graph_teams_functions.hpp"
#include "graph_client.hpp"
#include "graph_teams_client.hpp"
#include "graph_excel_secret.hpp"
#include "graph_output_utils.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "yyjson.hpp"
#include "erpl_odata_banner.hpp"
#include "graph_json_scan.hpp"

using namespace duckdb;
using namespace duckdb_yyjson;

namespace erpl_web {

// =============================================================================
// Shared lazy-streaming bind data
//
// Bind stores parameters and first_url — no HTTP calls happen there.
// Scan fetches first_url on the first call, then follows @odata.nextLink pages
// transparently across vector boundaries. Peak memory is one page at a time.
// =============================================================================

struct TeamsBindData : public TableFunctionData {
    std::string secret_name;
    std::string user;
    std::shared_ptr<HttpAuthParams> auth_params;

    std::string first_url;
};

struct TeamChannelsBindData : public TeamsBindData {
    std::string team_id;
};

struct TeamMembersBindData : public TeamsBindData {
    std::string team_id;
};

struct ChannelMessagesBindData : public TeamsBindData {
    std::string team_id;
    std::string channel_id;
};

// =============================================================================
// Pagination helpers
// =============================================================================

static bool FetchTeamsPage(GraphPagedScanState &state, const TeamsBindData &bd,
                           const std::string &url) {
    const auto body = GraphClient(bd.auth_params, "GRAPH_TEAMS").GetServerSuppliedUrl(url, state.origin_url);
    return state.LoadPage(body);
}

static bool InitTeamsScan(GraphPagedScanState &state, const TeamsBindData &bd, DataChunk &output) {
    if (state.done) {
        output.SetCardinality(0);
        return false;
    }
    if (!state.initialized) {
        // The first URL's origin is what every later next link is measured against.
        state.origin_url = bd.first_url;
        if (!FetchTeamsPage(state, bd, bd.first_url)) {
            state.done = true;
            output.SetCardinality(0);
            return false;
        }
        state.initialized = true;
    }
    return true;
}

static yyjson_val *NextTeamsItem(GraphPagedScanState &state, const TeamsBindData &bd) {
    return state.NextItem([&](const std::string &url) { return FetchTeamsPage(state, bd, url); });
}

static std::string GetNamedStr(TableFunctionBindInput &input, const char *name) {
    auto it = input.named_parameters.find(name);
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        return it->second.GetValue<std::string>();
    }
    return {};
}

// =============================================================================
// graph_my_teams Implementation
// =============================================================================

unique_ptr<FunctionData> GraphTeamsFunctions::MyTeamsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    auto bind_data         = make_uniq<TeamsBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;
    bind_data->first_url   = GraphTeamsUrlBuilder::BuildMyTeamsUrl(bind_data->user);

    names        = {"id", "display_name", "description", "visibility", "web_url", "is_archived"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
    };
    return std::move(bind_data);
}

void GraphTeamsFunctions::MyTeamsScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<TeamsBindData>();
    if (!InitTeamsScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextTeamsItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "description"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "visibility"));
        SetStrCell(output.data[4], row, yyjson_obj_get(item, "webUrl"));
        SetBoolCell(output.data[5], row, yyjson_obj_get(item, "isArchived"));
        row++;
    }
    output.SetCardinality(row);
}

// =============================================================================
// graph_teams_channels Implementation
// =============================================================================

unique_ptr<FunctionData> GraphTeamsFunctions::TeamChannelsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.empty()) {
        throw BinderException("graph_teams_channels requires a team_id parameter");
    }

    auto bind_data         = make_uniq<TeamChannelsBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;

    // Accept team displayName or GUID
    const std::string team_param = input.inputs[0].GetValue<std::string>();
    GraphTeamsClient resolver(bind_data->auth_params);
    bind_data->team_id   = resolver.ResolveTeamId(team_param, bind_data->user);
    bind_data->first_url = GraphTeamsUrlBuilder::BuildTeamChannelsUrl(bind_data->team_id);

    names        = {"id", "display_name", "description", "membership_type", "created_datetime"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
    };
    return std::move(bind_data);
}

void GraphTeamsFunctions::TeamChannelsScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<TeamChannelsBindData>();
    if (!InitTeamsScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextTeamsItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "description"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "membershipType"));
        SetStrCell(output.data[4], row, yyjson_obj_get(item, "createdDateTime"));
        row++;
    }
    output.SetCardinality(row);
}

// =============================================================================
// graph_teams_members Implementation
// =============================================================================

unique_ptr<FunctionData> GraphTeamsFunctions::TeamMembersBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.empty()) {
        throw BinderException("graph_teams_members requires a team_id parameter");
    }

    auto bind_data         = make_uniq<TeamMembersBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;

    // Accept team displayName or GUID
    const std::string team_param = input.inputs[0].GetValue<std::string>();
    GraphTeamsClient resolver(bind_data->auth_params);
    bind_data->team_id   = resolver.ResolveTeamId(team_param, bind_data->user);
    bind_data->first_url = GraphTeamsUrlBuilder::BuildTeamMembersUrl(bind_data->team_id);

    names        = {"id", "user_id", "display_name", "email", "role"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
    };
    return std::move(bind_data);
}

void GraphTeamsFunctions::TeamMembersScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<TeamMembersBindData>();
    if (!InitTeamsScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextTeamsItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "userId"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "email"));

        // roles is an array; pick first element or default to "member"
        auto *roles_arr = yyjson_obj_get(item, "roles");
        if (roles_arr && yyjson_is_arr(roles_arr) && yyjson_arr_size(roles_arr) > 0) {
            auto *first_role = yyjson_arr_get_first(roles_arr);
            SetStrCell(output.data[4], row, first_role);
        } else {
            SetStrCellNN(output.data[4], row, "member");
        }
        row++;
    }
    output.SetCardinality(row);
}

// =============================================================================
// graph_channel_messages Implementation
// =============================================================================

unique_ptr<FunctionData> GraphTeamsFunctions::ChannelMessagesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names) {

    if (input.inputs.size() < 2) {
        throw BinderException("graph_channel_messages requires team_id and channel_id parameters");
    }

    auto bind_data         = make_uniq<ChannelMessagesBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;

    // Accept team displayName or GUID, then channel displayName or ID
    const std::string team_param    = input.inputs[0].GetValue<std::string>();
    const std::string channel_param = input.inputs[1].GetValue<std::string>();
    GraphTeamsClient resolver(bind_data->auth_params);
    bind_data->team_id    = resolver.ResolveTeamId(team_param, bind_data->user);
    bind_data->channel_id = resolver.ResolveChannelId(bind_data->team_id, channel_param);
    bind_data->first_url  = GraphTeamsUrlBuilder::BuildChannelMessagesUrl(
        bind_data->team_id, bind_data->channel_id);

    names        = {"id", "created_datetime", "from_name", "body_content", "importance", "message_type"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
    };
    return std::move(bind_data);
}

void GraphTeamsFunctions::ChannelMessagesScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output) {

    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<ChannelMessagesBindData>();
    if (!InitTeamsScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextTeamsItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "createdDateTime"));

        auto *from_obj = yyjson_obj_get(item, "from");
        auto *user_obj = from_obj ? yyjson_obj_get(from_obj, "user") : nullptr;
        SetStrCell(output.data[2], row, user_obj ? yyjson_obj_get(user_obj, "displayName") : nullptr);

        auto *body_obj = yyjson_obj_get(item, "body");
        SetStrCell(output.data[3], row, body_obj ? yyjson_obj_get(body_obj, "content") : nullptr);

        SetStrCell(output.data[4], row, yyjson_obj_get(item, "importance"));
        SetStrCell(output.data[5], row, yyjson_obj_get(item, "messageType"));
        row++;
    }
    output.SetCardinality(row);
}

// =============================================================================
// Registration
// =============================================================================

void GraphTeamsFunctions::Register(ExtensionLoader &loader) {
    {
        TableFunction my_teams_func("graph_my_teams", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, MyTeamsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, MyTeamsBind));
        my_teams_func.named_parameters["user"]   = LogicalType::VARCHAR;
        my_teams_func.named_parameters["secret"] = LogicalType::VARCHAR;
        my_teams_func.init_global = GraphPagedScanState::Init;
        CreateTableFunctionInfo info(my_teams_func);
        FunctionDescription desc;
        desc.description = "List Microsoft Teams for a user. Omit user to query the authenticated "
                           "user's teams (delegated); provide user (GUID, UPN, or email) for app-only auth.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples = {
            "SELECT * FROM graph_my_teams(secret := 'ms_graph')",
            "SELECT * FROM graph_my_teams(user := 'jr@data-zoo.de', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "teams"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction team_channels_func("graph_teams_channels", {LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, TeamChannelsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, TeamChannelsBind));
        team_channels_func.named_parameters["secret"] = LogicalType::VARCHAR;
        team_channels_func.named_parameters["user"]   = LogicalType::VARCHAR;
        team_channels_func.init_global = GraphPagedScanState::Init;
        CreateTableFunctionInfo info(team_channels_func);
        FunctionDescription desc;
        desc.description = "List all channels in a Microsoft Teams team. "
                           "The first argument accepts a team GUID or a displayName (resolved via /me/joinedTeams).";
        desc.parameter_names = {"team_id_or_name"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_teams_channels('Engineering', secret := 'ms_graph')",
            "SELECT * FROM graph_teams_channels('xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "teams"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction team_members_func("graph_teams_members", {LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, TeamMembersScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, TeamMembersBind));
        team_members_func.named_parameters["secret"] = LogicalType::VARCHAR;
        team_members_func.named_parameters["user"]   = LogicalType::VARCHAR;
        team_members_func.init_global = GraphPagedScanState::Init;
        CreateTableFunctionInfo info(team_members_func);
        FunctionDescription desc;
        desc.description = "List all members of a Microsoft Teams team. "
                           "The first argument accepts a team GUID or a displayName (resolved via /me/joinedTeams).";
        desc.parameter_names = {"team_id_or_name"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_teams_members('Engineering', secret := 'ms_graph')",
            "SELECT * FROM graph_teams_members('xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "teams"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction channel_messages_func("graph_channel_messages",
            {LogicalType::VARCHAR, LogicalType::VARCHAR},
            DATAZOO_GUARD(ERPL_ODATA_BANNER, ChannelMessagesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ChannelMessagesBind));
        channel_messages_func.named_parameters["secret"] = LogicalType::VARCHAR;
        channel_messages_func.named_parameters["user"]   = LogicalType::VARCHAR;
        channel_messages_func.init_global = GraphPagedScanState::Init;
        CreateTableFunctionInfo info(channel_messages_func);
        FunctionDescription desc;
        desc.description = "List messages in a Microsoft Teams channel. Pages lazily through "
                           "large message histories via @odata.nextLink. "
                           "Both arguments accept a GUID/channel-ID or a human-readable displayName.";
        desc.parameter_names = {"team_id_or_name", "channel_id_or_name"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {
            "SELECT * FROM graph_channel_messages('Engineering', 'General', secret := 'ms_graph')",
            "SELECT * FROM graph_channel_messages('team-guid', '19:xxx@thread.tacv2', secret := 'ms_graph')"
        };
        desc.categories = {"microsoft", "graph", "teams"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
}

} // namespace erpl_web
