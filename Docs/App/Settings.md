Settings screen, grouped into sections.
### Connection
- Autoconnect to specified device (on/off); its label reads "Autoconnect: <target>" once a
  target is set.
- Autoconnect device row showing the target or a hint, with a Clear button (clears the on/off
  flag and the target).
- The target is set with the star button on a device in the Connection page.
### Notifications (app open)
- Allow notifications (on/off)
	- Per event selection (Device discovered, Device lost, Backup finished), shown only when enabled
### Operating system notifications
- If app open do not notify OS (true/false)
- Allow notifications (to OS) (on/off)
	- Per event selection (same event list), shown only when enabled
- Note: the app persists these settings but currently only delivers in-app notifications
  (`app/lib/core/notifications.dart`); OS delivery is pending (Issues.md).
### About
- App version
- App compile date
