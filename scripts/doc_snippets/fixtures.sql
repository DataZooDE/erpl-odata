-- Prepended to every executed snippet. Snippets run in a fresh temp directory, so this
-- file-backed database is new each time. ODP state (odp_odata_list_subscriptions and
-- friends) refuses an in-memory catalog, which is why the default is a file.
ATTACH 'doc_snippets.db' AS doc_snippets;
USE doc_snippets;
-- Creates the erpl_web schema (subscriptions + audit tables), so docs that query
-- erpl_web.odp_subscription_audit run as they would after a first ODP read.
SELECT count(*) AS _odp_state_initialised FROM odp_odata_list_subscriptions();
