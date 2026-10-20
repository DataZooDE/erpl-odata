#include "graph_excel_copy.hpp"
#include "duckdb_compat.hpp"
#include "graph_excel_client.hpp"
#include "graph_excel_secret.hpp"
#include "graph_write_helpers.hpp"
#include "graph_sharepoint_client.hpp"
#include "tracing.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/common/exception.hpp"
#include <mutex>
#include <string>
#include <vector>
#include "odata_url_helpers.hpp"

namespace erpl_web {

using duckdb::BinderException;
using duckdb::CopyFunction;
using duckdb::CopyFunctionBindInput;
using duckdb::ClientContext;
using duckdb::ExecutionContext;
using duckdb::FunctionData;
using duckdb::GlobalFunctionData;
using duckdb::LocalFunctionData;
using duckdb::DataChunk;
using duckdb::LogicalType;
using duckdb::Value;
using duckdb::make_uniq;
using duckdb::unique_ptr;
using duckdb::idx_t;

// ------------------------------------------------------------------
// Bind data
// ------------------------------------------------------------------

struct ExcelTableCopyBindData : public FunctionData {
    std::string file_path;
    std::string table_name;
    std::string drive_id;
    std::string secret_name;
    std::shared_ptr<HttpAuthParams> auth_params;
    std::vector<std::string> column_names;

