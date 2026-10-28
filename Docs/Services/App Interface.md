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
| uint16                    | uint8[ATT_MTU - 5]                   |
#### UDP Packet
For Wi-Fi only devices (planned; not implemented). The device connects as a client to an existing network, which the app scans for.

| Valid payload size | Payload |
| ------------------ | ------- |
| uint16             | Limited maximum size, to prevent fragmentation |
### BLE Settings
Block type `0x0B`, core only: the link's configuration and state. **Every write to this block must arrive over USB** — a frame from the BLE interface is refused whether or not that link is bonded and authenticated, because a paired phone must not be able to change the key, wipe the bonds, rename the device or switch its radio off.

| Field | Key | Type | Flags | Note |
| --- | --- | --- | --- | --- |
| Enabled | 0 | `Bool` | P, W | false = the radio is off: no advertising, no connections |
| Passkey | 0 | `Text` (6 digits) | W | read and write; `000000` disables it and pairing falls back to Just Works. Stored in NVS outside the register file; USB only |
| Mode | 0 | `Enum` | RO | Just Works or Passkey, derived from the passkey so the two cannot disagree |
| Display name | 0 | `Text` | P, W | the name advertised over BLE; empty means the System `Name`. Applies from the next advertising start |
| Bond count | 0 | `Index` | RO | entries in the bond store |
| Bond | 0..N | `Text` | RO | address and name of a bonded peer, oldest first |
| Window | 0 | `Uint32` | RO | milliseconds left in the connectable window, 0 when closed |
| Action | 0 | `Enum` | W | 0 opens the pairing window, 1 forgets every bond. USB only |

- The device is connectable only inside a window: opened by `Action = 0`, or automatically for five minutes after boot when no bond is stored.
- Characteristics require an encrypted link; with a passkey set they also require an authenticated (MITM-protected) one.
- Bonds and the passkey live in NVS, not in the register file: they survive a firmware reflash, and `Action = 1` is the only way to clear them.
- The store holds `CONFIG_BT_NIMBLE_MAX_BONDS` bonds (three today) and the device accepts one live connection at a time.
### WiFi Settings
Block type `0x0C`, core only, **compiled in only when the WiFi option is enabled** — it is absent from a stock device, and absent means the device has no WiFi. The radio itself is not implemented yet.

| Field | Key | Type | Flags | Note |
| --- | --- | --- | --- | --- |
| Enabled | 0 | `Bool` | TR, P | associate when set |
| SSID | 1 | `Text` | P | |
| Password | 2 | `Text` | W | write-only: it always reads back empty and is stored in NVS outside the register file |
| State | 3 | `Enum` | RO | see below |
| IP address | 4 | `Undefined` | RO | 16 raw bytes, the address in network order; all-zero when not connected. An IPv4 address is reported IPv4-mapped (::ffff:a.b.c.d) |

`State`: `0` Disconnected (enabled, credentials present, not associated), `1` Connecting, `2` Connected, `3` No SSID, `4` No password, `5` Wrong password (association refused), `6` Error. With no radio implemented only `0`, `3` and `4` are reported, because an incomplete configuration is detectable without one.

`IP address` holds 16 raw bytes of the address in network order, and all-zero means no address. An IPv4-only network reports its address IPv4-mapped (`::ffff:a.b.c.d`, bytes 10 and 11 set to `0xFF`), so one field carries both families and the app decodes it one way. The firmware does not format the value: the app renders the standard grouped-hex form. IPv6 is already enabled in this build (`CONFIG_LWIP_IPV6`, `CONFIG_LWIP_IPV6_NUM_ADDRESSES = 3`), so the 16-byte width needs no configuration change.
