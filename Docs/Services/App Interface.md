Per device specific, USB, BLE or both.
Use define USE_APP_INTERFACE.
Must not interfere with CLI!
Use a global status bool variable to report if the app is actually connected.

Provides forwarding of packets to the app via the avaliable interface.
Maximize throughput (split into fragments, fully fill payload), since line is bi-directional peer to peer.

#### USB Packet (64 bytes):

| Start trigger | CRC8 | Length | Payload (Packets, serialized stream) | Stop |
| ------------- | ---- | ------ | ------------------------------------ | ---- |
| 0xFA          | 8bit | 8bit   | 60 bytes max                         | 0xBF |
CRC is over Length + Payload.

#### BLE Packet:

| Length of this BLE packet | Payload (Packets, serialized stream) |
| ------------------------- | ------------------------------------ |
| uint16                    | (MTU size - 2) bytes max             |
SRV CID are App defined transaction IDs, the device does not care.