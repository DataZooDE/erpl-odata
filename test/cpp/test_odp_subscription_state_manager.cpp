#include "catch.hpp"
#include "duckdb_compat.hpp"
#include "odp_subscription_state_manager.hpp"
#include "odp_test_db.hpp"
#include "duckdb.hpp"

using namespace erpl_web;
using namespace duckdb;

TEST_CASE("OdpSubscriptionStateManager - Basic State Management", "[odp_state_manager]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    
    std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest";
    std::string entity_set_name = "EntityOfTest";
    std::string secret_name = "test_secret";
    
    SECTION("Initial state - force full load") {
        OdpSubscriptionStateManager manager(context, service_url, entity_set_name, secret_name, true);
        
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::INITIAL_LOAD);
        REQUIRE(manager.ShouldPerformInitialLoad());
        REQUIRE(!manager.ShouldPerformDeltaFetch());
        REQUIRE(manager.GetCurrentDeltaToken().empty());
        REQUIRE(manager.IsSubscriptionActive());
        REQUIRE(!manager.GetSubscriptionId().empty());
    }
    
    SECTION("Initial state - existing subscription with token") {
        // First create a subscription with a delta token
        {
            OdpSubscriptionStateManager manager1(context, service_url, entity_set_name, secret_name, true);
            manager1.TransitionToDeltaFetch("test_delta_token_123", true);
        }
        
        // Now create a new manager for the same subscription (should find existing)
        OdpSubscriptionStateManager manager2(context, service_url, entity_set_name, secret_name, false);
        
        REQUIRE(manager2.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::DELTA_FETCH);
        REQUIRE(!manager2.ShouldPerformInitialLoad());
        REQUIRE(manager2.ShouldPerformDeltaFetch());
        REQUIRE(manager2.GetCurrentDeltaToken() == "test_delta_token_123");
        REQUIRE(manager2.IsSubscriptionActive());
    }
    
    SECTION("Import delta token") {
        std::string import_token = "D20240101_120000_456";
        OdpSubscriptionStateManager manager(context, service_url + "2", "EntityOfTest2", secret_name, false, import_token);
        
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::DELTA_FETCH);
        REQUIRE(manager.GetCurrentDeltaToken() == import_token);
        REQUIRE(manager.ShouldPerformDeltaFetch());
    }
}

TEST_CASE("OdpSubscriptionStateManager - State Transitions", "[odp_state_transitions]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    
    std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTransition";
    std::string entity_set_name = "EntityOfTransition";
    
    OdpSubscriptionStateManager manager(context, service_url, entity_set_name, "", true);
    
    SECTION("Initial Load -> Delta Fetch") {
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::INITIAL_LOAD);
        
        // Transition to delta fetch
        std::string delta_token = "delta_token_after_initial_load";
        manager.TransitionToDeltaFetch(delta_token, true);
        
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::DELTA_FETCH);
        REQUIRE(manager.GetCurrentDeltaToken() == delta_token);
        REQUIRE(manager.ShouldPerformDeltaFetch());
        REQUIRE(!manager.ShouldPerformInitialLoad());
        REQUIRE(manager.IsSubscriptionActive());
    }
    
    SECTION("Delta Fetch -> Initial Load (reset)") {
        // First transition to delta fetch
        manager.TransitionToDeltaFetch("some_token", true);
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::DELTA_FETCH);
        
        // Reset to initial load
        manager.TransitionToInitialLoad();
        
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::INITIAL_LOAD);
        REQUIRE(manager.GetCurrentDeltaToken().empty());
        REQUIRE(manager.ShouldPerformInitialLoad());
        REQUIRE(!manager.ShouldPerformDeltaFetch());
        REQUIRE(manager.IsSubscriptionActive());
    }
    
    SECTION("Any State -> Terminated") {
        manager.TransitionToTerminated();
        
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::TERMINATED);
        REQUIRE(!manager.IsSubscriptionActive());
        REQUIRE(!manager.ShouldPerformInitialLoad());
        REQUIRE(!manager.ShouldPerformDeltaFetch());
    }
    
    SECTION("Any State -> Error") {
        std::string error_msg = "Test error message";
        manager.TransitionToError(error_msg);
        
        REQUIRE(manager.GetCurrentPhase() == OdpSubscriptionStateManager::SubscriptionPhase::ERROR_STATE);
        REQUIRE(!manager.IsSubscriptionActive());
        REQUIRE(!manager.ShouldPerformInitialLoad());
        REQUIRE(!manager.ShouldPerformDeltaFetch());
    }
}

