# Real Hardware Verification - Tamu v2.0A + DAS v0.1

Final verification state 2026-08-23. Two devices on the RS-485 RSBus:

- **Tamu v2.0A** (ESP32-C3, USB `/dev/ttyACM0`, console + RSBus) - core, SNDB ID 1, SN `E4B063C820700000000000000000`.
- **DAS v0.1** (CH32V003, WCH-Link `/dev/ttyACM1`, no console) - SNDB ID 2, SN `CDABBD6400000000000000000000`.

The DAS has no console; it is reached exclusively over the RSBus through the Tamu console
(DAS upload: `pio run -e DAS_v0_1 -t upload --upload-port /dev/ttyACM1`). Re-flashing the DAS
rewrites the whole chip flash, so its storage filesystem is reformatted on every upload.

## Session issues and their resolution

| # | Issue | Resolution |
| --- | --- | --- |
| 1 | Deleted files `ZERO`(0)/`ALPHA`(100) reappeared in the DAS table at session start | **CLOSED - not reproducible.** Exhaustive stress (create/delete/reboot cycles, pointer-page exhaustion to 13/16 slots with several stale table generations left in flash, storage-full state) never reproduced it; WCH-Link pointer-page + table dumps matched the display byte-for-byte. Pointer-page last-valid-wins invalidation verified sound. |
| 2 | Meas Number fields stored out-of-range values verbatim (DAS) | **FIXED + VERIFIED.** Write triggers clamp SamplingRate to [1,1000] Hz and FilterCoeff to [0,1] at write time; stored value now equals applied (2.5->1.0, -0.5->0, 1e6->1000, -1->1). |
| 3 | Fan duty >100 % accepted | **NOT A BUG** - duty is specified in percent (2.0 = 2 %). >100 clamps to 100 and the clamped value is stored; the field is committed only after `ledc_update_duty` succeeds. |
| 4 | Deleted dynamic/keyed blocks readable until the next save | **FIXED + VERIFIED.** Reads/writes on `Deleted`-flagged blocks now fail; topology summaries count only visible blocks. |
| 5 | Silent timeouts for dead addresses / missing services | **FIXED + VERIFIED.** CLI commands wait 500 ms and print "no response from device N (timeout)"; live devices unaffected. |
| 6 | Garbage number strings silently parsed as 0 (CLI) | **FIXED + VERIFIED.** Strict `strtod` with full-consumption check; rejects hex (`0x10`) and garbage (`abc`) with "Failed to parse value"; `2.5`, `1e3`, `.5`, `-0.25` accepted. |
| 7 | Size-0 files did not occupy their reserved block (NEW, found while stress-testing #1) | **FIXED + VERIFIED.** `BlockUsed` now clamps the block count to >= 1, so a size-0 file's single block is reserved. Reproduced the corruption first (ZERO@512 + BETA@512 aliasing; resizing ZERO up shadowed BETA), then proved the fix removes it. |

## Size-0 fix regression (DAS)

| Case | Result |
| --- | --- |
| `create X 0` then `create Y 64` | PASS - Y gets a distinct block (was aliasing X) |
| `create G 200`, `resize G 0`, then `create D 64` | PASS - G keeps its block; D goes elsewhere |
| `resize Z 0 -> 200` with a live file in the path | PASS - fails cleanly (was silently shadowing) |
| `resize Z 0 -> 192` with free space | PASS - succeeds |
| Table moves, delete, reboot persistence | PASS - unaffected |

## Verified correct (regression evidence)

### Storage edge cases (DAS, `file 2 ...`)

| Test | Result |
| --- | --- |
| Create size 0 / duplicate name / 8-char-truncated name | PASS (truncated duplicates rejected) |
| Table growth (>75 % full) and move (self-describing entry0 + pointer page) | PASS (flash-dump verified) |
| Resize up in place / out of space / to 0 / zero-size up | PASS (failed resize leaves file unchanged) |
| Delete existing / non-existent | PASS |
| Read non-existent / at EOF / far beyond (999 B) | PASS (clamped, no overrun) |
| Out-of-space create (fragmented) | PASS (no partial record) |
| Reboot persistence across stress states | PASS |

### Memory / Number edge cases (DAS + Tamu)

| Test | Result |
| --- | --- |
| Type mismatch (Number/Integer/Colour into wrong field) | PASS (rejected, even in-range) |
| Write to RO field / enum out of range / invalid index / wrong key | PASS (rejected) |
| Large Number saturates at Q16.16 max | PASS |
| Dynamic/Keyed create/write/save/rmem/recall; purge on save | PASS |
| Fan duty clamp (>100 -> 100, -5 -> 0) | PASS |

### Device / infrastructure

| Test | Result |
| --- | --- |
| Name truncation to 23 chars | PASS |
| Uptime sanity after reboot (no 4.29 G corruption) | PASS |
| First time-sync after reboot: large offset, converges over rounds | PASS |
| `tree 1` / `tree 2` | PASS (2 blocks on DAS) |
| SNDB registry intact (ID 1 core, ID 2 DAS) | PASS |
| DAS loop avg 0.06 ms; measurement ~1023 raw open input | PASS |
| Tamu/DAS full system backups | PASS |

## Test artifacts left on the devices

- SNDB: core as ID 1, DAS as ID 2 (expected).
- Tamu: no dynamic/keyed blocks; fan duty 0; `SYSMEM`/`DYNMEM`/`KEYMEM` backup files (expected).
- DAS storage: `.TABLE` + `SYSMEM` only (defaults restored: FilterCoeff 0.5, SamplingRate 10 on
  both Meas blocks, saved to SYSMEM).
- Device names in RAM: "Tamu Node" (core), "DAS v0.1" (DAS) - RAM only.