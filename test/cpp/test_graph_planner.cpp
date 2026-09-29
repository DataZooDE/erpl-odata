#include "catch.hpp"
#include "duckdb.hpp"
#include "graph_planner_client.hpp"

using namespace erpl_web;

// =============================================================================
// GraphPlannerUrlBuilder Tests
// =============================================================================

TEST_CASE("GraphPlannerUrlBuilder - GetBaseUrl", "[graph_planner][url_builder]") {
    REQUIRE(GraphPlannerUrlBuilder::GetBaseUrl() == "https://graph.microsoft.com/v1.0");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildGroupPlansUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildGroupPlansUrl("group-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/groups/group-id-123/planner/plans");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildPlanUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildPlanUrl("plan-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/plans/plan-id-123");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildPlanBucketsUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildPlanBucketsUrl("plan-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/plans/plan-id-123/buckets");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildPlanTasksUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildPlanTasksUrl("plan-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/plans/plan-id-123/tasks");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildTaskDetailsUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildTaskDetailsUrl("task-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/tasks/task-id-123/details");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildTaskUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildTaskUrl("task-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/tasks/task-id-123");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildBucketUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildBucketUrl("bucket-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/buckets/bucket-id-123");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildBucketTasksUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildBucketTasksUrl("bucket-id-123");
    REQUIRE(url == "https://graph.microsoft.com/v1.0/planner/buckets/bucket-id-123/tasks");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildMyTasksUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildMyTasksUrl();
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/planner/tasks");
}

TEST_CASE("GraphPlannerUrlBuilder - BuildMyPlansUrl", "[graph_planner][url_builder]") {
    auto url = GraphPlannerUrlBuilder::BuildMyPlansUrl();
    REQUIRE(url == "https://graph.microsoft.com/v1.0/me/planner/plans");
}

// =============================================================================
// DuckDB Integration Tests (Function Existence)
// =============================================================================

TEST_CASE("Microsoft Graph Planner Functions Exist", "[graph_planner][functions]") {
    duckdb::DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    duckdb::DuckDB db(nullptr, &config);
    duckdb::Connection con(db);

    // Load the extension
    auto result = con.Query("LOAD erpl_odata");
    REQUIRE_FALSE(result->HasError());

    SECTION("graph_planner_plans function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_planner_plans'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_planner_buckets function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_planner_buckets'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }

    SECTION("graph_planner_tasks function exists") {
        result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'graph_planner_tasks'");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    }
}
