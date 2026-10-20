// GitHub #210: restores the delta_share_scan data-path coverage that the https-only URL
// policy took away, and adds the projection coverage the #209 review found missing.
//
// The scan refuses any file URL that is not https (or http on loopback), because the URL
// comes from the share server and parquet_scan resolves whatever it is handed. Reading a
// remote parquet file needs httpfs, which is not built here, so the reader could not be
// exercised end to end at all - leaving the row-draining and chunk-lifetime fixes in #207
// with no regression cover, and the schema alignment with none ever.
//
// DeltaShareFileReader is driven directly instead, with a local parquet file. That does
// not weaken the policy: the policy is enforced in DeltaShareScanInitGlobal, where the
// share server's file list arrives, and every URL reaching the reader in production has
// passed it. test_delta_share_scan_data.cpp covers the policy itself.

#include "catch.hpp"
#include "duckdb_compat.hpp"
#include "duckdb.hpp"

#include "delta_share_scan.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// See CLAUDE.md: a bare DuckDB(nullptr) crashes in DEBUG builds shortly after the first
// query unless jemalloc's background threads own arena management.
class TestDatabase {
public:
    TestDatabase()
    {
        config.SetOption("allocator_background_threads", duckdb::Value::BOOLEAN(true));
        database = duckdb::make_uniq<duckdb::DuckDB>(nullptr, &config);
        connection = duckdb::make_uniq<duckdb::Connection>(*database);
    }

    duckdb::Connection &Con() const { return *connection; }
    duckdb::ClientContext &Context() const { return *connection->context; }

private:
    duckdb::DBConfig config;
    duckdb::unique_ptr<duckdb::DuckDB> database;
    duckdb::unique_ptr<duckdb::Connection> connection;
};

std::string UniqueSuffix()
{
    static std::atomic<unsigned> counter{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<long long>(ticks)) + "_" + std::to_string(counter.fetch_add(1));
}

class ScratchParquet {
public:
    ScratchParquet() : path("erpl_reader_" + UniqueSuffix() + ".parquet") {}
    ~ScratchParquet() { std::remove(path.c_str()); }

    // Writes `select_list` FROM range(rows) to the file.
    void Write(duckdb::Connection &con, const std::string &select_list, int rows)
    {
        auto result = con.Query("COPY (SELECT " + select_list + " FROM range(0, " +
                                std::to_string(rows) + ") t(i)) TO '" + path +
                                "' (FORMAT PARQUET)");
        INFO((result->HasError() ? result->GetError() : std::string()));
        REQUIRE_FALSE(result->HasError());
    }

    const std::string &Path() const { return path; }

private:
    std::string path;
};

// Drains a reader completely, returning every row's first column as a string plus the
// number of chunks seen.
struct DrainResult {
    std::vector<std::string> first_column;
    int chunks = 0;
};

DrainResult Drain(erpl_web::DeltaShareFileReader &reader)
{
    DrainResult drained;
    while (auto *chunk = reader.NextChunk()) {
        drained.chunks++;
        for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
            drained.first_column.push_back(duckdb_compat::ResultValue(*chunk, 0, row).ToString());
        }
    }
    return drained;
}

// Built inside functions, not as namespace-scope constants: odr-using
// LogicalType::INTEGER at namespace scope makes mold report it as duplicated against
// libduckdb_static.a.
duckdb::vector<duckdb::string> IdName() { return {"id", "name"}; }

duckdb::vector<duckdb::LogicalType> IntVarchar()
{
    return {duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER),
            duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};
}

}  // namespace

// #207 defect 2 and 3: one file larger than a single output chunk. The pre-fix scan
// emitted the first chunk and moved to the next file, returning 2048 of 5000 rows out of
// freed memory.
TEST_CASE("DeltaShareFileReader drains every chunk of a file", "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id, 'row-' || i AS name", 5000);

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());
    REQUIRE(reader.IsOpen());

    const auto drained = Drain(reader);
    REQUIRE(drained.first_column.size() == 5000);
    // More than one chunk, or the test would not distinguish the defect at all.
    REQUIRE(drained.chunks > 1);
    // Every row, in order, and read from live memory.
    REQUIRE(drained.first_column.front() == "0");
    REQUIRE(drained.first_column.back() == "4999");
}

TEST_CASE("DeltaShareFileReader reports exhaustion and can be reopened",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id, 'row-' || i AS name", 10);

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());
    REQUIRE(Drain(reader).first_column.size() == 10);
    REQUIRE(reader.NextChunk() == nullptr);

    // The scan reuses one reader across files, so reopening has to start clean.
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());
    REQUIRE(Drain(reader).first_column.size() == 10);
}

// #209 review, F2: the first alignment mapped columns POSITIONALLY, so a file whose column
// order differs from the share-declared schema returned one column's values under another
// column's name - silently.
TEST_CASE("DeltaShareFileReader aligns columns by name, not position",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    // File order is (name, id) - the reverse of the declared schema.
    parquet.Write(database.Con(), "'row-' || i AS name, CAST(i AS INTEGER) AS id", 5);

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->size() == 5);
    // Column 0 is 'id' because the schema says so, whatever order the file used.
    REQUIRE(duckdb_compat::ResultValue(*chunk, 0, 0).ToString() == "0");
    REQUIRE(duckdb_compat::ResultValue(*chunk, 1, 0).ToString() == "row-0");
    REQUIRE(chunk->data[0].GetType() == duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER));
}

