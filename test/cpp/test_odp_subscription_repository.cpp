#include "catch.hpp"
#include "duckdb_compat.hpp"
#include "odp_subscription_repository.hpp"
#include "odp_test_db.hpp"
#include "duckdb.hpp"
#include <chrono>
#include <thread>

using namespace erpl_web;
using namespace duckdb;

TEST_CASE("OdpSubscriptionRepository - Basic Operations", "[odp_repository]") {
    odp_test::TempDatabase temp_db;
    Connection& conn = temp_db.Conn();
    ClientContext& context = temp_db.Context();
    
    OdpSubscriptionRepository repo(context);
    
    SECTION("Schema and table creation") {
        REQUIRE_NOTHROW(repo.EnsureSchemaExists());
        REQUIRE_NOTHROW(repo.EnsureTablesExist());
        
        // Verify schema exists
        auto result = conn.Query("SELECT schema_name FROM information_schema.schemata WHERE schema_name = 'erpl_web'");
        REQUIRE(result->RowCount() == 1);
        
        // Verify tables exist
        auto tables_result = conn.Query("SELECT table_name FROM information_schema.tables WHERE table_schema = 'erpl_web'");
        REQUIRE(tables_result->RowCount() >= 2); // At least subscriptions and audit tables
    }
    
    SECTION("Subscription creation and retrieval") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest";
        std::string entity_set_name = "EntityOfTest";
        std::string secret_name = "test_secret";
        
        // Create subscription
        std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name, secret_name);
        REQUIRE(!subscription_id.empty());
        
        // Verify subscription ID format (timestamp_cleaned_url)
        REQUIRE(subscription_id.length() > 15); // At least YYYYMMDD_HHMMSS
        REQUIRE(subscription_id.find("_") != std::string::npos);
        
        // Retrieve subscription
        auto subscription = repo.GetSubscription(subscription_id);
        REQUIRE(subscription.has_value());
        REQUIRE(subscription->subscription_id == subscription_id);
        REQUIRE(subscription->service_url == service_url);
        REQUIRE(subscription->entity_set_name == entity_set_name);
        REQUIRE(subscription->secret_name == secret_name);
        REQUIRE(subscription->subscription_status == "active");
        REQUIRE(subscription->preference_applied == false);
    }
    
    SECTION("Find active subscription") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest2";
        std::string entity_set_name = "EntityOfTest2";
        
        // No subscription should exist initially
        auto existing = repo.FindActiveSubscription(service_url, entity_set_name, "default");
        REQUIRE(!existing.has_value());
        
        // Create subscription
        std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name);
        
        // Should find the active subscription
        existing = repo.FindActiveSubscription(service_url, entity_set_name, "default");
        REQUIRE(existing.has_value());
        REQUIRE(existing->subscription_id == subscription_id);
    }
    
    SECTION("Delta token updates") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest3";
        std::string entity_set_name = "EntityOfTest3";
        
        // Create subscription
        std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name);
        
        // Advance the delta token from its initial empty value
        std::string delta_token = "test_delta_token_12345";
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(subscription_id, "", delta_token));
        
        // Verify update
        auto subscription = repo.GetSubscription(subscription_id);
        REQUIRE(subscription.has_value());
        REQUIRE(subscription->delta_token == delta_token);
    }
    
    SECTION("Subscription status updates") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest4";
        std::string entity_set_name = "EntityOfTest4";
        
        // Create subscription
        std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name);
        
        // Update status
        bool success = repo.UpdateSubscriptionStatus(subscription_id, "terminated");
        REQUIRE(success);
        
        // Verify update
        auto subscription = repo.GetSubscription(subscription_id);
        REQUIRE(subscription.has_value());
        REQUIRE(subscription->subscription_status == "terminated");
    }
    
    SECTION("List subscriptions") {
        // Create multiple subscriptions
        std::string base_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/";
        
        repo.CreateSubscription(base_url + "EntityOfTest5", "EntityOfTest5");
        repo.CreateSubscription(base_url + "EntityOfTest6", "EntityOfTest6");
        
        // List all subscriptions
        auto subscriptions = repo.ListAllSubscriptions();
        REQUIRE(subscriptions.size() >= 2);
        
        // Verify ordering (should be by created_at DESC due to timestamp prefix)
        if (subscriptions.size() >= 2) {
            REQUIRE(subscriptions[0].created_at >= subscriptions[1].created_at);
        }
    }
    
    SECTION("Audit entry creation") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest7";
        std::string entity_set_name = "EntityOfTest7";
        
        // Create subscription
        std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name);
        
        // Create audit entry
        OdpAuditEntry entry(subscription_id, "initial_load");
        entry.request_url = service_url;
        entry.delta_token_before = "";
        
        int64_t audit_id = repo.CreateAuditEntry(entry);
        REQUIRE(audit_id > 0);
        
        // Update audit entry
        entry.audit_id = audit_id;
        entry.response_timestamp = std::chrono::system_clock::now();
        entry.http_status_code = 200;
        entry.rows_fetched = 100;
        entry.package_size_bytes = 1024;
        entry.delta_token_after = "new_delta_token";
        entry.duration_ms = 500;
        
        bool success = repo.UpdateAuditEntry(entry);
        REQUIRE(success);
    }
    
    SECTION("Remove subscription") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest8";
        std::string entity_set_name = "EntityOfTest8";
        
        // Create subscription
        std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name);
        
        // Verify it exists
        auto subscription = repo.GetSubscription(subscription_id);
        REQUIRE(subscription.has_value());
        
        // Remove subscription
        bool success = repo.RemoveSubscription(subscription_id);
        REQUIRE(success);
        
        // Verify it's gone
        subscription = repo.GetSubscription(subscription_id);
        REQUIRE(!subscription.has_value());
    }
}

