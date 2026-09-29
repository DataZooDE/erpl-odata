#include "graph_outlook_functions.hpp"
#include "graph_outlook_client.hpp"
#include "graph_client.hpp"
#include "graph_excel_secret.hpp"
#include "graph_output_utils.hpp"
#include "tracing.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "yyjson.hpp"
#include "erpl_odata_banner.hpp"
#include "graph_json_scan.hpp"

using namespace duckdb_yyjson;

namespace erpl_web {

using namespace duckdb;

// ============================================================================
// Shared bind-data base — lazy page streaming
//
// Bind stores parameters and computes first_url; no HTTP calls happen there.
// Scan fetches first_url on the first call, then follows @odata.nextLink pages
// transparently across vector boundaries. Peak memory is one page at a time.
// ============================================================================

struct OutlookBindData : public TableFunctionData {
    std::string secret_name;
    std::string user;
    std::shared_ptr<HttpAuthParams> auth_params;

    // first_url: set in Bind, fetched on the first Scan call of each execution.
    std::string first_url;
};

// Fetch a single page (no pagination loop) and load it into the scan state.
static bool FetchPage(GraphPagedScanState &state, const OutlookBindData &bd,
                      const std::string &url, const char *array_key = "value") {
    const auto body = GraphClient(bd.auth_params, "GRAPH_OUTLOOK").GetServerSuppliedUrl(url, state.origin_url);
    return state.LoadPage(body, array_key);
}

// On the first Scan call, fetch first_url and init the iterator.
// Returns false (and emits an empty chunk) if there is nothing to scan.
static bool InitScan(GraphPagedScanState &state, const OutlookBindData &bd, DataChunk &output,
                     const char *array_key = "value") {
    if (state.done) {
        output.SetCardinality(0);
        return false;
    }
    if (!state.initialized) {
        // The first URL's origin is what every later next link is measured against.
        state.origin_url = bd.first_url;
        if (!FetchPage(state, bd, bd.first_url, array_key)) {
            state.done = true;
            output.SetCardinality(0);
            return false;
        }
        state.initialized = true;
    }
    return true;
}

// Return the next item, transparently crossing page boundaries.
// Returns nullptr when all pages are exhausted.
static yyjson_val *NextItem(GraphPagedScanState &state, const OutlookBindData &bd,
                             const char *array_key = "value") {
    return state.NextItem([&](const std::string &url) { return FetchPage(state, bd, url, array_key); });
}

// Named-parameter helper
static std::string GetNamedStr(TableFunctionBindInput &input, const char *name) {
    auto it = input.named_parameters.find(name);
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        return it->second.GetValue<std::string>();
    }
    return {};
}

// Normalise a bare ISO date ("2024-06-15") to the format calendarView expects
// ("2024-06-15T00:00:00"). Full datetimes pass through unchanged.
static std::string NormaliseCalendarViewDate(const std::string &s) {
    if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
        return s + "T00:00:00";
    }
    return s;
}

// ============================================================================
// graph_calendars
// ============================================================================

unique_ptr<FunctionData> GraphOutlookFunctions::CalendarsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names)
{
    auto bind_data         = make_uniq<OutlookBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;
    bind_data->first_url   = GraphOutlookUrlBuilder::BuildCalendarsUrl(bind_data->user);

    names        = {"id", "name", "color", "is_default_calendar", "can_edit"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
    };
    return std::move(bind_data);
}

void GraphOutlookFunctions::CalendarsScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output)
{
    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<OutlookBindData>();
    if (!InitScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "name"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "color"));
        SetBoolCell(output.data[3], row, yyjson_obj_get(item, "isDefaultCalendar"));
        SetBoolCell(output.data[4], row, yyjson_obj_get(item, "canEdit"));
        row++;
    }
    output.SetCardinality(row);
}

// ============================================================================
// graph_calendar_events
// ============================================================================

struct CalendarEventsBindData : public OutlookBindData {
    std::string calendar_id;
    std::string start_date;
    std::string end_date;
};

