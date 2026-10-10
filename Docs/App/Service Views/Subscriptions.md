Subscriptions service view (a device with a subscription capability).
### Appbar
- Title "Subscriptions - <device name>"
- Refresh button
### Tabs
- **Provider (incoming)** — the tables this device serves to others. Hidden/short message
  when the device has no provider capability. Each entry expands to trigger, period,
  min interval, deadzone, last sent (uptime), requester address, TRID and hash.
- **Requester (outgoing)** — subscriptions this device requests (shown when the device has the
  Subscriptions Request capability). Each entry
  expands to provider, trigger, period, min interval and deadzone, with Edit and Delete.
  An "Add Subscription" button creates a new one; the create/edit dialog picks source and
  target block/field and the trigger parameters.
