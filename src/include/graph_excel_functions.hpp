#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace erpl_web {

// Microsoft Graph Excel table functions
class GraphExcelFunctions {
public:
    static void Register(duckdb::ExtensionLoader &loader);

private:
    // graph_show_files(secret_name, folder_path) - List files in OneDrive
    static duckdb::unique_ptr<duckdb::FunctionData> ListFilesBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void ListFilesScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_excel_tables(secret_name, file_path) - List tables in Excel workbook
    static duckdb::unique_ptr<duckdb::FunctionData> ExcelTablesBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void ExcelTablesScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_excel_worksheets(secret_name, file_path) - List worksheets in Excel workbook
    static duckdb::unique_ptr<duckdb::FunctionData> ExcelWorksheetsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void ExcelWorksheetsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_excel_range(secret_name, file_path, sheet_name, range?) - Read Excel range
    static duckdb::unique_ptr<duckdb::FunctionData> ExcelRangeBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void ExcelRangeScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_excel_read(secret_name, file_path, table_name) - Read Excel table data
    static duckdb::unique_ptr<duckdb::FunctionData> ExcelTableDataBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void ExcelTableDataScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_excel_add_rows(file_path, table_name, data := '[[...]]', drive := '...', secret := '...')
    // Append pre-serialized rows to an Excel table. data must be a JSON 2-D array.
    static duckdb::unique_ptr<duckdb::FunctionData> AddRowsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void AddRowsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

    // graph_excel_delete_rows(file_path, table_name, col_index, col_value, drive := '...', secret := '...')
    // Delete all rows where the column at col_index (0-based) equals col_value.
    static duckdb::unique_ptr<duckdb::FunctionData> DeleteRowsBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    // graph_excel_delete_rows(file_path, table_name, column, col_value, ...) where column is a
    // column name. The name is resolved to a 0-based index against the table's header at scan time;
    // a purely numeric name falls back to being treated as the index itself.
    static duckdb::unique_ptr<duckdb::FunctionData> DeleteRowsByNameBind(
        duckdb::ClientContext &context,
        duckdb::TableFunctionBindInput &input,
        duckdb::vector<duckdb::LogicalType> &return_types,
        duckdb_compat::BindNames &names);

    static void DeleteRowsScan(
        duckdb::ClientContext &context,
        duckdb::TableFunctionInput &data,
        duckdb::DataChunk &output);

public:
    // Resolve a column reference to a 0-based index against an ordered list of column names.
    // Matches by exact name first, then case-insensitively; if no name matches and the reference
    // is a non-negative integer literal, it is used directly as the index. Throws otherwise.
    // Public so it can be unit-tested independently of any network I/O.
    static duckdb::idx_t ResolveColumnIndex(const std::vector<std::string> &columns,
                                            const std::string &column_ref);
};

} // namespace erpl_web