unique_ptr<FunctionData> GraphOutlookFunctions::CalendarEventsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names)
{
    auto bind_data        = make_uniq<CalendarEventsBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    bind_data->calendar_id = GetNamedStr(input, "calendar_id");
    bind_data->start_date  = GetNamedStr(input, "start_date");
    bind_data->end_date    = GetNamedStr(input, "end_date");

    const bool has_start = !bind_data->start_date.empty();
    const bool has_end   = !bind_data->end_date.empty();
    if (has_start != has_end) {
        throw InvalidInputException(
            "graph_calendar_events: start_date and end_date must both be provided or both omitted");
    }

    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;

    if (has_start) {
        bind_data->first_url = GraphOutlookUrlBuilder::BuildCalendarViewUrl(
            bind_data->user,
            NormaliseCalendarViewDate(bind_data->start_date),
            NormaliseCalendarViewDate(bind_data->end_date));
    } else if (!bind_data->calendar_id.empty()) {
        bind_data->first_url = GraphOutlookUrlBuilder::BuildCalendarEventsUrl(
            bind_data->user, bind_data->calendar_id);
    } else {
        bind_data->first_url = GraphOutlookUrlBuilder::BuildEventsUrl(bind_data->user);
    }

    names        = {"id", "subject", "body_preview", "start_time", "end_time",
                    "location", "organizer_name", "organizer_email",
                    "is_all_day", "is_cancelled", "web_link"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
        LogicalType::VARCHAR,
    };
    return std::move(bind_data);
}

void GraphOutlookFunctions::CalendarEventsScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output)
{
    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<CalendarEventsBindData>();
    if (!InitScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "subject"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "bodyPreview"));

        auto *start_obj = yyjson_obj_get(item, "start");
        auto *end_obj   = yyjson_obj_get(item, "end");
        auto *loc_obj   = yyjson_obj_get(item, "location");
        SetStrCell(output.data[3], row, start_obj ? yyjson_obj_get(start_obj, "dateTime")    : nullptr);
        SetStrCell(output.data[4], row, end_obj   ? yyjson_obj_get(end_obj,   "dateTime")    : nullptr);
        SetStrCell(output.data[5], row, loc_obj   ? yyjson_obj_get(loc_obj,   "displayName") : nullptr);

        auto *organizer = yyjson_obj_get(item, "organizer");
        auto *org_email = organizer ? yyjson_obj_get(organizer, "emailAddress") : nullptr;
        SetStrCell(output.data[6], row, org_email ? yyjson_obj_get(org_email, "name")    : nullptr);
        SetStrCell(output.data[7], row, org_email ? yyjson_obj_get(org_email, "address") : nullptr);

        SetBoolCell(output.data[8],  row, yyjson_obj_get(item, "isAllDay"));
        SetBoolCell(output.data[9],  row, yyjson_obj_get(item, "isCancelled"));
        SetStrCell(output.data[10], row, yyjson_obj_get(item, "webLink"));
        row++;
    }
    output.SetCardinality(row);
}

// ============================================================================
// graph_contacts
// ============================================================================

unique_ptr<FunctionData> GraphOutlookFunctions::ContactsBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names)
{
    auto bind_data         = make_uniq<OutlookBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;
    bind_data->first_url   = GraphOutlookUrlBuilder::BuildContactsUrl(bind_data->user);

    names        = {"id", "display_name", "given_name", "surname", "email",
                    "mobile_phone", "business_phone", "company_name", "job_title"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
    };
    return std::move(bind_data);
}

void GraphOutlookFunctions::ContactsScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output)
{
    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<OutlookBindData>();
    if (!InitScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "givenName"));
        SetStrCell(output.data[3], row, yyjson_obj_get(item, "surname"));

        auto *emails_arr  = yyjson_obj_get(item, "emailAddresses");
        auto *first_email = emails_arr ? yyjson_arr_get_first(emails_arr) : nullptr;
        SetStrCell(output.data[4], row, first_email ? yyjson_obj_get(first_email, "address") : nullptr);

        SetStrCell(output.data[5], row, yyjson_obj_get(item, "mobilePhone"));

        auto *phones_arr  = yyjson_obj_get(item, "businessPhones");
        auto *first_phone = phones_arr ? yyjson_arr_get_first(phones_arr) : nullptr;
        SetStrCell(output.data[6], row, first_phone);

        SetStrCell(output.data[7], row, yyjson_obj_get(item, "companyName"));
        SetStrCell(output.data[8], row, yyjson_obj_get(item, "jobTitle"));
        row++;
    }
    output.SetCardinality(row);
}

// ============================================================================
// graph_outlook_mail_folders
// ============================================================================

unique_ptr<FunctionData> GraphOutlookFunctions::MailFoldersBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names)
{
    auto bind_data         = make_uniq<OutlookBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;
    bind_data->first_url   = GraphOutlookUrlBuilder::BuildMailFoldersUrl(bind_data->user);

    names        = {"id", "display_name", "parent_folder_id", "total_item_count", "unread_item_count"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::INTEGER,
        LogicalType::INTEGER,
    };
    return std::move(bind_data);
}

