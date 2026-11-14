# Entra ID (Azure AD) via Microsoft Graph

Permissions (Application, admin-consented): `User.Read.All`, `Group.Read.All`,
`Device.Read.All`. `graph_signin_logs()` also needs `AuditLog.Read.All` and an Entra ID
P1/P2 licence.

```sql
SELECT id, display_name, mail, job_title, department, account_enabled FROM graph_users();

SELECT id, display_name, mail_enabled, security_enabled FROM graph_groups() WHERE security_enabled;

SELECT id, display_name, operating_system, os_version, trust_type FROM graph_devices();

SELECT user_display_name, app_display_name, ip_address, status, created_datetime
FROM graph_signin_logs()
WHERE status <> 'Success'
ORDER BY created_datetime DESC;
```

## Audit recipes

```sql
-- Disabled accounts that still own Planner plans or appear in Teams
SELECT u.display_name, u.mail
FROM graph_users() u
WHERE NOT u.account_enabled;

-- Failed sign-ins per user and app over the retention window
SELECT user_display_name, app_display_name, count(*) AS failures
FROM graph_signin_logs()
WHERE status <> 'Success'
GROUP BY ALL
ORDER BY failures DESC
LIMIT 20;

-- Departments without a manager-type job title (data quality on the directory)
SELECT department, count(*) AS users
FROM graph_users()
GROUP BY department
HAVING count(*) FILTER (WHERE job_title ILIKE '%manager%') = 0;
```

Directory data is personal data. Land it in a protected database, and mask it before
sharing (anofox-tabular plugin, `anofox-tabular-pii` skill).
