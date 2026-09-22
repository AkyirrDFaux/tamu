# Issues

## System block Net ID (field 7) cannot be written via Register
- `RegisterGetSystemField` exposes field 7 (Net ID) as `Id | Persistent` (writable in
  principle), but `HandleSystemBlockWrite` only accepts field 6 (Name); a write to field 7
  returns failure.
- Impact: the backup captures the Net ID (complete register snapshot) but the restore plan
  marks it unavailable ("Identity field") because the write would fail; see `RestorePlan`
  in `app/lib/core/backup.dart`.
- Fix: extend `HandleSystemBlockWrite` to apply a Net ID write (and re-register on the bus)
  if restoring identity is desired.