TEST_CASE("OdpSubscriptionRepository - Utility Methods", "[odp_repository_utils]") {
    SECTION("Generate subscription ID") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest";
        std::string entity_set_name = "EntityOfTest";
        
        std::string id1 = OdpSubscriptionRepository::GenerateSubscriptionId(service_url, entity_set_name, "default");
        
        // Wait a second to ensure different timestamp
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        std::string id2 = OdpSubscriptionRepository::GenerateSubscriptionId(service_url, entity_set_name, "default");
        
        // IDs should be different due to timestamp
        REQUIRE(id1 != id2);
        
        // Both should contain timestamp pattern
        REQUIRE(id1.find("_") != std::string::npos);
        REQUIRE(id2.find("_") != std::string::npos);
        
        // ID1 should be lexicographically smaller (earlier timestamp)
        REQUIRE(id1 < id2);
    }
    
    SECTION("Clean URL for ID") {
        std::string url1 = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfTest";
        std::string cleaned1 = OdpSubscriptionRepository::CleanUrlForId(url1);
        
        REQUIRE(cleaned1.find("https") == std::string::npos); // Protocol removed
        REQUIRE(cleaned1.find("://") == std::string::npos);   // Protocol separator removed
        REQUIRE(cleaned1.find("/") == std::string::npos);     // Slashes replaced
        REQUIRE(!cleaned1.empty());
        
        // Should contain underscores instead of special characters
        REQUIRE(cleaned1.find("_") != std::string::npos);
    }
    
    SECTION("Validate ODP URL") {
        // Valid ODP URLs
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/EntityOfTest"));
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/FactsOfTest"));
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfSEPM_ISO"));
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/sap/opu/odata/sap/TEST_SRV/FactsOf0D_NW_C01"));
        // A query string must not make a valid service URL fail the check (#105)
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl(
            "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfSEPM_ISO?$format=json"));
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl(
            "https://test.com/sap/opu/odata/sap/TEST_SRV/FactsOf0D_NW_C01?$format=json&!deltatoken=D20240101"));
        REQUIRE(OdpSubscriptionRepository::IsValidOdpUrl(
            "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfSEPM_ISO/"));
        
        // Invalid URLs
        REQUIRE(!OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/RegularEntity"));
        REQUIRE(!OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/TestEntity"));
        REQUIRE(!OdpSubscriptionRepository::IsValidOdpUrl("https://test.com/"));
        REQUIRE(!OdpSubscriptionRepository::IsValidOdpUrl(""));
    }
}

