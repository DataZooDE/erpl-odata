# Planner via Microsoft Graph

Permissions: `Tasks.Read.All`, `Group.Read.All` to read; `Tasks.ReadWrite.All` to create.
Plans belong to Microsoft 365 groups; find the group id with `graph_groups()`.

```sql
SELECT id, title, owner_group_id, created_at FROM graph_planner_plans('group-id');
SELECT id, name, plan_id, order_hint       FROM graph_planner_buckets('plan-id');

SELECT id, title, bucket_id, percent_complete, priority, due_at
FROM graph_planner_tasks('plan-id')
WHERE percent_complete < 100
ORDER BY due_at;
```

## Creating tasks

`graph_planner_create_task(plan_id, title, ...)` is a table function returning
`(task_id, task_url)`. **`plan_id` and `title` are positional**; everything else is named.

```sql
SELECT task_id, task_url
FROM graph_planner_create_task(
    'plan-id',
    'Review Q2 budget',
    bucket_id        := 'Backlog',        -- bucket name or GUID
    due_date         := '2025-07-31',     -- 'YYYY-MM-DD' or ISO 8601
    start_date       := '2025-07-01',
    assigned_to      := 'user-object-id',
    description      := 'Check the finance sheet before the board meeting.',
    priority         := 3,                -- 1 urgent, 3 important, 5 medium, 9 low
    percent_complete := 0,
    secret           := 'ms_graph'
);
```

### One task per row of a query

Arguments must be constants, so `LATERAL graph_planner_create_task(t.title, ...)` is
rejected. Generate one statement per row and run the file, in the DuckDB CLI:

```sql
CREATE TABLE IF NOT EXISTS created_tasks (invoice_no VARCHAR, task_id VARCHAR, task_url VARCHAR);
.mode list
.headers off
.once create_tasks.sql
SELECT format('INSERT INTO created_tasks SELECT {0}, task_id, task_url FROM graph_planner_create_task(''plan-id'', {1}, bucket_id := ''Collections'', due_date := ''{2}'', secret := ''ms_graph'');',
              quote(invoice_no), quote('Chase invoice ' || invoice_no), strftime(current_date + 7, '%Y-%m-%d'))
FROM overdue_invoices
WHERE invoice_no NOT IN (SELECT invoice_no FROM created_tasks);
.read create_tasks.sql
```

The `NOT IN (SELECT ... FROM created_tasks)` makes a re-run idempotent: every execution of
a create statement creates the task again, so record what was created and skip it next time.
