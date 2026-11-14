-- Prepended to every executed snippet. Snippets run in a fresh temp directory, so this
-- file-backed database is new each time. ODP state (odp_odata_list_subscriptions and
-- friends) refuses an in-memory catalog, which is why the default is a file.
ATTACH 'doc_snippets.db' AS doc_snippets;
USE doc_snippets;