TEST_CASE("OdpSubscriptionRepository - Error Handling", "[odp_repository_errors]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    
    OdpSubscriptionRepository repo(context);
    repo.EnsureTablesExist();
    
    SECTION("A URL that does not follow the ODP naming convention is accepted with a warning") {
        // The EntityOf*/FactsOf*/AttrOf* convention is a hint, not a contract:
        // refusing such a URL blocked legitimate services (#105).
        REQUIRE_NOTHROW(repo.CreateSubscription("https://valid.com/RegularEntity", "RegularEntity"));
    }
    
    SECTION("Non-existent subscription operations") {
        std::string fake_id = "20240101_120000_fake_subscription";
        
        // Get non-existent subscription
        auto subscription = repo.GetSubscription(fake_id);
        REQUIRE(!subscription.has_value());
        
        // Advancing the token of a subscription that is gone must say so, not
        // report success (#95).
        REQUIRE_THROWS(repo.AdvanceDeltaToken(fake_id, "", "test_token"));
        
        bool success = repo.UpdateSubscriptionStatus(fake_id, "terminated");
        REQUIRE(!success); // Should fail gracefully
        
        // Remove non-existent subscription
        success = repo.RemoveSubscription(fake_id);
        REQUIRE(!success); // Should fail gracefully
    }
    
    SECTION("Duplicate subscription handling") {
        std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfDuplicate";
        std::string entity_set_name = "EntityOfDuplicate";
        
        // Create first subscription
        std::string id1 = repo.CreateSubscription(service_url, entity_set_name);
        REQUIRE(!id1.empty());
        
        // Attempt to create duplicate - should return existing ID
        std::string id2 = repo.CreateSubscription(service_url, entity_set_name);
        REQUIRE(id1 == id2);
    }
}


// ============================================================================
// GitHub #99 -- repository SQL must be built from bound parameters
// ============================================================================

TEST_CASE("OdpSubscriptionRepository - values containing SQL syntax round-trip", "[odp_repository][odp_sql_injection]") {
    odp_test::TempDatabase temp_db;
    ClientContext& context = temp_db.Context();
    OdpSubscriptionRepository repo(context);

    SECTION("A single quote in the service URL and entity set name survives a round-trip") {
        // Pre-fix: FindActiveSubscription interpolated both values straight into
        // the SQL text, so the quote produced a parse error that was swallowed
        // and reported as "no subscription" -- this REQUIRE failed.
        const std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOf'Quote";
        const std::string entity_set_name = "EntityOf'Quote";

        const std::string subscription_id = repo.CreateSubscription(service_url, entity_set_name, "sec'ret");

        auto found = repo.FindActiveSubscription(service_url, entity_set_name, "sec'ret");
        REQUIRE(found.has_value());
        REQUIRE(found->subscription_id == subscription_id);
        REQUIRE(found->service_url == service_url);
        REQUIRE(found->entity_set_name == entity_set_name);
        REQUIRE(found->secret_name == "sec'ret");
    }

    SECTION("A crafted entity set name cannot execute a statement of its own") {
        // Pre-fix: this value closed the string literal and appended a DROP, and
        // the repository ran it on a full-privilege connection -- the table was
        // gone by the time this REQUIRE ran.
        const std::string malicious = "x'; DROP TABLE erpl_web.odp_subscriptions; --";
        repo.CreateSubscription("https://test.com/EntityOfInjection", malicious);

        auto surviving = temp_db.Conn().Query(
            "SELECT count(*) FROM information_schema.tables "
            "WHERE table_schema = 'erpl_web' AND table_name = 'odp_subscriptions'");
        REQUIRE(!surviving->HasError());
        REQUIRE(duckdb_compat::ResultValue(*surviving, 0, 0).GetValue<int64_t>() == 1);

        auto found = repo.FindActiveSubscription("https://test.com/EntityOfInjection", malicious, "default");
        REQUIRE(found.has_value());
        REQUIRE(found->entity_set_name == malicious);
    }

    SECTION("A delta token containing a quote round-trips") {
        const std::string url = "https://test.com/EntityOfTokenQuote";
        const std::string id = repo.CreateSubscription(url, "EntityOfTokenQuote");

        const std::string token = "D2024'0101_120000";
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(id, "", token));

        auto reloaded = repo.GetSubscription(id);
        REQUIRE(reloaded.has_value());
        REQUIRE(reloaded->delta_token == token);
    }
}