    unique_ptr<FunctionData> Copy() const override {
        auto copy = make_uniq<ExcelTableCopyBindData>();
        copy->file_path    = file_path;
        copy->table_name   = table_name;
        copy->drive_id     = drive_id;
        copy->secret_name  = secret_name;
        copy->auth_params  = auth_params;
        copy->column_names = column_names;
        return copy;
    }
    bool Equals(const FunctionData &other_p) const override {
        const auto &o = other_p.Cast<ExcelTableCopyBindData>();
        return file_path == o.file_path && table_name == o.table_name;
    }
};

// ------------------------------------------------------------------
// Global state: accumulates serialised row arrays across Sink calls
// ------------------------------------------------------------------

struct ExcelTableCopyGlobalState : public GlobalFunctionData {
    std::string row_arrays;  // comma-separated "[v1,v2,...]" strings
    std::mutex  mutex;
    idx_t       row_count = 0;
};

// ------------------------------------------------------------------
// Helper: extract a single string option from COPY options map
// ------------------------------------------------------------------

static std::string GetCopyOption(const duckdb_compat::CopyOptions &opts,
                                 const std::string &key) {
    auto it = opts.find(duckdb_compat::Ident(key));
    if (it == opts.end() || it->second.empty()) { return ""; }
    return it->second[0].ToString();
}

// ------------------------------------------------------------------
// Helper: resolve drive_id from COPY options (drive/site options)
// ------------------------------------------------------------------

static std::string ResolveDriveIdFromCopyOptions(
    const std::shared_ptr<HttpAuthParams> &auth_params,
    const duckdb_compat::CopyOptions &opts)
{
    const std::string drive_raw = GetCopyOption(opts, "drive");
    const std::string site_raw  = GetCopyOption(opts, "site");

    if (drive_raw.empty() && site_raw.empty()) { return ""; }

    GraphSharePointClient sp_client(auth_params);

    if (!drive_raw.empty()) {
        if (GraphSharePointClient::LooksLikeDriveId(drive_raw)) { return drive_raw; }
        if (LooksLikeAbsoluteHttpUrl(drive_raw)) {
            return sp_client.ResolveDriveIdFromUrl(drive_raw);
        }
    }

    // Name-based: resolve site first, then drive within it
    const std::string site_param  = !site_raw.empty() ? site_raw  : drive_raw;
    const std::string drive_param = !site_raw.empty() ? drive_raw : "";
    const std::string site_id = sp_client.ResolveSiteId(site_param);
    return sp_client.ResolveDriveId(site_id, drive_param);
}

// ------------------------------------------------------------------
// CopyFunction callbacks
// ------------------------------------------------------------------

static unique_ptr<FunctionData> ExcelTableCopyBind(
    ClientContext &context,
    CopyFunctionBindInput &input,
    const duckdb_compat::CopyBindNames &names,
    const duckdb::vector<LogicalType> & /*sql_types*/)
{
    auto bind = make_uniq<ExcelTableCopyBindData>();
    bind->file_path    = input.info.file_path;
    bind->column_names = duckdb_compat::Strs(names);

    const auto &opts = input.info.options;
    bind->table_name  = GetCopyOption(opts, "table");
    bind->secret_name = GetCopyOption(opts, "secret");

    if (bind->table_name.empty()) {
        throw BinderException("FORMAT ms_excel_table requires the 'table' option");
    }

    auto auth_info = ResolveGraphAuth(context, bind->secret_name);
    bind->auth_params = auth_info.auth_params;
    bind->drive_id    = ResolveDriveIdFromCopyOptions(auth_info.auth_params, opts);

    return bind;
}

static unique_ptr<GlobalFunctionData> ExcelTableCopyInitializeGlobal(
    ClientContext & /*context*/,
    FunctionData & /*bind_data_p*/,
    const std::string & /*file_path*/)
{
    return make_uniq<ExcelTableCopyGlobalState>();
}

static unique_ptr<LocalFunctionData> ExcelTableCopyInitializeLocal(
    ExecutionContext & /*context*/,
    FunctionData & /*bind_data_p*/)
{
    return make_uniq<LocalFunctionData>();
}

static void ExcelTableCopySink(
    ExecutionContext & /*context*/,
    FunctionData &bind_data_p,
    GlobalFunctionData &gstate_p,
    LocalFunctionData & /*lstate*/,
    DataChunk &input)
{
    auto &bind   = bind_data_p.Cast<ExcelTableCopyBindData>();
    auto &gstate = gstate_p.Cast<ExcelTableCopyGlobalState>();

    input.Flatten();

    const idx_t col_count = static_cast<idx_t>(bind.column_names.size());
    std::string local_rows;
    local_rows.reserve(input.size() * col_count * 12);

    for (idx_t row = 0; row < input.size(); row++) {
        if (!local_rows.empty()) { local_rows += ','; }
        local_rows += '[';
        for (idx_t col = 0; col < col_count; col++) {
            if (col > 0) { local_rows += ','; }
            local_rows += DuckDbValueToJsonLiteral(input.GetValue(col, row));
        }
        local_rows += ']';
    }

    std::lock_guard<std::mutex> lock(gstate.mutex);
    if (!gstate.row_arrays.empty() && !local_rows.empty()) {
        gstate.row_arrays += ',';
    }
    gstate.row_arrays += local_rows;
    gstate.row_count  += input.size();
}

static void ExcelTableCopyCombine(
    ExecutionContext & /*context*/,
    FunctionData & /*bind_data*/,
    GlobalFunctionData & /*gstate*/,
    LocalFunctionData & /*lstate*/)
{
    // Sink writes to the synchronized global state, so there is no local state to merge.
}

static void ExcelTableCopyFinalize(
    ClientContext & /*context*/,
    FunctionData &bind_data_p,
    GlobalFunctionData &gstate_p)
{
    auto &bind   = bind_data_p.Cast<ExcelTableCopyBindData>();
    auto &gstate = gstate_p.Cast<ExcelTableCopyGlobalState>();

    if (gstate.row_count == 0) { return; }

    ERPL_TRACE_INFO("GRAPH_EXCEL", "Appending " + std::to_string(gstate.row_count) +
                    " rows to table '" + bind.table_name + "' in '" + bind.file_path + "'");

    const std::string rows_json = "{\"values\":[" + gstate.row_arrays + "]}";
    GraphExcelClient client(bind.auth_params);
    client.AddTableRows(bind.file_path, bind.table_name, rows_json, bind.drive_id);
}

// ------------------------------------------------------------------
// Registration
// ------------------------------------------------------------------

void RegisterExcelTableCopyFunction(duckdb::ExtensionLoader &loader) {
    CopyFunction fn("graph_excel_table");
    fn.copy_to_bind              = ExcelTableCopyBind;
    fn.copy_to_initialize_global = ExcelTableCopyInitializeGlobal;
    fn.copy_to_initialize_local  = ExcelTableCopyInitializeLocal;
    fn.copy_to_sink              = ExcelTableCopySink;
    // DuckDB 2.0 requires a combine callback for parallel COPY even though
    // this writer accumulates no thread-local state to combine.
    fn.copy_to_combine           = ExcelTableCopyCombine;
    fn.copy_to_finalize          = ExcelTableCopyFinalize;
    fn.extension                 = "xlsx";

    loader.RegisterFunction(std::move(fn));
}

} // namespace erpl_web
