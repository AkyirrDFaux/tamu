# Improvement suggestions

## Resolved 2026-08-24

- SNDB deletion is now covered by the updated Docs/Services/Device service.md:
  SNDB Write (CID 14) with ID = 0 deletes the entry carrying the serial number.
  Firmware and app follow this exactly; the temporary CID 15 was removed again.
- Block-type editing via the block-level Write (field index invalid) is implemented
  in both memory services per "type is user editable"; consider noting it next to
  the Write row in both CID tables.