// #209 review, F3: a column the file does not carry is what schema evolution looks like -
// a hard failure there rejects a legitimate table.
TEST_CASE("DeltaShareFileReader fills NULL for a column absent from the file",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id", 4);  // no 'name' column

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->size() == 4);
    REQUIRE(duckdb_compat::ResultValue(*chunk, 0, 0).ToString() == "0");
    REQUIRE(duckdb_compat::ResultValue(*chunk, 1, 0).IsNull());
}

// The file's types need not match the declared schema; the share server supplies both.
// Referencing a mismatched vector into the output raised a fatal INTERNAL error.
TEST_CASE("DeltaShareFileReader casts the file's types to the declared schema",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    // id is BIGINT in the file, INTEGER in the schema.
    parquet.Write(database.Con(), "i AS id, 'row-' || i AS name", 3);

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->data[0].GetType() == duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER));
    REQUIRE(duckdb_compat::ResultValue(*chunk, 0, 2).ToString() == "2");
}

// Case-insensitive fallback: a file differing only in capitalisation still lines up rather
// than silently becoming all NULLs.
TEST_CASE("DeltaShareFileReader matches column names case-insensitively",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS ID, 'row-' || i AS NAME", 3);

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), IdName(), IntVarchar());

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE_FALSE(duckdb_compat::ResultValue(*chunk, 0, 0).IsNull());
    REQUIRE(duckdb_compat::ResultValue(*chunk, 1, 0).ToString() == "row-0");
}

// In Delta, a partition column's value is NOT stored in the data file - it is carried per
// file in the protocol. Treating its absence as schema evolution returned every partition
// column as NULL, so "SELECT dt, count(*) ... GROUP BY dt" collapsed to one NULL group:
// wrong data, no error.
TEST_CASE("DeltaShareFileReader takes partition columns from the file's partition values",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    // The file carries only id and name; dt exists solely as a partition value.
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id, 'row-' || i AS name", 3);

    duckdb::vector<duckdb::string> names = {"id", "name", "dt"};
    duckdb::vector<duckdb::LogicalType> types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
        duckdb::LogicalType(duckdb::LogicalTypeId::DATE)};

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), names, types, {{"dt", "2024-03-01"}});

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->size() == 3);
    // The partition value reaches every row, cast to the bound type rather than left a string.
    REQUIRE(chunk->data[2].GetType() == duckdb::LogicalType(duckdb::LogicalTypeId::DATE));
    for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
        INFO("row " << row);
        REQUIRE_FALSE(duckdb_compat::ResultValue(*chunk, 2, row).IsNull());
        REQUIRE(duckdb_compat::ResultValue(*chunk, 2, row).ToString() == "2024-03-01");
    }
}

TEST_CASE("DeltaShareFileReader still NULLs a column in neither the file nor the partitions",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id", 2);

    duckdb::vector<duckdb::string> names = {"id", "name", "dt"};
    duckdb::vector<duckdb::LogicalType> types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), names, types, {{"dt", "2024-03-01"}});

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(duckdb_compat::ResultValue(*chunk, 2, 0).ToString() == "2024-03-01");  // partition
    REQUIRE(duckdb_compat::ResultValue(*chunk, 1, 0).IsNull());                    // genuinely absent
}

// Delta Sharing PROTOCOL.md, Partition Value Serialization: "An empty string for any type
// translates to a null partition value." Emitting it literally made '' fail the cast on a
// DATE column - taking the whole scan with it - and produced '' instead of NULL on VARCHAR.
TEST_CASE("DeltaShareFileReader reads an empty partition value as NULL",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id", 2);

    duckdb::vector<duckdb::string> names = {"id", "dt", "region"};
    duckdb::vector<duckdb::LogicalType> types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER),
        duckdb::LogicalType(duckdb::LogicalTypeId::DATE),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};

    erpl_web::DeltaShareFileReader reader;
    // A DATE would throw on CAST('' AS DATE); a VARCHAR would quietly become ''.
    reader.Open(database.Context(), parquet.Path(), names, types,
                {{"dt", ""}, {"region", ""}});

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->size() == 2);
    REQUIRE(duckdb_compat::ResultValue(*chunk, 1, 0).IsNull());
    REQUIRE(duckdb_compat::ResultValue(*chunk, 2, 0).IsNull());
}

// The file-column lookup beside it is case-insensitive; having the two disagree would mean
// a file whose casing differs lines up while its partition columns silently do not.
TEST_CASE("DeltaShareFileReader matches partition names case-insensitively",
          "[delta_share][reader]") {
    TestDatabase database;
    ScratchParquet parquet;
    parquet.Write(database.Con(), "CAST(i AS INTEGER) AS id", 2);

    duckdb::vector<duckdb::string> names = {"id", "dt"};
    duckdb::vector<duckdb::LogicalType> types = {
        duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER),
        duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR)};

    erpl_web::DeltaShareFileReader reader;
    reader.Open(database.Context(), parquet.Path(), names, types, {{"DT", "2024-03-01"}});

    auto *chunk = reader.NextChunk();
    REQUIRE(chunk != nullptr);
    REQUIRE(duckdb_compat::ResultValue(*chunk, 1, 0).ToString() == "2024-03-01");
}

// #207 defect 4: a file that cannot be read must fail loudly. Swallowing it into an empty
// chunk ended the scan, so one bad file truncated the table and reported success.
TEST_CASE("DeltaShareFileReader throws on a file it cannot read", "[delta_share][reader]") {
    TestDatabase database;

    erpl_web::DeltaShareFileReader reader;
    REQUIRE_THROWS_AS(
        reader.Open(database.Context(), "no_such_file_" + UniqueSuffix() + ".parquet", IdName(),
                    IntVarchar()),
        duckdb::IOException);
    // And it leaves nothing half-open behind.
    REQUIRE_FALSE(reader.IsOpen());
}
