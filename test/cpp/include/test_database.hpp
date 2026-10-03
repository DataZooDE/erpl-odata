#pragma once

#include "duckdb.hpp"

namespace erpl_web {
namespace test_support {

// An in-memory DuckDB with one connection.
//
// See CLAUDE.md: a bare DuckDB(nullptr) crashes in DEBUG builds shortly after the first query
// unless jemalloc's background threads own arena management. Every test that touches a
// DuckDB instance goes through this class so that cannot be forgotten.
class TestDatabase {
public:
    TestDatabase()
    {
        config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));
        database = duckdb::make_uniq<duckdb::DuckDB>(nullptr, &config);
        connection = duckdb::make_uniq<duckdb::Connection>(*database);
    }

    duckdb::Connection &Con() const { return *connection; }

private:
    duckdb::DBConfig config;
    duckdb::unique_ptr<duckdb::DuckDB> database;
    duckdb::unique_ptr<duckdb::Connection> connection;
};

}  // namespace test_support
}  // namespace erpl_web
