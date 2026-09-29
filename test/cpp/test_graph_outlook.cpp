#include "catch.hpp"
#include "graph_outlook_client.hpp"
#include "duckdb.hpp"

using namespace erpl_web;
using namespace duckdb;

// =============================================================================
// URL Builder Tests
// =============================================================================

TEST_CASE("GraphOutlookUrlBuilder builds correct base URL", "[graph_outlook][url]") {
    REQUIRE(GraphOutlookUrlBuilder::GetBaseUrl() == "https://graph.microsoft.com/v1.0");
}

TEST_CASE("GraphOutlookUrlBuilder - me segment when user is empty", "[graph_outlook][url]") {
    SECTION("events") {
        REQUIRE(GraphOutlookUrlBuilder::BuildEventsUrl("") ==
                "https://graph.microsoft.com/v1.0/me/events");
    }
    SECTION("calendars") {
        REQUIRE(GraphOutlookUrlBuilder::BuildCalendarsUrl("") ==
                "https://graph.microsoft.com/v1.0/me/calendars");
    }
    SECTION("calendar events") {
        REQUIRE(GraphOutlookUrlBuilder::BuildCalendarEventsUrl("", "cal-123") ==
                "https://graph.microsoft.com/v1.0/me/calendars/cal-123/events");
    }
    SECTION("contacts") {
        REQUIRE(GraphOutlookUrlBuilder::BuildContactsUrl("") ==
                "https://graph.microsoft.com/v1.0/me/contacts");
    }
    SECTION("messages") {
        REQUIRE(GraphOutlookUrlBuilder::BuildMessagesUrl("") ==
                "https://graph.microsoft.com/v1.0/me/messages");
    }
    SECTION("mail folders") {
        REQUIRE(GraphOutlookUrlBuilder::BuildMailFoldersUrl("") ==
                "https://graph.microsoft.com/v1.0/me/mailFolders");
    }
    SECTION("folder messages") {
        REQUIRE(GraphOutlookUrlBuilder::BuildFolderMessagesUrl("", "inbox") ==
                "https://graph.microsoft.com/v1.0/me/mailFolders/inbox/messages");
    }
}

TEST_CASE("GraphOutlookUrlBuilder - users segment when GUID provided", "[graph_outlook][url]") {
    const std::string guid = "ae6f7cf1-52a8-4e8b-9864-136bf1353604";

    SECTION("events") {
        REQUIRE(GraphOutlookUrlBuilder::BuildEventsUrl(guid) ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/events");
    }
    SECTION("calendars") {
        REQUIRE(GraphOutlookUrlBuilder::BuildCalendarsUrl(guid) ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/calendars");
    }
    SECTION("calendar events") {
        REQUIRE(GraphOutlookUrlBuilder::BuildCalendarEventsUrl(guid, "cal-123") ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/calendars/cal-123/events");
    }
    SECTION("contacts") {
        REQUIRE(GraphOutlookUrlBuilder::BuildContactsUrl(guid) ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/contacts");
    }
    SECTION("messages") {
        REQUIRE(GraphOutlookUrlBuilder::BuildMessagesUrl(guid) ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/messages");
    }
    SECTION("mail folders") {
        REQUIRE(GraphOutlookUrlBuilder::BuildMailFoldersUrl(guid) ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/mailFolders");
    }
    SECTION("folder messages") {
        REQUIRE(GraphOutlookUrlBuilder::BuildFolderMessagesUrl(guid, "inbox") ==
                "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604/mailFolders/inbox/messages");
    }
}

TEST_CASE("GraphOutlookUrlBuilder - UPN and email are percent-encoded", "[graph_outlook][url]") {
    SECTION("UPN with @") {
        REQUIRE(GraphOutlookUrlBuilder::BuildEventsUrl("jr@data-zoo.de") ==
                "https://graph.microsoft.com/v1.0/users/jr%40data-zoo.de/events");
    }
    SECTION("email with dots") {
        REQUIRE(GraphOutlookUrlBuilder::BuildContactsUrl("john.doe@contoso.com") ==
                "https://graph.microsoft.com/v1.0/users/john.doe%40contoso.com/contacts");
    }
    SECTION("calendar view with UPN") {
        auto url = GraphOutlookUrlBuilder::BuildCalendarViewUrl(
            "jr@data-zoo.de", "2024-01-01T00:00:00", "2024-12-31T00:00:00");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/users/jr%40data-zoo.de"
                       "/calendarView?startDateTime=2024-01-01T00:00:00&endDateTime=2024-12-31T00:00:00");
    }
}

TEST_CASE("GraphOutlookUrlBuilder - calendarView URL includes date params", "[graph_outlook][url]") {
    SECTION("me segment") {
        auto url = GraphOutlookUrlBuilder::BuildCalendarViewUrl("", "2024-01-01T00:00:00", "2024-12-31T00:00:00");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/me/calendarView?startDateTime=2024-01-01T00:00:00&endDateTime=2024-12-31T00:00:00");
    }
    SECTION("GUID segment") {
        auto url = GraphOutlookUrlBuilder::BuildCalendarViewUrl(
            "ae6f7cf1-52a8-4e8b-9864-136bf1353604", "2024-01-01T00:00:00", "2024-12-31T00:00:00");
        REQUIRE(url == "https://graph.microsoft.com/v1.0/users/ae6f7cf1-52a8-4e8b-9864-136bf1353604"
                       "/calendarView?startDateTime=2024-01-01T00:00:00&endDateTime=2024-12-31T00:00:00");
    }
}

// =============================================================================
// DuckDB registration tests
// =============================================================================

TEST_CASE("Graph Outlook functions are registered in DuckDB", "[graph_outlook][duckdb]") {
    DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    DuckDB db(nullptr, &config);
    Connection con(db);

    con.Query("LOAD erpl_odata");

    SECTION("graph_calendars") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_calendars'");
        REQUIRE(result->RowCount() == 1);
    }
    SECTION("graph_calendar_events") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_calendar_events'");
        REQUIRE(result->RowCount() == 1);
    }
    SECTION("graph_contacts") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_contacts'");
        REQUIRE(result->RowCount() == 1);
    }
    SECTION("graph_outlook_mail_folders") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_outlook_mail_folders'");
        REQUIRE(result->RowCount() == 1);
    }
    SECTION("graph_outlook_emails") {
        auto result = con.Query("SELECT function_name FROM duckdb_functions() WHERE function_name = 'graph_outlook_emails'");
        REQUIRE(result->RowCount() == 1);
    }
}

TEST_CASE("graph_calendar_events: start_date without end_date is an error", "[graph_outlook][duckdb]") {
    DBConfig config;
    config.SetOption("allocator_background_threads", Value(true));
    DuckDB db(nullptr, &config);
    Connection con(db);

    con.Query("LOAD erpl_odata");
    con.Query("SET allow_persistent_secrets=false");

    auto result = con.Query("SELECT * FROM graph_calendar_events(start_date := '2024-01-01')");
    REQUIRE(result->HasError());
}