void GraphOutlookFunctions::MailFoldersScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output)
{
    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<OutlookBindData>();
    if (!InitScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "displayName"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "parentFolderId"));
        SetInt32Cell(output.data[3], row, yyjson_obj_get(item, "totalItemCount"));
        SetInt32Cell(output.data[4], row, yyjson_obj_get(item, "unreadItemCount"));
        row++;
    }
    output.SetCardinality(row);
}

// ============================================================================
// graph_outlook_emails
// ============================================================================

static bool IsWellKnownFolder(const std::string &name) {
    static const std::string known[] = {
        "inbox", "sentitems", "deleteditems", "drafts",
        "junkemail", "outbox", "archive", "clutter"
    };
    for (const auto &k : known) {
        if (name == k) {
            return true;
        }
    }
    return false;
}

struct MessagesBindData : public OutlookBindData {
    std::string folder;
    std::unordered_map<std::string, std::string> folder_name_by_id;
};

unique_ptr<FunctionData> GraphOutlookFunctions::MessagesBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<std::string> &names)
{
    auto bind_data         = make_uniq<MessagesBindData>();
    bind_data->secret_name = GetNamedStr(input, "secret");
    bind_data->user        = GetNamedStr(input, "user");
    bind_data->folder      = GetNamedStr(input, "folder");

    auto auth_info         = ResolveGraphAuth(context, bind_data->secret_name);
    bind_data->auth_params = auth_info.auth_params;

    // Compute first_url. Custom folder names require one API call to resolve
    // display name → folder id; well-known names and the default (all messages)
    // need no API call.
    if (bind_data->folder.empty()) {
        bind_data->first_url = GraphOutlookUrlBuilder::BuildMessagesUrl(bind_data->user);
    } else if (IsWellKnownFolder(bind_data->folder)) {
        bind_data->first_url = GraphOutlookUrlBuilder::BuildFolderMessagesUrl(
            bind_data->user, bind_data->folder);
    } else {
        // Resolve display name → folder id (one bounded API call).
        GraphOutlookClient client(auth_info.auth_params);
        const std::string folders_json = client.GetMailFolders(bind_data->user);
        yyjson_doc *doc  = yyjson_read(folders_json.c_str(), folders_json.size(), 0);
        if (!doc) {
            throw IOException("graph_outlook_emails: failed to parse mail folders response");
        }
        std::string folder_id;
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *arr  = yyjson_obj_get(root, "value");
        if (arr && yyjson_is_arr(arr)) {
            size_t idx, max;
            yyjson_val *folder;
            yyjson_arr_foreach(arr, idx, max, folder) {
                auto *name_val = yyjson_obj_get(folder, "displayName");
                auto *id_val   = yyjson_obj_get(folder, "id");
                if (name_val && yyjson_is_str(name_val) &&
                    id_val   && yyjson_is_str(id_val) &&
                    bind_data->folder == yyjson_get_str(name_val)) {
                    folder_id = yyjson_get_str(id_val);

                    // While we have the response, also populate the name map.
                    // Re-parse the whole array below to complete the map.
                    break;
                }
            }
        }
        yyjson_doc_free(doc);

        if (folder_id.empty()) {
            throw InvalidInputException(
                "graph_outlook_emails: no mail folder found with name '" + bind_data->folder + "'");
        }
        bind_data->first_url = GraphOutlookUrlBuilder::BuildFolderMessagesUrl(
            bind_data->user, folder_id);
    }

    // Build folder_id → display_name map (one bounded API call) for the
    // folder_name output column. The folder list is small so this is fast.
    {
        GraphOutlookClient client(auth_info.auth_params);
        const std::string folders_json = client.GetMailFolders(bind_data->user);
        yyjson_doc *fdoc = yyjson_read(folders_json.c_str(), folders_json.size(), 0);
        if (fdoc) {
            yyjson_val *froot = yyjson_doc_get_root(fdoc);
            yyjson_val *farr  = yyjson_obj_get(froot, "value");
            if (farr && yyjson_is_arr(farr)) {
                size_t fidx, fmax;
                yyjson_val *fitem;
                yyjson_arr_foreach(farr, fidx, fmax, fitem) {
                    auto *id_v   = yyjson_obj_get(fitem, "id");
                    auto *name_v = yyjson_obj_get(fitem, "displayName");
                    if (id_v && yyjson_is_str(id_v) && name_v && yyjson_is_str(name_v)) {
                        bind_data->folder_name_by_id[yyjson_get_str(id_v)] = yyjson_get_str(name_v);
                    }
                }
            }
            yyjson_doc_free(fdoc);
        }
    }

    names        = {"id", "subject", "body_preview", "from_name", "from_email",
                    "received_at", "has_attachments", "is_read", "importance", "web_link",
                    "folder_id", "folder_name"};
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
    };
    return std::move(bind_data);
}