// ============================================================================
// GitHub #92 -- ODP state must live in a named, persistent catalog
// ============================================================================

TEST_CASE("OdpSubscriptionRepository - state catalog resolution", "[odp_repository][odp_state_catalog]") {
    SECTION("An in-memory database is refused") {
        // Pre-fix: the schema was created in the in-memory catalog and every
        // delta token was discarded at exit -- nothing threw.
        DBConfig config;
        config.SetOption("allocator_background_threads", Value(true));
        DuckDB db(nullptr, &config);
        Connection conn(db);

        OdpSubscriptionRepository repo(*conn.context);
        REQUIRE_THROWS_AS(repo.EnsureTablesExist(), InvalidInputException);
    }

    SECTION("A persistent database is used and named") {
        odp_test::TempDatabase temp_db;
        OdpSubscriptionRepository repo(temp_db.Context());
        REQUIRE_NOTHROW(repo.EnsureTablesExist());
        REQUIRE(!repo.GetStateCatalog().empty());
        REQUIRE(repo.GetStateCatalog() != "memory");
    }

    SECTION("USE of another database does not move the state") {
        // Pre-fix: the schema name was unqualified, so a USE re-pointed every
        // later statement -- the subscription landed in the other database and
        // this count came back 0.
        odp_test::TempDatabase state_db("odp_state_home");
        const auto other_path = odp_test::TempDatabase::MakeUniquePath("odp_state_elsewhere");

        OdpSubscriptionRepository repo(state_db.Context());
        repo.EnsureTablesExist();
        const std::string home_catalog = repo.GetStateCatalog();

        auto attach = state_db.Conn().Query("ATTACH '" + other_path.string() + "' AS elsewhere");
        REQUIRE(!attach->HasError());
        auto use_result = state_db.Conn().Query("USE elsewhere");
        REQUIRE(!use_result->HasError());

        repo.CreateSubscription("https://test.com/EntityOfHome", "EntityOfHome");

        auto rows = state_db.Conn().Query(
            "SELECT count(*) FROM \"" + home_catalog + "\".erpl_web.odp_subscriptions");
        REQUIRE(!rows->HasError());
        REQUIRE(duckdb_compat::ResultValue(*rows, 0, 0).GetValue<int64_t>() == 1);

        auto stray = state_db.Conn().Query(
            "SELECT count(*) FROM duckdb_tables() WHERE database_name = 'elsewhere' AND schema_name = 'erpl_web'");
        REQUIRE(!stray->HasError());
        REQUIRE(duckdb_compat::ResultValue(*stray, 0, 0).GetValue<int64_t>() == 0);

        state_db.Conn().Query("USE \"" + home_catalog + "\"");
        state_db.Conn().Query("DETACH elsewhere");
        std::error_code ignored;
        std::filesystem::remove(other_path, ignored);
    }
}

// ============================================================================
// GitHub #95 -- concurrent readers must not split the delta stream
// ============================================================================

