#include "graph_sharepoint_copy.hpp"
#include "duckdb_compat.hpp"
#include "graph_sharepoint_client.hpp"
#include "graph_excel_secret.hpp"
#include "graph_write_helpers.hpp"
#include "tracing.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/common/exception.hpp"
#include <mutex>
#include <string>
#include <vector>

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

struct SharePointListCopyBindData : public FunctionData {
    std::string site_id;
    std::string list_id;
    std::string secret_name;
    std::shared_ptr<HttpAuthParams> auth_params;
    std::vector<std::string> column_names;

    unique_ptr<FunctionData> Copy() const override {
        auto copy = make_uniq<SharePointListCopyBindData>();
        copy->site_id      = site_id;
        copy->list_id      = list_id;
        copy->secret_name  = secret_name;
        copy->auth_params  = auth_params;
        copy->column_names = column_names;
        return copy;
    }
    bool Equals(const FunctionData &other_p) const override {
        const auto &o = other_p.Cast<SharePointListCopyBindData>();
        return site_id == o.site_id && list_id == o.list_id;
    }
};

// ------------------------------------------------------------------
// Global state: row count and mutex for thread-safety
// ------------------------------------------------------------------

struct SharePointListCopyGlobalState : public GlobalFunctionData {
    std::mutex mutex;
    idx_t      row_count = 0;
};

// ------------------------------------------------------------------
// Helper: extract a single string option from COPY options map
// ------------------------------------------------------------------

static std::string GetCopyOpt(const duckdb_compat::CopyOptions &opts,
                               const std::string &key) {
    auto it = opts.find(duckdb_compat::Ident(key));
    if (it == opts.end() || it->second.empty()) { return ""; }
    return it->second[0].ToString();
}

// ------------------------------------------------------------------
// CopyFunction callbacks
// ------------------------------------------------------------------

static unique_ptr<FunctionData> SharePointListCopyBind(
    ClientContext &context,
    CopyFunctionBindInput &input,
    const duckdb_compat::CopyBindNames &names,
    const duckdb::vector<LogicalType> & /*sql_types*/)
{
    auto bind = make_uniq<SharePointListCopyBindData>();
    bind->column_names = duckdb_compat::Strs(names);

    const auto &opts   = input.info.options;
    bind->secret_name  = GetCopyOpt(opts, "secret");

    // The TO 'path' argument is the site name or URL
    const std::string site_raw = input.info.file_path;
    const std::string list_raw = GetCopyOpt(opts, "list");

    if (site_raw.empty()) {
        throw BinderException("FORMAT ms_sharepoint_list: specify the site as the COPY TO path");
    }
    if (list_raw.empty()) {
        throw BinderException("FORMAT ms_sharepoint_list requires the 'list' option");
    }

    auto auth_info   = ResolveGraphAuth(context, bind->secret_name);
    bind->auth_params = auth_info.auth_params;

    GraphSharePointClient sp_client(auth_info.auth_params);
    bind->site_id = sp_client.ResolveSiteId(site_raw);
    bind->list_id = sp_client.ResolveListId(bind->site_id, list_raw);

    return bind;
}

static unique_ptr<GlobalFunctionData> SharePointListCopyInitializeGlobal(
    ClientContext & /*context*/,
    FunctionData & /*bind_data_p*/,
    const std::string & /*file_path*/)
{
    return make_uniq<SharePointListCopyGlobalState>();
}

static unique_ptr<LocalFunctionData> SharePointListCopyInitializeLocal(
    ExecutionContext & /*context*/,
    FunctionData & /*bind_data_p*/)
{
    return make_uniq<LocalFunctionData>();
}

static void SharePointListCopySink(
    ExecutionContext & /*context*/,
    FunctionData &bind_data_p,
    GlobalFunctionData &gstate_p,
    LocalFunctionData & /*lstate*/,
    DataChunk &input)
{
    auto &bind   = bind_data_p.Cast<SharePointListCopyBindData>();
    auto &gstate = gstate_p.Cast<SharePointListCopyGlobalState>();

    input.Flatten();

    GraphSharePointClient sp_client(bind.auth_params);
    const std::vector<std::string> col_names(bind.column_names.begin(), bind.column_names.end());

    idx_t rows_written = 0;
    for (idx_t row = 0; row < input.size(); row++) {
        const std::string fields_json = DataChunkRowToSharePointFieldsJson(input, row, col_names);
        sp_client.CreateListItem(bind.site_id, bind.list_id, fields_json);
        rows_written++;
    }

    std::lock_guard<std::mutex> lock(gstate.mutex);
    gstate.row_count += rows_written;
}

static void SharePointListCopyCombine(
    ExecutionContext & /*context*/,
    FunctionData & /*bind_data*/,
    GlobalFunctionData & /*gstate*/,
    LocalFunctionData & /*lstate*/)
{
    // Sink sends each row and updates the synchronized global count directly.
}

static void SharePointListCopyFinalize(
    ClientContext & /*context*/,
    FunctionData &bind_data_p,
    GlobalFunctionData &gstate_p)
{
    const auto &bind   = bind_data_p.Cast<SharePointListCopyBindData>();
    const auto &gstate = gstate_p.Cast<SharePointListCopyGlobalState>();
    ERPL_TRACE_INFO("GRAPH_SHAREPOINT",
                    "Inserted " + std::to_string(gstate.row_count) +
                    " items into list '" + bind.list_id + "' on site '" + bind.site_id + "'");
}

// ------------------------------------------------------------------
// Registration
// ------------------------------------------------------------------

void RegisterSharePointListCopyFunction(duckdb::ExtensionLoader &loader) {
    CopyFunction fn("graph_sharepoint_list");
    fn.copy_to_bind              = SharePointListCopyBind;
    fn.copy_to_initialize_global = SharePointListCopyInitializeGlobal;
    fn.copy_to_initialize_local  = SharePointListCopyInitializeLocal;
    fn.copy_to_sink              = SharePointListCopySink;
    // DuckDB 2.0 requires a combine callback for parallel COPY even though
    // this writer accumulates no thread-local state to combine.
    fn.copy_to_combine           = SharePointListCopyCombine;
    fn.copy_to_finalize          = SharePointListCopyFinalize;

    loader.RegisterFunction(std::move(fn));
}

} // namespace erpl_web
