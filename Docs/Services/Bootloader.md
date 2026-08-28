For non-USB devices.
Self-contained, never overwritten, works over the RSBus.
Completely minimal implementation, as little flash as possible, separate flash segment.
Use define USE_BOOTLOADER.

Enter: Button press at boot or from calling service switch at main node's app runtime.
If button was not pressed at boot, continue to node's app.
Leave: By service command or a manual restart.

ID is not valid at this stage, broadcast is used, high priority to prevent losses.

Inbound packets contain 256-byte chunks of the main node's app, ordered by Fragmentation,
they are sent by the (complementary) app.
Outbound packets are confirmation 


| Function          | SRV CID | Content request                                    | Content response                                       | Note                                  |
| ----------------- | ------- | -------------------------------------------------- | ------------------------------------------------------ | ------------------------------------- |
| Bootloader check  | 0       | Device SN                                          | Device SN + bool (true = bootloader mode, false = app) |                                       |
| Switch bootloader | 1       | Device SN + bool (true = enter, false = leave)     | -                                                      | Enters bootloader from main app only. |
| App write         | 1       | Fragmentation, Node's app binary contents (stream) | Last sequential fragmentation index written.           | Respond only if requested             |
| Content verify    | 2       | CRC32 of app binary sent                           | bool (true = verfied)                                  |                                       |