void GraphOutlookFunctions::MessagesScan(
    ClientContext &,
    TableFunctionInput &data,
    DataChunk &output)
{
    auto &state = data.global_state->Cast<GraphPagedScanState>();
    auto &bd = data.bind_data->Cast<MessagesBindData>();
    if (!InitScan(state, bd, output)) {
        return;
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE) {
        auto *item = NextItem(state, bd);
        if (!item) {
            state.done = true;
            break;
        }
        SetStrCell(output.data[0], row, yyjson_obj_get(item, "id"));
        SetStrCell(output.data[1], row, yyjson_obj_get(item, "subject"));
        SetStrCell(output.data[2], row, yyjson_obj_get(item, "bodyPreview"));

        auto *from_obj   = yyjson_obj_get(item, "from");
        auto *from_email = from_obj ? yyjson_obj_get(from_obj, "emailAddress") : nullptr;
        SetStrCell(output.data[3], row, from_email ? yyjson_obj_get(from_email, "name")    : nullptr);
        SetStrCell(output.data[4], row, from_email ? yyjson_obj_get(from_email, "address") : nullptr);

        SetStrCell(output.data[5], row, yyjson_obj_get(item, "receivedDateTime"));
        SetBoolCell(output.data[6], row, yyjson_obj_get(item, "hasAttachments"));
        SetBoolCell(output.data[7], row, yyjson_obj_get(item, "isRead"));
        SetStrCell(output.data[8], row, yyjson_obj_get(item, "importance"));
        SetStrCell(output.data[9], row, yyjson_obj_get(item, "webLink"));
        SetStrCell(output.data[10], row, yyjson_obj_get(item, "parentFolderId"));

        auto *pid_val = yyjson_obj_get(item, "parentFolderId");
        if (pid_val && yyjson_is_str(pid_val)) {
            auto it = bd.folder_name_by_id.find(yyjson_get_str(pid_val));
            if (it != bd.folder_name_by_id.end()) {
                FlatVector::GetData<string_t>(output.data[11])[row] =
                    StringVector::AddString(output.data[11], it->second);
            } else {
                FlatVector::Validity(output.data[11]).SetInvalid(row);
            }
        } else {
            FlatVector::Validity(output.data[11]).SetInvalid(row);
        }
        row++;
    }
    output.SetCardinality(row);
}

// ============================================================================
// Registration
// ============================================================================

