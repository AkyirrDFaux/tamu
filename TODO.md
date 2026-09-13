# TODO

## Next (reduced subscription service)
- [x] `TriggerType` -> 3 enums (firmware + app)
- [x] Reduced provider entry (28 B wire) + FNV-1a hash on-change detection, main-loop checked
- [x] Reduced requester entry (24 B wire), index-based table, raw-byte value updates + hash confirmation
- [x] DAS as provider (`USE_SUB_PROVIDE`, 4-entry table, `Node | Subscriptions` capability, `SubscriptionsTick` in the loop)
- [x] App: reduced models, wire (24/28-byte entries, CID 1/4 with trigger+pad), UI drops counter/tolerance/edge/delta
- [x] Verified: cross-device (DAS temp -> Tamu field), self-loopback (Uptime -> SubExample), requester persistence + boot re-registration, OnChangeConfirm stops after confirmation
- [x] HIL + unit suites green; both firmware targets build + fit

## Backlog
- Replaces the `Tlvf`/`Tlvf.fromBytes` leftovers and the `DataType`/`int32*` helpers in the app? (the raw-value wire no longer needs them; a few helpers are still used elsewhere)
- Consider making the app warn when a subscription's source/target data types mismatch (raw bytes are type-less on the wire)
- Clean up `test/debug_register_cache.dart` (see Issues.md)
- Review the DAS flash slack (~3.9 KB) if the subscription provider table should grow beyond 4 entries