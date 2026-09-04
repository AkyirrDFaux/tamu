Per device specific, USB, BLE or both.
Use define USE_APP_INTERFACE.
Must not interfere with CLI!

Provides forwarding of packets to the app via the avaliable interface.
Maximize throughput (split into fragments, fully fill payload), since line is bi-directional peer to peer.

App has reserved TRID range 0xF000 - 0xFFFF.
#### USB Packet (64 bytes):

| Start trigger | CRC8 | Length | Payload (Packets, serialized stream) | Stop |
| ------------- | ---- | ------ | ------------------------------------ | ---- |
| 0xFA          | 8bit | 8bit   | 60 bytes max                         | 0xBF |
CRC is over Length + Payload.

#### BLE Packet:

| Length of this BLE packet | Payload (Packets, serialized stream) |
| ------------------------- | ------------------------------------ |
| uint16                    | (ATT_MTU - 2) bytes max              |