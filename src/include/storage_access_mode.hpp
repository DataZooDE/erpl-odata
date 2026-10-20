#pragma once

#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"

namespace erpl_web {

inline bool IsAccessModeOption(const std::string &key) {
    // DuckDB's ACCESS_MODE_OPTIONS table accepts these four spellings. Keep
    // attach option validation aligned with the explicit-mode scan below.
    return key == "read_only" || key == "readonly" || key == "read_write" || key == "readwrite";
}

inline void EnforceReadOnlyAttachment(duckdb::AttachedDatabase &db, duckdb::AttachInfo &info,
                                      duckdb::AttachOptions &options, const char *error) {
    // On 2.0 an explicit storage TYPE no longer receives an automatic remote-file
    // READ_ONLY mode. Distinguish an explicit write request from that default.
    for (const auto &entry : info.options) {
        const auto key = duckdb::StringUtil::Lower(entry.first);
        const bool read_only_spelling = key == "read_only" || key == "readonly";
        const bool read_write_spelling = key == "read_write" || key == "readwrite";
        if ((read_only_spelling || read_write_spelling) &&
            entry.second.GetValue<bool>() == read_write_spelling) {
            throw duckdb::BinderException(error);
        }
    }
    options.access_mode = duckdb::AccessMode::READ_ONLY;
    db.SetReadOnlyDatabase();
}

} // namespace erpl_web
