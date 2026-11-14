# Excel workbooks (OneDrive / SharePoint) via Microsoft Graph

Permission: `Files.ReadWrite.All`, also for reads, because the Graph workbook service
(WAC) requires write access to open a session.

Locate a workbook with `site := 'Finance'` plus `drive := 'Documents'` (names), or with the
raw `drive_id := 'b!...'`. Without either, paths are resolved against the app's or user's
OneDrive.

## Discover

```sql
-- Files in a drive or folder
SELECT id, name, web_url, size, modified_at, is_folder
FROM graph_show_files(site := 'Finance', drive := 'Documents', secret := 'ms_graph');

-- Worksheets and named tables in a workbook
SELECT name, id, position, visibility
FROM graph_excel_worksheets('Budget.xlsx', site := 'Finance', drive := 'Documents', secret := 'ms_graph');

SELECT name, id, show_headers, show_totals
FROM graph_excel_tables('Budget.xlsx', site := 'Finance', drive := 'Documents', secret := 'ms_graph');
```

## Read

```sql
-- A named Excel table (preferred: typed by header, stable as the sheet grows)
SELECT * FROM graph_excel_read('Budget.xlsx', 'SalesData',
                               site := 'Finance', drive := 'Documents', secret := 'ms_graph');

-- The used range of a worksheet (when the data is not formatted as a table)
SELECT * FROM graph_excel_range('Budget.xlsx', 'Sheet1',
                                site := 'Finance', drive := 'Documents', secret := 'ms_graph');
```

Prefer named tables. A used range includes stray cells and headers are guessed; a table
has explicit headers and boundaries. Ask the workbook owner to *Format as Table* if needed.

## Write

Append rows with an attached workbook. Its named tables appear as DuckDB tables:

```sql
ATTACH 'Budget.xlsx' AS budget (TYPE excel_workbook, SECRET 'ms_graph', SITE 'Finance', DRIVE 'Documents');
SHOW TABLES FROM budget;

INSERT INTO budget.main."SalesData"
SELECT region, product, revenue FROM local_sales WHERE quarter = 'Q2';
```

Or with `COPY`, which appends a query result to a named table:

```sql
COPY (SELECT region, product, revenue FROM local_sales WHERE quarter = 'Q2')
TO 'Budget.xlsx' (FORMAT graph_excel_table, table 'SalesData', secret 'ms_graph',
                  site 'Finance', drive 'Documents');
```

Or with the function form, giving rows as a JSON 2-D array in the table's column order
(check the order with `graph_excel_tables`):

```sql
SELECT rows_added
FROM graph_excel_add_rows('Budget.xlsx', 'SalesData',
                          data := '[["North","Widget",1200],["South","Gadget",800]]',
                          secret := 'ms_graph');
```

Delete every row whose column equals a value (column by header name or 0-based index; the
value is compared as a string):

```sql
SELECT rows_deleted
FROM graph_excel_delete_rows('Budget.xlsx', 'SalesData', 'Region', 'North',
                             site := 'Finance', secret := 'ms_graph');
```

## Gotchas

- Column order matters for `graph_excel_add_rows` and `COPY`: values are written
  positionally into the table's columns.
- Excel has no types beyond number, text, boolean and date serials. Cast dates and
  decimals explicitly on the way in and out.
- The Graph workbook API is slow for big tables. Keep bulk data in DuckDB or Parquet and
  put only summaries into Excel.
- For reading `.xlsx` *files* as a whole (not through the workbook API), the
  duckdb-sharepoint extension's `sharepoint-excel` skill, or DuckDB's own `excel` extension
  on a downloaded file, may fit better.