void GraphOutlookFunctions::Register(ExtensionLoader &loader) {
    ERPL_TRACE_INFO("GRAPH_OUTLOOK", "Registering Microsoft Graph Outlook functions");

    {
        TableFunction fn("graph_calendars", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, CalendarsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, CalendarsBind));
        fn.init_global = GraphPagedScanState::Init;
        fn.named_parameters["user"]    = LogicalType::VARCHAR;
        fn.named_parameters["secret"]  = LogicalType::VARCHAR;
        CreateTableFunctionInfo info(fn);
        FunctionDescription desc;
        desc.description = "List Outlook calendars for a user via Microsoft Graph. "
                           "Returns id, name, color, is_default_calendar, can_edit. "
                           "Omit user to query the authenticated user's calendars (delegated); "
                           "provide user (GUID, UPN, or email) for app-only auth.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples    = {
            "SELECT * FROM graph_calendars(secret := 'ms_graph')",
            "SELECT * FROM graph_calendars(user := 'jr@contoso.com', secret := 'ms_graph')"
        };
        desc.categories  = {"microsoft", "graph", "outlook"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction fn("graph_calendar_events", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, CalendarEventsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, CalendarEventsBind));
        fn.init_global = GraphPagedScanState::Init;
        fn.named_parameters["user"]        = LogicalType::VARCHAR;
        fn.named_parameters["calendar_id"] = LogicalType::VARCHAR;
        fn.named_parameters["start_date"]  = LogicalType::VARCHAR;
        fn.named_parameters["end_date"]    = LogicalType::VARCHAR;
        fn.named_parameters["secret"]      = LogicalType::VARCHAR;
        CreateTableFunctionInfo info(fn);
        FunctionDescription desc;
        desc.description = "List Outlook calendar events for a user via Microsoft Graph. "
                           "Returns id, subject, body_preview, start_time, end_time, location, "
                           "organizer_name, organizer_email, is_all_day, is_cancelled, web_link. "
                           "Use start_date + end_date (ISO 8601 date or datetime) for a date-bounded "
                           "calendarView query; omit both for all upcoming events. "
                           "Use calendar_id to target a specific calendar instead of the default.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples    = {
            "SELECT * FROM graph_calendar_events(secret := 'ms_graph')",
            "SELECT * FROM graph_calendar_events(start_date := '2024-01-01', end_date := '2024-12-31', secret := 'ms_graph')",
            "SELECT * FROM graph_calendar_events(user := 'jr@contoso.com', calendar_id := 'calendar-guid', secret := 'ms_graph')"
        };
        desc.categories  = {"microsoft", "graph", "outlook"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction fn("graph_contacts", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ContactsScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ContactsBind));
        fn.init_global = GraphPagedScanState::Init;
        fn.named_parameters["user"]    = LogicalType::VARCHAR;
        fn.named_parameters["secret"]  = LogicalType::VARCHAR;
        CreateTableFunctionInfo info(fn);
        FunctionDescription desc;
        desc.description = "List Outlook contacts for a user via Microsoft Graph. "
                           "Returns id, display_name, email, phone, company, job_title. "
                           "Omit user to query the authenticated user's contacts (delegated); "
                           "provide user (GUID, UPN, or email) for app-only auth.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples    = {
            "SELECT * FROM graph_contacts(secret := 'ms_graph')",
            "SELECT display_name, email FROM graph_contacts(user := 'jr@contoso.com', secret := 'ms_graph') WHERE company = 'Acme'"
        };
        desc.categories  = {"microsoft", "graph", "outlook"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction fn("graph_outlook_mail_folders", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, MailFoldersScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, MailFoldersBind));
        fn.init_global = GraphPagedScanState::Init;
        fn.named_parameters["user"]    = LogicalType::VARCHAR;
        fn.named_parameters["secret"]  = LogicalType::VARCHAR;
        CreateTableFunctionInfo info(fn);
        FunctionDescription desc;
        desc.description = "List Outlook mail folders for a user via Microsoft Graph. "
                           "Returns id, display_name, total_item_count, unread_item_count. "
                           "Use the returned display_name or id as the folder parameter of "
                           "graph_outlook_emails() to filter messages by folder.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples    = {
            "SELECT * FROM graph_outlook_mail_folders(secret := 'ms_graph')",
            "SELECT display_name, unread_item_count FROM graph_outlook_mail_folders(user := 'jr@contoso.com', secret := 'ms_graph') ORDER BY unread_item_count DESC"
        };
        desc.categories  = {"microsoft", "graph", "outlook"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }
    {
        TableFunction fn("graph_outlook_emails", {}, DATAZOO_GUARD(ERPL_ODATA_BANNER, MessagesScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, MessagesBind));
        fn.init_global = GraphPagedScanState::Init;
        fn.named_parameters["user"]    = LogicalType::VARCHAR;
        fn.named_parameters["folder"]  = LogicalType::VARCHAR;
        fn.named_parameters["secret"]  = LogicalType::VARCHAR;
        CreateTableFunctionInfo info(fn);
        FunctionDescription desc;
        desc.description = "List Outlook email messages for a user via Microsoft Graph. "
                           "Returns id, subject, sender_name, sender_email, received_datetime, "
                           "body_preview, is_read, has_attachments, web_link. "
                           "Use folder to filter by a well-known name ('inbox', 'sentitems', "
                           "'drafts', 'deleteditems') or any displayName from "
                           "graph_outlook_mail_folders(). Pages lazily through large mailboxes.";
        desc.parameter_names = {};
        desc.parameter_types = {};
        desc.examples    = {
            "SELECT * FROM graph_outlook_emails(secret := 'ms_graph')",
            "SELECT subject, sender_name, received_datetime FROM graph_outlook_emails(folder := 'inbox', secret := 'ms_graph') WHERE NOT is_read",
            "SELECT * FROM graph_outlook_emails(user := 'jr@contoso.com', folder := 'sentitems', secret := 'ms_graph')"
        };
        desc.categories  = {"microsoft", "graph", "outlook"};
        info.descriptions.push_back(std::move(desc));
        loader.RegisterFunction(std::move(info));
    }

    ERPL_TRACE_INFO("GRAPH_OUTLOOK", "Successfully registered Microsoft Graph Outlook functions");
}

} // namespace erpl_web