TEST_CASE("OdpSubscriptionStateManager - Delta Token Management", "[odp_delta_tokens]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    
    std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfDelta";
    std::string entity_set_name = "EntityOfDelta";
    
    OdpSubscriptionStateManager manager(context, service_url, entity_set_name, "", true);
    
    SECTION("Update delta token") {
        std::string initial_token = "initial_delta_token";
        std::string updated_token = "updated_delta_token";
        
        // Set initial token
        manager.TransitionToDeltaFetch(initial_token, true);
        REQUIRE(manager.GetCurrentDeltaToken() == initial_token);
        
        // Update token
        manager.UpdateDeltaToken(updated_token);
        REQUIRE(manager.GetCurrentDeltaToken() == updated_token);
        
        // Verify persistence by creating new manager
        OdpSubscriptionStateManager manager2(context, service_url, entity_set_name, "", false);
        REQUIRE(manager2.GetCurrentDeltaToken() == updated_token);
    }
    
    SECTION("Multiple token updates") {
        std::vector<std::string> tokens = {
            "token_001",
            "token_002", 
            "token_003"
        };
        
        for (const auto& token : tokens) {
            manager.UpdateDeltaToken(token);
            REQUIRE(manager.GetCurrentDeltaToken() == token);
        }
        
        // Final token should be persisted
        REQUIRE(manager.GetCurrentDeltaToken() == tokens.back());
    }
}

// GitHub #158 follow-up: writing the extraction totals must not blank the rest of the row.
// UpdateAuditEntry assigns EVERY column, so calling it at the end of a scan with only rows
// and bytes wiped out the delta_token_after a page-boundary update had recorded - the one
// field that says which package the audit row refers to.
TEST_CASE("OdpSubscriptionStateManager - writing totals preserves the delta token",
          "[odp_audit]") {
    odp_test::TempDatabase temp_db;
    ClientContext &context = temp_db.Context();

    const std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTotals";
    OdpSubscriptionStateManager manager(context, service_url, "EntityOfTotals");

    const int64_t audit_id = manager.CreateAuditEntry("initial_load", service_url);
    REQUIRE(audit_id > 0);

    // A page boundary records the token and status for the package.
    manager.UpdateAuditEntry(audit_id, 200, 0, 4096, "TOKEN_FOR_PACKAGE", "", 100);

    // The scan then finishes and records what it actually delivered.
    manager.UpdateAuditTotals(audit_id, 3000, 24000);

    auto row = temp_db.Conn().Query(
        "SELECT rows_fetched, package_size_bytes, delta_token_after, http_status_code "
        "FROM erpl_web.odp_subscription_audit WHERE audit_id = " + std::to_string(audit_id));
    INFO((row->HasError() ? row->GetError() : std::string()));
    REQUIRE_FALSE(row->HasError());
    REQUIRE(row->RowCount() == 1);

    CHECK(duckdb_compat::ResultValue(*row, 0, 0).GetValue<int64_t>() == 3000);
    CHECK(duckdb_compat::ResultValue(*row, 1, 0).GetValue<int64_t>() == 24000);
    // The token and status must survive the totals write.
    CHECK(duckdb_compat::ResultValue(*row, 2, 0).ToString() == "TOKEN_FOR_PACKAGE");
    CHECK(duckdb_compat::ResultValue(*row, 3, 0).GetValue<int32_t>() == 200);
}

TEST_CASE("OdpSubscriptionStateManager - Audit Operations", "[odp_audit]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    
    std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfAudit";
    std::string entity_set_name = "EntityOfAudit";
    
    OdpSubscriptionStateManager manager(context, service_url, entity_set_name);
    
    SECTION("Create and update audit entry") {
        std::string request_url = service_url + "?$format=json";
        
        // Create audit entry
        int64_t audit_id = manager.CreateAuditEntry("initial_load", request_url);
        REQUIRE(audit_id > 0);
        
        // Update audit entry
        manager.UpdateAuditEntry(audit_id, 200, 150, 2048, "new_delta_token", "", 750);
        
        // Verify the audit entry was created (we can't easily verify the update without direct DB access)
        REQUIRE(audit_id > 0);
    }
    
    SECTION("Multiple audit entries") {
        std::vector<std::string> operations = {"initial_load", "delta_fetch", "delta_fetch"};
        std::vector<int64_t> audit_ids;
        
        for (const auto& operation : operations) {
            int64_t audit_id = manager.CreateAuditEntry(operation, service_url);
            REQUIRE(audit_id > 0);
            audit_ids.push_back(audit_id);
            
            // Update each entry
            manager.UpdateAuditEntry(audit_id, 200, 100, 1024);
        }
        
        // All audit IDs should be unique and increasing
        for (size_t i = 1; i < audit_ids.size(); i++) {
            REQUIRE(audit_ids[i] > audit_ids[i-1]);
        }
    }
}

