# Improvement suggestions

## New 2026-08-25 (consolidation/audit round)

- **Data Formats.md ID model is internally inconsistent**: it says "16bit
  (4 bit net + 12 bit device)" but also defines Broadcast as 0xFFFFFFFF (32-bit), and the
  firmware/app use flat 16-bit addresses (Broadcast 0xFFFF) with dense IDs from 2 - no
  net/device split anywhere. Pick one model (the implementation follows the 16-bit one).
- **Keyed Memory.md only lists CIDs 0-6** but the firmware (and app) implement CID 7
  "read all entries of a dictionary in one round trip" - document it next to the other CIDs.
- **Docs/App/Backup.md promises more than the app implements**: per-part selection of synced
  items, cross-device sync of compatible targets, and file-system (not just System Memory)
  backup. The app currently archives/restores System Memory blocks only (per-device JSON
  zip + live restore). Either trim the doc to what exists or track the rest as a feature.
- **Docs/App/Service views/Storage.md mentions uploading files** from the host; the app only
  downloads/previews (the firmware has Write Stream Open/Close/Write CIDs 7/8/64+, so upload
  is implementable).
- **Docs/App/Settings.md describes in-app and OS notifications**, but the app only persists
  the preference toggles - nothing consumes them. Either implement the notification feed or
  mark the feature not-yet-implemented in the doc.
- **Docs/App/Device view.md lists a Router table viewer and Script editor**; both wait on
  their (not-yet-implemented) firmware services. The capability display also shows all six
  service bits while the doc says "Show only non-services" - clarify what that means (all
  current capability bits ARE service bits).
- **Docs/App/Devices.md wants routers below the core in a tree**; the Router service is not
  implemented, so the graph currently splits core-vs-others. Revisit when routers exist.

## Resolved 2026-08-24

- SNDB deletion is now covered by the updated Docs/Services/Device service.md:
  SNDB Write (CID 14) with ID = 0 deletes the entry carrying the serial number.
  Firmware and app follow this exactly; the temporary CID 15 was removed again.
- Block-type editing via the block-level Write (field index invalid) is implemented
  in both memory services per "type is user editable"; consider noting it next to
  the Write row in both CID tables.