TEST_CASE("OdpSubscriptionRepository - concurrent delta advance", "[odp_repository][odp_concurrency]") {
    odp_test::TempDatabase temp_db;
    auto second_connection = temp_db.NewConnection();

    OdpSubscriptionRepository session_a(temp_db.Context());
    OdpSubscriptionRepository session_b(*second_connection->context);

    const std::string url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfRace";
    const std::string entity = "EntityOfRace";
    const std::string id = session_a.CreateSubscription(url, entity);

    SECTION("The second advance from a stale token fails loudly") {
        // Both sessions read the same starting token.
        auto seen_by_a = session_a.FindActiveSubscription(url, entity, "default");
        auto seen_by_b = session_b.FindActiveSubscription(url, entity, "default");
        REQUIRE(seen_by_a.has_value());
        REQUIRE(seen_by_b.has_value());
        REQUIRE(seen_by_a->delta_token == seen_by_b->delta_token);

        session_a.AdvanceDeltaToken(id, seen_by_a->delta_token, "TOKEN_FROM_A");

        // Pre-fix: this was an unconditional UPDATE, so it quietly overwrote A's
        // token and the two sessions each got half of the change stream.
        REQUIRE_THROWS(session_b.AdvanceDeltaToken(id, seen_by_b->delta_token, "TOKEN_FROM_B"));

        auto final_state = session_a.GetSubscription(id);
        REQUIRE(final_state.has_value());
        REQUIRE(final_state->delta_token == "TOKEN_FROM_A");
    }

    SECTION("An advance from the current token succeeds") {
        session_a.AdvanceDeltaToken(id, "", "TOKEN_1");
        REQUIRE_NOTHROW(session_b.AdvanceDeltaToken(id, "TOKEN_1", "TOKEN_2"));
        REQUIRE(session_a.GetSubscription(id)->delta_token == "TOKEN_2");
    }

    SECTION("Only one subscription can exist per (service_url, entity_set_name)") {
        // Pre-fix: there was no unique constraint, so two sessions racing on the
        // first read each inserted their own row -- this INSERT succeeded.
        auto duplicate = temp_db.Conn().Query(
            "INSERT INTO \"" + session_a.GetStateCatalog() + "\".erpl_web.odp_subscriptions "
            "(subscription_id, service_url, entity_set_name, secret_name, delta_token, "
            "subscription_status, preference_applied, schema_version) "
            "VALUES ('other_id', '" + url + "', '" + entity + "', 'default', '', 'active', FALSE, 1)");
        REQUIRE(duplicate->HasError());
    }

    SECTION("Audit ids come from a sequence") {
        // Pre-fix: the id was the current time in microseconds, so it was a
        // 16-digit number rather than a sequence value.
        OdpAuditEntry first(id, "initial_load");
        OdpAuditEntry second(id, "delta_fetch");

        const int64_t first_id = session_a.CreateAuditEntry(first);
        const int64_t second_id = session_a.CreateAuditEntry(second);

        REQUIRE(first_id == 1);
        REQUIRE(second_id == 2);
    }
}

// ============================================================================
// GitHub #96 -- a failing query is not the same answer as "no subscription"
// ============================================================================

TEST_CASE("OdpSubscriptionRepository - failures are distinguished from not-found",
          "[odp_repository][odp_error_reporting]") {
    SECTION("A broken state table throws instead of reporting no subscription") {
        odp_test::TempDatabase temp_db;
        OdpSubscriptionRepository repo(temp_db.Context());
        repo.EnsureTablesExist();
        repo.CreateSubscription("https://test.com/EntityOfBroken", "EntityOfBroken");

        auto dropped = temp_db.Conn().Query(
            "DROP TABLE \"" + repo.GetStateCatalog() + "\".erpl_web.odp_subscriptions");
        REQUIRE(!dropped->HasError());

        // Pre-fix: the query error was caught and turned into nullopt, which the
        // caller read as "never subscribed" and answered with a full reload.
        REQUIRE_THROWS(repo.FindActiveSubscription("https://test.com/EntityOfBroken", "EntityOfBroken", "default"));
    }

    SECTION("A state table written by an older version is detected") {
        odp_test::TempDatabase temp_db;
        auto& conn = temp_db.Conn();
        REQUIRE(!conn.Query("CREATE SCHEMA erpl_web")->HasError());
        // The pre-#92 layout: no schema_version column.
        REQUIRE(!conn.Query(
            "CREATE TABLE erpl_web.odp_subscriptions ("
            "subscription_id VARCHAR PRIMARY KEY, service_url VARCHAR, entity_set_name VARCHAR, "
            "secret_name VARCHAR, delta_token VARCHAR, created_at TIMESTAMP, last_updated TIMESTAMP, "
            "subscription_status VARCHAR, preference_applied BOOLEAN)")->HasError());

        OdpSubscriptionRepository repo(temp_db.Context());
        // Pre-fix: CREATE TABLE IF NOT EXISTS accepted the old table and the
        // repository read its columns by position, misreading every row.
        REQUIRE_THROWS_AS(repo.EnsureTablesExist(), InvalidInputException);
    }
}

// ============================================================================
// GitHub #100 -- an error body is truncated before it is persisted
// ============================================================================

