#include "catch.hpp"
#include "graph_entra_client.hpp"
#include "duckdb.hpp"

using namespace erpl_web;
using namespace duckdb;

// =============================================================================
// URL Builder Tests
// =============================================================================

TEST_CASE("GraphEntraUrlBuilder builds correct base URL", "[graph_entra][url]") {
    REQUIRE(GraphEntraUrlBuilder::GetBaseUrl() == "https://graph.microsoft.com/v1.0");
}

TEST_CASE("GraphEntraUrlBuilder builds user URLs", "[graph_entra][url]") {
    SECTION("Users list URL") {
        auto url = GraphEntraUrlBuilder::BuildUsersUrl();
        REQUIRE(url == "https://graph.microsoft.com/v1.0/users");
    }

    SECTION("Single user URL") {
        auto url = GraphEntraUrlBuilder::BuildUserUrl("user-123");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/users/user-123");
    }
}

TEST_CASE("GraphEntraUrlBuilder builds group URLs", "[graph_entra][url]") {
    SECTION("Groups list URL") {
        auto url = GraphEntraUrlBuilder::BuildGroupsUrl();
        REQUIRE(url == "https://graph.microsoft.com/v1.0/groups");
    }

    SECTION("Single group URL") {
        auto url = GraphEntraUrlBuilder::BuildGroupUrl("group-abc");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/groups/group-abc");
    }

    SECTION("Group members URL") {
        auto url = GraphEntraUrlBuilder::BuildGroupMembersUrl("group-xyz");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/groups/group-xyz/members");
    }
}

TEST_CASE("GraphEntraUrlBuilder builds device URLs", "[graph_entra][url]") {
    SECTION("Devices list URL") {
        auto url = GraphEntraUrlBuilder::BuildDevicesUrl();
        REQUIRE(url == "https://graph.microsoft.com/v1.0/devices");
    }

    SECTION("Single device URL") {
        auto url = GraphEntraUrlBuilder::BuildDeviceUrl("device-456");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/devices/device-456");
    }
}

TEST_CASE("GraphEntraUrlBuilder builds sign-in logs URL", "[graph_entra][url]") {
    auto url = GraphEntraUrlBuilder::BuildSignInLogsUrl();
    REQUIRE(url == "https://graph.microsoft.com/v1.0/auditLogs/signIns");
}

// =============================================================================
// DuckDB Integration Tests
// =============================================================================

TEST_CASE("Graph Entra functions are registered in DuckDB", "[graph_entra][duckdb]") {
    DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    DuckDB db(nullptr, &config);
    Connection con(db);

    // Load extension
    con.Query("LOAD erpl_odata");

    SECTION("graph_users function exists") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_users'");
        REQUIRE(result->RowCount() == 1);
    }

    SECTION("graph_groups function exists") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_groups'");
        REQUIRE(result->RowCount() == 1);
    }

    SECTION("graph_devices function exists") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_devices'");
        REQUIRE(result->RowCount() == 1);
    }

    SECTION("graph_signin_logs function exists") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_signin_logs'");
        REQUIRE(result->RowCount() == 1);
    }
}

TEST_CASE("Graph Entra functions require secret parameter", "[graph_entra][duckdb]") {
    DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    DuckDB db(nullptr, &config);
    Connection con(db);

    con.Query("LOAD erpl_odata");
    con.Query("SET allow_persistent_secrets=false");

    SECTION("graph_users requires secret") {
        auto result = con.Query("SELECT * FROM graph_users()");
        REQUIRE(result->HasError());
    }

    SECTION("graph_groups requires secret") {
        auto result = con.Query("SELECT * FROM graph_groups()");
        REQUIRE(result->HasError());
    }

    SECTION("graph_devices requires secret") {
        auto result = con.Query("SELECT * FROM graph_devices()");
        REQUIRE(result->HasError());
    }

    SECTION("graph_signin_logs requires secret") {
        auto result = con.Query("SELECT * FROM graph_signin_logs()");
        REQUIRE(result->HasError());
    }
}
