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
| uint16                    | (MTU - 2) bytes max                  |
SRV CID are App defined transaction IDs, the device does not care.

The BLE payload is a length-prefixed chunk of the serialized packet stream: the 3-byte
ATT header plus the 2-byte length prefix leave MTU - 5 bytes for the stream per
notification.