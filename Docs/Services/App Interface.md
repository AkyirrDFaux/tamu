Per device specific, USB, BLE or both.
Use define USE_APP_INTERFACE.

Provides forwarding of packets to the app via the avaliable interface.
Maximize throughput (split into fragments, fully fill payload), since line is bi-directional peer to peer.

App has reserved TRID range.
#### USB Packet (64 bytes):

| Start trigger | CRC8 | Length | Payload (Packets, serialized stream) | Stop |
| ------------- | ---- | ------ | ------------------------------------ | ---- |
| 0xFA          | 8bit | 8bit   | 60 bytes max                         | 0xBF |
CRC is over Length + Payload.
#### BLE Packet:
Preffered wireless method.

| Length of this BLE packet | Payload (Packets, serialized stream) |
| ------------------------- | ------------------------------------ |
| uint16                    | (ATT_MTU - 2) bytes max              |
#### UDP Packet:
For Wi-Fi only devices. Connects as a device to an existing network. The app has to scan for it.

| Valid payload size | Payload                                       |
| ------------------ | --------------------------------------------- |
| uint16             | limited maximum size to prevent fragmentation |
