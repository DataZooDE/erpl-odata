# Outlook and Teams via Microsoft Graph

## Outlook

Permissions: `Mail.Read`, `Calendars.Read`, `Contacts.Read`, Application or Delegated.
With an app-only secret pass `user := 'upn, email or GUID'`; with a delegated secret omit
it to read `/me`.

```sql
SELECT id, name, is_default_calendar FROM graph_calendars(user := 'jr@contoso.com', secret := 'ms_graph');

-- Events in a date window (calendarView); start_date and end_date go together
SELECT subject, start_time, end_time, organizer_name, location, is_all_day
FROM graph_calendar_events(user := 'jr@contoso.com',
                           start_date := '2025-01-01', end_date := '2025-03-31',
                           secret := 'ms_graph');

-- A specific calendar
SELECT subject, start_time FROM graph_calendar_events(user := 'jr@contoso.com',
                                                      calendar_id := 'calendar-guid', secret := 'ms_graph');

SELECT display_name, email, company_name, job_title FROM graph_contacts(user := 'jr@contoso.com');

SELECT display_name, total_item_count, unread_item_count
FROM graph_outlook_mail_folders(user := 'jr@contoso.com');

-- Mail metadata only (no bodies). folder: inbox, sentitems, drafts, deleteditems,
-- junkemail, outbox, archive, or any display name from graph_outlook_mail_folders()
SELECT subject, from_name, from_email, received_at, importance, is_read
FROM graph_outlook_emails(user := 'jr@contoso.com', folder := 'inbox')
ORDER BY received_at DESC;
```

Typical uses: meeting-load analysis per team, response-time statistics on a shared
mailbox, contact list exports.

## Teams

Permissions: `Team.ReadBasic.All`, `Channel.ReadBasic.All`, `ChannelMessage.Read.All`,
`TeamMember.Read.All`.

```sql
-- Teams of a user (app-only needs user :=; delegated reads /me/joinedTeams)
SELECT id, display_name, description FROM graph_my_teams(user := 'jr@contoso.com');

SELECT id, display_name, membership_type FROM graph_teams_channels('team-id');
SELECT id, display_name, role            FROM graph_teams_members('team-id');

SELECT id, created_datetime, from_name, body_content, importance
FROM graph_channel_messages('team-id', 'channel-id')
ORDER BY created_datetime DESC;
```

`ChannelMessage.Read.All` is a protected API in Microsoft Graph: besides admin consent,
Microsoft may require the app to be approved for it. Channel message bodies are HTML.