TEST_CASE("OdpSubscriptionRepository - audit error bodies are truncated", "[odp_repository][odp_redaction]") {
    const std::string huge(OdpSubscriptionRepository::MAX_AUDIT_ERROR_LENGTH + 500, 'x');
    const std::string truncated = OdpSubscriptionRepository::TruncateForAudit(huge);

    // Pre-fix: the whole body went into the audit row verbatim.
    REQUIRE(truncated.length() < huge.length());
    REQUIRE(truncated.find("[truncated]") != std::string::npos);

    const std::string small = "short error";
    REQUIRE(OdpSubscriptionRepository::TruncateForAudit(small) == small);
}

TEST_CASE("OdpSubscriptionRepository - the secret is part of subscription identity",
          "[odp_repository][odp_secret_identity]") {
    odp_test::TempDatabase temp_db;
    OdpSubscriptionRepository repo(temp_db.Context());

    const std::string service_url = "https://test.com/sap/opu/odata/sap/TEST_SRV/EntityOfShared";
    const std::string entity_set_name = "EntityOfShared";

    SECTION("two credentials on one entity set get their own subscription and their own cursor") {
        // Before #236 the key was (service_url, entity_set_name), so these two callers shared
        // one row. Alice's extraction advanced Bob's cursor, and Bob then received a delta
        // relative to a position he never reached -- silently, with rows missing.
        const std::string alice = repo.CreateSubscription(service_url, entity_set_name, "alice_secret");
        const std::string bob = repo.CreateSubscription(service_url, entity_set_name, "bob_secret");

        REQUIRE(alice != bob);

        auto found_alice = repo.FindActiveSubscription(service_url, entity_set_name, "alice_secret");
        auto found_bob = repo.FindActiveSubscription(service_url, entity_set_name, "bob_secret");
        REQUIRE(found_alice.has_value());
        REQUIRE(found_bob.has_value());
        REQUIRE(found_alice->subscription_id == alice);
        REQUIRE(found_bob->subscription_id == bob);

        // The cursors must move independently: advancing Alice's leaves Bob's where it was.
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(alice, std::string(), "alice_token_1"));

        auto alice_after = repo.FindActiveSubscription(service_url, entity_set_name, "alice_secret");
        auto bob_after = repo.FindActiveSubscription(service_url, entity_set_name, "bob_secret");
        REQUIRE(alice_after->delta_token == "alice_token_1");
        REQUIRE(bob_after->delta_token.empty());
    }

    SECTION("a secret nobody subscribed under finds nothing rather than someone else's row") {
        repo.CreateSubscription(service_url, entity_set_name, "alice_secret");

        auto found = repo.FindActiveSubscription(service_url, entity_set_name, "carol_secret");
        REQUIRE_FALSE(found.has_value());
    }

    SECTION("switching secrets starts a fresh subscription with no inherited delta position") {
        // The old position was established under credentials the caller is no longer using,
        // so resuming from it would be a claim we cannot support. A full load is the honest
        // outcome, and it is what an empty delta token produces.
        const std::string first = repo.CreateSubscription(service_url, entity_set_name, "old_secret");
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(first, std::string(), "token_from_old_credentials"));

        const std::string second = repo.CreateSubscription(service_url, entity_set_name, "new_secret");
        REQUIRE(second != first);

        auto found = repo.FindActiveSubscription(service_url, entity_set_name, "new_secret");
        REQUIRE(found.has_value());
        REQUIRE(found->delta_token.empty());

        // and the original is untouched, not reused or cleared
        auto original = repo.FindActiveSubscription(service_url, entity_set_name, "old_secret");
        REQUIRE(original.has_value());
        REQUIRE(original->delta_token == "token_from_old_credentials");
    }

    SECTION("an unnamed secret is the 'default' one, consistently on both write and read") {
        const std::string id = repo.CreateSubscription(service_url, entity_set_name, "");

        REQUIRE(repo.FindActiveSubscription(service_url, entity_set_name, "").has_value());
        REQUIRE(repo.FindActiveSubscription(service_url, entity_set_name, "default").has_value());
        REQUIRE(repo.FindActiveSubscription(service_url, entity_set_name, "")->subscription_id == id);
    }
}

