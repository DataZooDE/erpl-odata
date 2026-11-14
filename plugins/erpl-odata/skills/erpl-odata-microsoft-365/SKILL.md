---
name: erpl-odata-microsoft-365
description: >
  Using Microsoft 365 as a data source and sink from DuckDB with erpl_odata's
  Microsoft Graph functions: Excel workbooks on OneDrive/SharePoint
  (graph_show_files, graph_excel_worksheets, graph_excel_tables,
  graph_excel_read, graph_excel_range, graph_excel_add_rows,
  graph_excel_delete_rows, ATTACH TYPE excel_workbook, COPY TO FORMAT
  graph_excel_table), SharePoint lists (graph_show_sites, graph_show_drives,
  graph_show_lists, graph_describe_list, graph_sharepoint_list_read,
  graph_sharepoint_create_item / update_item / delete_item, ATTACH TYPE
  sharepoint_lists with INSERT/UPDATE/DELETE, COPY TO FORMAT
  graph_sharepoint_list), Planner (plans, buckets, tasks, creating tasks
  from SQL), Outlook mail, calendars and contacts, Teams channels and
  messages, and Entra ID users, groups, devices and sign-in logs; app-only
  vs delegated permissions. Use when the user wants to read or write Excel or
  SharePoint data, push query results into a list or workbook, create Planner
  tasks from data, or report on M365 directory, mail or Teams activity.
version: 2026.10.24
user-invocable: false
---

# ERPL-OData — Microsoft 365 (Microsoft Graph)

**Extension:** `erpl_odata` v2026.10.24 | **DuckDB:** v1.4.5 LTS / v1.5.6 / v2.0

All functions here call Microsoft Graph with a `microsoft_graph` secret (see
`erpl-odata-setup`). Every function accepts `secret := 'name'`; without it the first
`microsoft_graph` secret is used.

## Which area, which reference

| Task | Reference file |
|---|---|
| Read Excel tables and ranges, append or delete rows, attach a workbook | `references/excel.md` |
| Read, filter and write SharePoint lists, attach a site | `references/sharepoint-lists.md` |
| Planner plans, buckets and tasks; bulk-create tasks from a query | `references/planner.md` |
| Outlook mail metadata, calendars, contacts; Teams channels and messages | `references/outlook-teams.md` |
| Entra ID users, groups, devices, sign-in audits | `references/entra-id.md` |

Read the reference for the task before writing SQL; the parameter names differ per area.

## Critical gotchas

1. **App-only vs delegated.** A `client_credentials` secret acts as the app: mail,
   calendar and Teams functions then need `user := 'upn-or-guid'`, because there is no
   `/me`. A `PROVIDER authorization_code` secret acts as the signed-in user, so omit `user`
   to get `/me/...`.
2. **Permissions are per area and need admin consent.** Application permissions on the app
   registration, granted by an admin:

   | Area | Read | Write |
   |---|---|---|
   | Excel | `Files.ReadWrite.All` (the Excel service requires write even to read) | same |
   | SharePoint lists, sites, drives | `Sites.Read.All` | `Sites.ReadWrite.All` |
   | Planner | `Tasks.Read.All`, `Group.Read.All` | `Tasks.ReadWrite.All` |
   | Outlook | `Mail.Read`, `Calendars.Read`, `Contacts.Read` | n/a |
   | Teams | `Team.ReadBasic.All`, `Channel.ReadBasic.All`, `ChannelMessage.Read.All`, `TeamMember.Read.All` | n/a |
   | Entra ID | `User.Read.All`, `Group.Read.All`, `Device.Read.All`; sign-ins add `AuditLog.Read.All` and Entra ID P1/P2 | n/a |

   A 403 from Graph almost always means a missing permission or missing consent.
3. **Names, URLs and IDs are interchangeable** for sites, lists and drives: `'Finance'`,
   `'https://contoso.sharepoint.com/sites/Finance'` and the raw GUID all resolve. Names are
   resolved with an extra call; use IDs in hot loops.
4. **Write functions are one-shot per bound statement.** `graph_excel_add_rows` and the
   create functions write when the statement runs and return what they created. Re-running
   the statement writes again, so do not put them in a view.
5. **Scalar vs table writers.** `graph_sharepoint_create_item` and
   `graph_planner_create_task` are **table** functions whose arguments must be constants:
   no `LATERAL` over a query. For bulk work use `INSERT` into an attached list, `COPY ... TO`,
   or generate one statement per row (see `references/planner.md`).
   `graph_sharepoint_update_item` and `graph_sharepoint_delete_item` are **scalar**,
   return `BOOLEAN`, and take the secret as the **last positional** argument, not `secret :=`.
6. **Outlook reads metadata only.** `graph_outlook_emails` returns subject, sender, dates,
   flags; no message bodies.
7. **duckdb-sharepoint is a separate extension.** For SharePoint and OneDrive *files* as a
   filesystem (`sharepoint://` paths, globbing, `read_csv` / `read_parquet` on documents,
   file versions, change feeds), use the duckdb-sharepoint extension and its
   `sharepoint-files` skill. erpl_odata covers lists, Excel tables through the Graph
   workbook API, and the rest of M365.

## Canonical workflow: Excel and SharePoint as a business data hub

```sql
CREATE SECRET ms_graph (TYPE microsoft_graph, TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...');

-- 1. Read a budget table maintained by the finance team in Excel
CREATE OR REPLACE TABLE budget AS
SELECT * FROM graph_excel_read('Budget 2025.xlsx', 'BudgetTable',
                               site := 'Finance', drive := 'Documents');

-- 2. Compare with actuals from the ERP (for example landed with erpl-odata-sap-gateway)
CREATE OR REPLACE TABLE variance AS
SELECT b.cost_center, b.amount AS budget, a.amount AS actual, a.amount - b.amount AS delta
FROM budget b JOIN actuals a USING (cost_center);

-- 3. Raise a SharePoint list item per cost center over budget
ATTACH 'Finance' AS finance_sp (TYPE sharepoint_lists, SECRET 'ms_graph');
INSERT INTO finance_sp.main."Budget Alerts" (Title, CostCenter, Delta)
SELECT 'Over budget: ' || cost_center, cost_center, delta FROM variance WHERE delta > 0;
```

See also: `erpl-odata-setup` (Graph secret providers), `erpl-odata-production`
(unattended tokens, error playbook), `erpl-odata-dynamics-and-delta-sharing` (Business
Central and Dataverse, which use their own secret types). For PII in directory or mail
exports, the anofox-tabular plugin's `anofox-tabular-pii` skill detects and masks it.

Reference docs:
- `README.md` (Microsoft 365 & Dynamics 365)
