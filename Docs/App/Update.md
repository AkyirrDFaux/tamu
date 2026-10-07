Update the firmware of the connected core or of a node on its bus (Bootloader service).
### How to update
- Choose the target's main binary (`.bin`, `.img`).
- Put the target in bootloader mode (hold its button while resetting/powering it on).
- Tap Update; writes are broadcast on the core's bus, then read back and corrected until
  verified (the node gives no write acknowledgement, verification confirms the image).
- Reset/power-cycle the device to run the new firmware. A failed update is safe to retry.
### Sections
- **How to update** — the steps above.
- **Target** — link state, the core, a "Bootloader probe" result, a "Check for bootloader"
  button (reads offset 0; only a device in bootloader mode answers), and a switch
  "Update the connected core directly (USB)" (off = relay to a node on the bus; on = flash
  the core itself).
- **Image** — chosen file name/size and a "Choose file" button.
- **Update** — a progress bar/phase (Writing / Verifying / Correcting / Done) and the
  verified/corrections result, plus the Update button.
- **Log** — a running log of the operation.