TEST_CASE("OdpSubscriptionRepository - a v1 state table migrates without losing delta tokens",
          "[odp_repository][odp_schema_migration]") {
    odp_test::TempDatabase temp_db;
    Connection& conn = temp_db.Conn();

    // Build the v1 layout by hand: the pre-#236 unique key, and rows stamped schema_version 1.
    REQUIRE_NOTHROW(conn.Query("CREATE SCHEMA IF NOT EXISTS erpl_web"));
    auto create = conn.Query(
        "CREATE TABLE erpl_web.odp_subscriptions ("
        "subscription_id VARCHAR PRIMARY KEY, service_url VARCHAR NOT NULL, "
        "entity_set_name VARCHAR NOT NULL, secret_name VARCHAR, delta_token VARCHAR, "
        "created_at TIMESTAMP DEFAULT NOW(), last_updated TIMESTAMP DEFAULT NOW(), "
        "subscription_status VARCHAR DEFAULT 'active', preference_applied BOOLEAN DEFAULT FALSE, "
        "schema_version INTEGER NOT NULL DEFAULT 1, "
        "UNIQUE (service_url, entity_set_name))");
    REQUIRE_FALSE(create->HasError());

    // Three shapes that exist in the wild: a named secret, an empty one, and a NULL one
    // from before the column was written at all.
    auto insert = conn.Query(
        "INSERT INTO erpl_web.odp_subscriptions "
        "(subscription_id, service_url, entity_set_name, secret_name, delta_token, "
        " subscription_status, preference_applied, schema_version) VALUES "
        "('id_named', 'https://sap.test/TEST_SRV/EntityOfA', 'EntityOfA', 'prod_secret', 'TOKEN_A', "
        " 'active', TRUE, 1), "
        "('id_empty', 'https://sap.test/TEST_SRV/EntityOfB', 'EntityOfB', '', 'TOKEN_B', "
        " 'active', FALSE, 1), "
        "('id_null',  'https://sap.test/TEST_SRV/EntityOfC', 'EntityOfC', NULL, 'TOKEN_C', "
        " 'active', FALSE, 1)");
    REQUIRE_FALSE(insert->HasError());

    OdpSubscriptionRepository repo(temp_db.Context());
    REQUIRE_NOTHROW(repo.EnsureTablesExist());

    SECTION("every delta token survives the migration") {
        // This is the whole point of migrating rather than telling the operator to drop the
        // schema: a lost token means a full re-extraction of an SAP source.
        auto named = repo.FindActiveSubscription("https://sap.test/TEST_SRV/EntityOfA", "EntityOfA",
                                                 "prod_secret");
        REQUIRE(named.has_value());
        REQUIRE(named->delta_token == "TOKEN_A");
        REQUIRE(named->subscription_id == "id_named");
        REQUIRE(named->preference_applied == true);
    }

    SECTION("rows with an empty or NULL secret become the 'default' one") {
        auto from_empty = repo.FindActiveSubscription("https://sap.test/TEST_SRV/EntityOfB",
                                                      "EntityOfB", "default");
        REQUIRE(from_empty.has_value());
        REQUIRE(from_empty->delta_token == "TOKEN_B");

        auto from_null = repo.FindActiveSubscription("https://sap.test/TEST_SRV/EntityOfC",
                                                     "EntityOfC", "default");
        REQUIRE(from_null.has_value());
        REQUIRE(from_null->delta_token == "TOKEN_C");
    }

    SECTION("the migrated table carries the v2 unique key") {
        auto constraints = conn.Query(
            "SELECT count(*) FROM duckdb_constraints() WHERE schema_name = 'erpl_web' "
            "AND table_name = 'odp_subscriptions' AND constraint_type = 'UNIQUE' "
            "AND list_contains(constraint_column_names, 'secret_name')");
        REQUIRE_FALSE(constraints->HasError());
        REQUIRE(duckdb_compat::ResultValue(*constraints, 0, 0).GetValue<int64_t>() > 0);

        // and the new key is actually enforced: same pair, second secret, both rows kept
        REQUIRE_NOTHROW(repo.CreateSubscription("https://sap.test/TEST_SRV/EntityOfA", "EntityOfA",
                                                "other_secret"));
        auto original = repo.FindActiveSubscription("https://sap.test/TEST_SRV/EntityOfA",
                                                    "EntityOfA", "prod_secret");
        REQUIRE(original.has_value());
        REQUIRE(original->delta_token == "TOKEN_A");
    }

    SECTION("the scratch table used during the migration is cleaned up") {
        auto leftovers = conn.Query(
            "SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'erpl_web' "
            "AND table_name LIKE '%_v1_migrating'");
        REQUIRE_FALSE(leftovers->HasError());
        REQUIRE(duckdb_compat::ResultValue(*leftovers, 0, 0).GetValue<int64_t>() == 0);
    }

    SECTION("migrating is idempotent - a second open leaves the table alone") {
        OdpSubscriptionRepository reopened(temp_db.Context());
        REQUIRE_NOTHROW(reopened.EnsureTablesExist());

        auto still_there = reopened.FindActiveSubscription("https://sap.test/TEST_SRV/EntityOfA",
                                                           "EntityOfA", "prod_secret");
        REQUIRE(still_there.has_value());
        REQUIRE(still_there->delta_token == "TOKEN_A");
    }
}

