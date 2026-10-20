#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace erpl_web {

class GraphOutlookFunctions {
public:
    static void Register(duckdb::ExtensionLoader &loader);

private:
    // graph_calendars([user_id], [secret]) — list calendars
    static duckdb::unique_ptr<duckdb::FunctionData> CalendarsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);
    static void CalendarsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_calendar_events([user_id], [calendar_id], [start_date], [end_date], [secret])
    static duckdb::unique_ptr<duckdb::FunctionData> CalendarEventsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);
    static void CalendarEventsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_contacts([user_id], [secret])
    static duckdb::unique_ptr<duckdb::FunctionData> ContactsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);
    static void ContactsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_outlook_mail_folders([user_id], [secret])
    static duckdb::unique_ptr<duckdb::FunctionData> MailFoldersBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);
    static void MailFoldersScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_outlook_emails([user_id], [folder], [secret])
    static duckdb::unique_ptr<duckdb::FunctionData> MessagesBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);
    static void MessagesScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);
};

} // namespace erpl_web
