Implemented per device, over USB, BLE or both.

Use define `USE_APP_INTERFACE`.

Provides forwarding of packets to the app over the available interface. Throughput is maximised by splitting into fragments and filling the payload completely, since the line is a bi-directional peer-to-peer link.

The app has a reserved TrID range within [[RSBus and Packets]].
#### USB Packet
The USB packet is 64 bytes long:

| Start Trigger | CRC8 | Length | Payload (packets, serialized stream) | Stop |
| ------------- | ---- | ------ | ------------------------------------ | ---- |
| 0xFA          | uint8 | uint8 | uint8[60]                            | 0xBF |

The CRC covers Length and Payload.
#### BLE Packet
The preferred wireless method.

| Length of this BLE packet | Payload (packets, serialized stream) |
| ------------------------- | ------------------------------------ |
| uint16                    | uint8[ATT_MTU - 2]                   |
#### UDP Packet
For Wi-Fi only devices. The device connects as a client to an existing network, which the app scans for.

| Valid payload size | Payload |
| ------------------ | ------- |
| uint16             | Limited maximum size, to prevent fragmentation |
