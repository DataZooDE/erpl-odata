# SharePoint lists via Microsoft Graph

Permissions: `Sites.Read.All` to read; `Sites.ReadWrite.All` to create, update or delete
items. Sites and lists accept a display name, a URL or a GUID.

## Discover

```sql
SELECT id, display_name, web_url FROM graph_show_sites();

SELECT id, name, drive_type, web_url FROM graph_show_drives(site := 'Finance', secret := 'ms_graph');

SELECT id, name, display_name, description FROM graph_show_lists(site := 'Finance', secret := 'ms_graph');

-- Columns of a list: internal name, type, required
SELECT name, column_type, required FROM graph_describe_list('Finance', 'Budget');
```

## Read

```sql
-- The whole list is fetched; WHERE is applied by DuckDB after the rows arrive
SELECT * FROM graph_sharepoint_list_read('Finance', 'Budget', secret := 'ms_graph')
WHERE status = 'Active';

SELECT * FROM graph_sharepoint_list_read('https://contoso.sharepoint.com/sites/Finance',
                                         'Project Tracker', secret := 'ms_graph');
```

Or attach a whole site; each list becomes a table and is fetched lazily:

```sql
ATTACH 'Finance' AS finance_sp (TYPE sharepoint_lists, SECRET 'ms_graph');
SHOW TABLES FROM finance_sp;
SELECT * FROM finance_sp.main."Project Tracker" WHERE status = 'Active';
```

## Write

Through the attached catalog, with ordinary DML:

```sql
INSERT INTO finance_sp.main."Budget" (Title, Status, Amount) VALUES ('New entry', 'Draft', 1500);
UPDATE finance_sp.main."Budget" SET Status = 'Approved' WHERE Title = 'New entry';
DELETE FROM finance_sp.main."Budget" WHERE Status = 'Obsolete';
```

With `COPY`, appending a query result to a list (the site is the `TO` path):

```sql
COPY (SELECT title AS Title, amount AS Amount FROM staging_items)
TO 'Finance' (FORMAT graph_sharepoint_list, list 'Budget', secret 'ms_graph');
```

With functions:

```sql
-- Table function, returns item_id. Fields as a JSON object of internal column names
SELECT item_id
FROM graph_sharepoint_create_item('Finance', 'Budget',
                                  '{"Title": "New Entry", "Status": "Draft", "Amount": 1500}',
                                  secret := 'ms_graph');

-- Scalars returning BOOLEAN; the secret is the LAST POSITIONAL argument
SELECT graph_sharepoint_update_item('Finance', 'Budget', '42', '{"Status": "Approved"}', 'ms_graph');
SELECT graph_sharepoint_delete_item('Finance', 'Budget', '42', 'ms_graph');
```

For bulk creation use the attached catalog (`INSERT INTO ... SELECT`) or `COPY ... TO`
above. `LATERAL graph_sharepoint_create_item(t.col, ...)` is rejected, because table-function
arguments must be constants. The scalar update and delete functions do work per row:

```sql
SELECT id, graph_sharepoint_update_item('Finance', 'Budget', id, '{"Status": "Archived"}', 'ms_graph') AS ok
FROM stale_items;
```

## Gotchas

- Field names in JSON are the list's **internal** column names (`graph_describe_list`),
  which differ from display names when the column was renamed or contains spaces
  (`Project_x0020_Name`).
- There is no filter pushdown: every read pages through the whole list (projection does
  push down on attached tables). For large lists, land the list once into a DuckDB table and query that.
- Lookup and person columns come back as IDs. Join them with the target list or with
  `graph_users()`.