TEST_CASE("OdpSubscriptionStateManager - Utility Methods", "[odp_state_utils]") {
    SECTION("Phase to string conversion") {
        REQUIRE(OdpSubscriptionStateManager::PhaseToString(
            OdpSubscriptionStateManager::SubscriptionPhase::INITIAL_LOAD) == "INITIAL_LOAD");
        REQUIRE(OdpSubscriptionStateManager::PhaseToString(
            OdpSubscriptionStateManager::SubscriptionPhase::DELTA_FETCH) == "DELTA_FETCH");
        REQUIRE(OdpSubscriptionStateManager::PhaseToString(
            OdpSubscriptionStateManager::SubscriptionPhase::TERMINATED) == "TERMINATED");
        REQUIRE(OdpSubscriptionStateManager::PhaseToString(
            OdpSubscriptionStateManager::SubscriptionPhase::ERROR_STATE) == "ERROR_STATE");
    }
    
    SECTION("Log current state (should not throw)") {
        odp_test::TempDatabase temp_db;
        ClientContext& context = temp_db.Context();
        
        OdpSubscriptionStateManager manager(context, 
            "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfLog", "EntityOfLog");
        
        REQUIRE_NOTHROW(manager.LogCurrentState());
        
        // Transition to different states and log
        manager.TransitionToDeltaFetch("test_token", true);
        REQUIRE_NOTHROW(manager.LogCurrentState());
        
        manager.TransitionToError("test error");
        REQUIRE_NOTHROW(manager.LogCurrentState());
    }
}

TEST_CASE("OdpSubscriptionStateManager - Error Handling", "[odp_state_errors]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    
    SECTION("Invalid URL validation") {
        REQUIRE_THROWS_AS(
            OdpSubscriptionStateManager(context, "", "EntityOfTest"),
            InvalidInputException
        );
        
        // A URL that does not follow the ODP naming convention is accepted with
        // a warning; refusing it blocked valid services (#105).
        REQUIRE_NOTHROW(
            OdpSubscriptionStateManager(context, "https://valid.com/RegularEntity", "RegularEntity"));
    }
    
    SECTION("Empty entity set name") {
        REQUIRE_THROWS_AS(
            OdpSubscriptionStateManager(context, "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest", ""),
            InvalidInputException
        );
    }
}


// ============================================================================
// GitHub #105 -- import_delta_token must be validated, not trusted
// ============================================================================

TEST_CASE("OdpSubscriptionStateManager - imported delta token validation", "[odp_state_manager][odp_token_validation]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();

    const std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfImport";
    const std::string entity_set_name = "EntityOfImport";

    SECTION("A well-formed token is accepted") {
        REQUIRE_NOTHROW(OdpSubscriptionStateManager(
            context, service_url, entity_set_name, "", false, "D20240101120000_000000000"));
    }

    SECTION("A token carrying URL syntax is rejected") {
        // Pre-fix: anything at all was accepted and pasted into the delta URL,
        // so a '&' or a space silently changed the request the service saw.
        REQUIRE_THROWS_AS(OdpSubscriptionStateManager(
            context, service_url, entity_set_name, "", false, "D2024&$top=1"), InvalidInputException);
        REQUIRE_THROWS_AS(OdpSubscriptionStateManager(
            context, service_url, entity_set_name, "", false, "token with spaces"), InvalidInputException);
        REQUIRE_THROWS_AS(OdpSubscriptionStateManager(
            context, service_url, entity_set_name, "", false, "tok\nen"), InvalidInputException);
    }

    SECTION("An implausibly long token is rejected") {
        REQUIRE_THROWS_AS(OdpSubscriptionStateManager(
            context, service_url, entity_set_name, "", false, std::string(4096, 'A')), InvalidInputException);
    }

    SECTION("force_full_load combined with import_delta_token is rejected") {
        // Pre-fix: the token was accepted and then thrown away by the forced
        // full load, leaving the caller believing they had resumed a stream.
        REQUIRE_THROWS_AS(OdpSubscriptionStateManager(
            context, service_url, entity_set_name, "", true, "D20240101120000_000000000"),
            InvalidInputException);
    }
}

// ============================================================================
// GitHub #95 -- the delta token advance is a compare-and-swap
// ============================================================================

TEST_CASE("OdpSubscriptionStateManager - two sessions on one subscription",
          "[odp_state_manager][odp_concurrency]") {
    odp_test::TempDatabase temp_db;
    auto second_connection = temp_db.NewConnection();

    const std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTwoSessions";
    const std::string entity_set_name = "EntityOfTwoSessions";

    OdpSubscriptionStateManager session_a(temp_db.Context(), service_url, entity_set_name);
    OdpSubscriptionStateManager session_b(*second_connection->context, service_url, entity_set_name);

    // Both sessions attached to the same subscription and see the same state.
    REQUIRE(session_a.GetSubscriptionId() == session_b.GetSubscriptionId());
    REQUIRE(session_a.GetCurrentDeltaToken() == session_b.GetCurrentDeltaToken());

    // A finishes its read first and advances the stream.
    session_a.TransitionToDeltaFetch("D20240101_AAA", true);
    REQUIRE(session_a.GetCurrentDeltaToken() == "D20240101_AAA");

    // Pre-fix: B's advance silently overwrote A's, and the two sessions each
    // held half of the change stream with no error anywhere.
    REQUIRE_THROWS(session_b.TransitionToDeltaFetch("D20240101_BBB", true));

    OdpSubscriptionStateManager reader(temp_db.Context(), service_url, entity_set_name);
    REQUIRE(reader.GetCurrentDeltaToken() == "D20240101_AAA");
}
