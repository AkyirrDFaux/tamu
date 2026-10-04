# Display layout files

Layout files for the LED display blocks (Docs/Modules and blocks/LED display.md). Each file maps a
W x H logical grid to physical LED strip positions.

## Format

| Brightness limit | Display width | Display Height | Table of LED indexes |
| ---------------- | ------------- | -------------- | -------------------- |
| u8 (0-255)       | u8            | u8             | W*H x u16 LE         |

- `brightness limit` is a percentage in 0-255 units (178 = 70%): a cap on the block's
  `Brightness` setting, held by the firmware so a stored scene cannot exceed it.

- `width` / `height` in LED cells.
- Then `width * height` little-endian uint16 values, row-major (row 0 first).
- Each value is a **0-based LED buffer index** for that display's strip, or `0xFFFF`
  for an unused (empty) position.

## Files

- `Vysi v1.0.lay` — the 11x10 layout of the Tamu v2.0A's Vysi v1.0 LED display. Its
  contents match the firmware's compiled-in default (`LayoutVysiv1_0` in
  `firmware/src/Blocks/Vysi1Display.h`), so the display renders identically whether the
  `LayoutFile` field is blank (built-in default) or set to this file. The file is
  preloaded to the Tamu's storage at boot (name `LAY_1`, 223 bytes) when it is missing.