TEST_CASE("OdpSubscriptionRepository - reactivation keeps a resumable delta position",
          "[odp_repository][odp_resumable]") {
    odp_test::TempDatabase temp_db;
    OdpSubscriptionRepository repo(temp_db.Context());

    const std::string service_url = "https://sap.test/TEST_SRV/EntityOfResume";
    const std::string entity_set_name = "EntityOfResume";
    const std::string secret = "prod_secret";

    SECTION("an errored subscription resumes from where it stopped") {
        // 'error' means an extraction failed for a reason that does not invalidate the delta
        // position. Clearing the token here turned a transient network blip into a full
        // re-extraction of an SAP source, which is the expense #236 exists to avoid.
        const std::string id = repo.CreateSubscription(service_url, entity_set_name, secret);
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(id, "", "POSITION_BEFORE_THE_BLIP"));
        REQUIRE(repo.UpdateSubscriptionStatus(id, "error"));

        const std::string reactivated = repo.CreateSubscription(service_url, entity_set_name, secret);
        REQUIRE(reactivated == id);

        auto found = repo.FindActiveSubscription(service_url, entity_set_name, secret);
        REQUIRE(found.has_value());
        REQUIRE(found->subscription_status == "active");
        REQUIRE(found->delta_token == "POSITION_BEFORE_THE_BLIP");
    }

    SECTION("a terminated subscription does not - its queue is gone") {
        // TerminateDeltasFor* drops the ODQ queue behind the subscription, so resuming from
        // its token would silently skip rows. This one must still start over.
        const std::string id = repo.CreateSubscription(service_url, entity_set_name, secret);
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(id, "", "TOKEN_FOR_A_DEAD_QUEUE"));
        REQUIRE(repo.UpdateSubscriptionStatus(id, "terminated"));

        const std::string reactivated = repo.CreateSubscription(service_url, entity_set_name, secret);
        REQUIRE(reactivated == id);

        auto found = repo.FindActiveSubscription(service_url, entity_set_name, secret);
        REQUIRE(found.has_value());
        REQUIRE(found->subscription_status == "active");
        REQUIRE(found->delta_token.empty());
        // preference_applied is reset with the position, since the new load re-establishes it
        REQUIRE(found->preference_applied == false);
    }

    SECTION("resuming still advances from the preserved token, not from empty") {
        // The advance is a compare-and-swap against the stored value, so a preserved token
        // has to be the value the next advance expects -- otherwise resuming would throw.
        const std::string id = repo.CreateSubscription(service_url, entity_set_name, secret);
        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(id, "", "POSITION_1"));
        REQUIRE(repo.UpdateSubscriptionStatus(id, "error"));
        repo.CreateSubscription(service_url, entity_set_name, secret);

        REQUIRE_NOTHROW(repo.AdvanceDeltaToken(id, "POSITION_1", "POSITION_2"));

        auto found = repo.FindActiveSubscription(service_url, entity_set_name, secret);
        REQUIRE(found->delta_token == "POSITION_2");
    }
}
