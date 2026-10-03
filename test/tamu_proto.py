#!/usr/bin/env python3
"""Direct protocol client for a Tamu node over USB Serial/JTAG (the App Interface).

The firmware's contract is the *wire protocol*, not the debug console: a packet is a 12-byte
PacketFrame header (crc8, flags, priority, payload-length-in-bytes, src, tgt, cmd, trid) plus a
payload of exactly that many bytes, sent crc8-first. Host and device wrap a stream of packets in
`0xFA | crc8 | length | payload | 0xBF` frames with the payload chunked to 60 bytes.

This is the project's test entry point for the rig: the protocol is the contract the app and
the bus depend on, so tests bind to it rather than to any printed output.

Two device-side behaviours to respect while developing against it:
  * The App Interface owns the USB port (the console/CLI was removed), so a half-fed link no
    longer has a mode to get stuck in - but a malformed packet still costs a resync. Compare
    against `app/lib/core/protocol.dart`, which builds the same frames.
  * The TX pump treats the port as dead if no app frame has arrived for ~500 ms, so a reply is
    only reliable while frames keep flowing: send and read within that window rather than
    sleeping between the two.

Usage:
    from tamu_proto import Tamu
    with Tamu() as t:
        print(t.ping(1))
        print(t.read_field(1, 0, 0, 6))   # System Name (block 0, field 6)
"""

import asyncio
import collections
import os
import select
import struct
import sys
import threading
import time

import serial

FRAME_START = 0xFA
FRAME_STOP = 0xBF
FRAME_MAX = 60
APP_ADDR = 0xFFFE  # "the app", not a bus node: replies to it come back over USB

FLAG_REQACK = 1 << 0
FLAG_START = 1 << 1
FLAG_STOP = 1 << 2
FLAG_TYPE = 1 << 3
FLAG_FRAG = 1 << 4
FLAG_SUCCESS = 1 << 5
FLAG_FAIL = 1 << 6

# Reserved TRID ranges (Docs/RSBus and Packets.md "Transaction IDs").
TRID_SYS_BASE = 0x0000
TRID_SYS_MAX = 0x0FFF
TRID_SUB_BASE = 0x1000
TRID_SUB_MAX = 0x1FFF
TRID_SCRIPT_BASE = 0x2000
TRID_SCRIPT_MAX = 0x2FFF
TRID_APP_BASE = 0xF000
TRID_APP_MAX = 0xFFFF


class Srv:
    Device = 0x00
    Register = 0x01
    LogHandler = 0x02
    Storage = 0x03
    Subscriptions = 0x04
    Script = 0x05
    App = 0x11


class DevCid:
    Discover = 0x00
    Ping = 0x01
    Identify = 0x02
    TimeSync = 0x03


class RegCid:
    EnumerateBlocks = 0x00
    EnumerateFields = 0x01
    Read = 0x02
    Write = 0x03
    Recall = 0x04
    Save = 0x05
    CreateDynamic = 0x10
    DeleteDynamic = 0x11
    GetName = 0x12
    SetName = 0x13


class StoreCid:
    Format = 0x00
    Create = 0x01
    Delete = 0x02
    Resize = 0x03
    Rename = 0x04
    Read = 0x05
    Write = 0x06


class BlockType:
    System = 0x00
    LEDButton = 0x03
    PWM = 0x04
    AccGyr = 0x05
    Vysi1Display = 0x06
    ResistiveMeasure = 0x08
    Button = 0x09
    LED = 0x0A


# The dynamic memory is four banked types (0x3F0-0x3F3) and scripts four (0x3F4-0x3F7),
# each 64 instances, addressed by one global 0..255 index. The wire type is the bank type,
# derived from the index - there is no single "dynamic"/"script" wire type.
def dynamic_type(global_index):
    return 0x3F0 + (global_index >> 6)


def script_type(global_index):
    return 0x3F4 + (global_index >> 6)


def crc8(data):
    """CRC-8, poly 0x07, init 0x00 (the wire checksum)."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def block_info(block_type, inst, field, key):
    """BlockInfo: Type10 | Instance6 | Field8 | Key8 (Register 0x01 addressing)."""
    return ((block_type & 0x3FF) << 22) | ((inst & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF)


class LinkParser:
    """Recovers the packet stream from USB link frames: 0xFA | crc8 | len | payload | 0xBF.

    The device frames the packet stream on the way out, so the raw bytes cannot be fed to the
    packet parser directly - that was the first version's mistake. `crc8` here covers length +
    payload, matching the firmware's UsbFramer.
    """

    def __init__(self):
        self.buf = bytearray()
        self.stream = bytearray()

    def feed(self, chunk):
        self.buf += chunk
        i = 0
        while i < len(self.buf):
            if self.buf[i] != FRAME_START:
                i += 1
                continue
            if len(self.buf) - i < 3:
                break
            n = self.buf[i + 2]
            if n > FRAME_MAX:
                i += 1
                continue
            if len(self.buf) - i < 3 + n + 1:
                break
            body = bytes(self.buf[i + 2:i + 3 + n])          # length byte + payload
            if crc8(body) == self.buf[i + 1] and self.buf[i + 3 + n] == FRAME_STOP:
                self.stream += body[1:]
                i += 3 + n + 1
            else:
                i += 1
        self.buf = self.buf[i:]
        out = bytes(self.stream)
        self.stream.clear()
        return out


class Packet:
    def __init__(self, flags, src, tgt_srv, cid, trid, payload=b""):
        self.flags = flags
        self.src = src
        self.tgt_srv = tgt_srv
        self.cid = cid
        self.trid = trid
        self.payload = payload

    @property
    def cmd(self):
        return (self.tgt_srv << 8) | self.cid

    def wire(self, addr):
        """12-byte header (crc8, flags, priority, len-bytes, src, tgt, cmd, trid) + payload."""
        body = self.payload
        # SRC = the app placeholder (0xFFFE): replies addressed to it are routed straight back
        # over USB (the dispatcher forwards frames targeting 0xFFFE to the App interface).
        pkt = bytearray(bytes([0, self.flags, 0, len(body)]) +
                        struct.pack("<HHHH", APP_ADDR, addr, self.cmd, self.trid) + body)
        pkt[0] = crc8(bytes(pkt[1:12 + len(body)]))  # flags..trid (+payload), as the firmware does
        return bytes(pkt)


def parse_packet(buf):
    """Returns (Packet, consumed) or (None, 0) when the buffer does not hold a full packet."""
    if len(buf) < 12:
        return None, 0
    size = 12 + buf[3]  # Payload Length is in bytes
    if buf[0] != crc8(bytes(buf[1:size])):
        return None, 1  # resync: drop a byte
    if len(buf) < size:
        return None, 0
    src, tgt, cmd, srv_src = struct.unpack("<HHHH", bytes(buf[4:12]))
    p = Packet(buf[1], src, cmd >> 8, cmd & 0xFF, srv_src, bytes(buf[12:size]))
    p.tgt = tgt
    p.srv_tgt = cmd          # the service/CID this packet addresses (a reply echoes our tag)
    return p, size


class UsbLink:
    """The USB Serial/JTAG link: `0xFA | crc8 | len | packet stream | 0xBF` frames."""

    def __init__(self, port=None):
        self.port = port or self.find_port()
        self.ser = serial.Serial(self.port, 115200, timeout=0.05)
        self.parser = LinkParser()
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    @staticmethod
    def find_port(vendor=b"303a"):
        for name in sorted(os.listdir("/dev")):
            if not name.startswith("ttyACM"):
                continue
            dev = f"/dev/{name}"
            try:
                with open(f"/sys/class/tty/{name}/device/../idVendor", "rb") as f:
                    if f.read().strip() == vendor:
                        return dev
            except OSError:
                pass
        raise RuntimeError("no Tamu (Espressif USB) console found")

    def write_stream(self, stream):
        for off in range(0, len(stream), FRAME_MAX):
            chunk = stream[off:off + FRAME_MAX]
            body = bytes([len(chunk)]) + chunk
            self.ser.write(bytes([FRAME_START, crc8(body)]) + body + bytes([FRAME_STOP]))

    def read(self, timeout=0.05):
        r, _, _ = select.select([self.ser], [], [], timeout)
        if not r:
            return b""
        return self.parser.feed(self.ser.read(self.ser.in_waiting or 1))

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


class BleLink:
    """The BLE (Nordic UART) link: each transfer is a uint16 LE length prefix followed by that
    many packet-stream bytes - a different framing from USB, which is the whole reason the
    harness has transports.

    bleak is asyncio-based, so a dedicated thread owns the event loop and this class marshals
    writes and received transfers across it.
    """

    NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    NUS_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  # host writes here
    NUS_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  # device notifies here
    BLE_CHUNK = 480  # the device's per-transfer stream buffer (AppBLE.h BLE_CHUNK)

    def __init__(self, address=None, scan_timeout=15.0):
        self.address = address
        self.rx = bytearray()        # unwrapped packet-stream bytes
        self._buf = bytearray()      # partial length-prefixed transfer
        self._error = None
        self._ready = threading.Event()
        self._loop = asyncio.new_event_loop()
        self._client = None
        self._write_size = 20
        threading.Thread(target=self._main, args=(scan_timeout,), daemon=True).start()
        if not self._ready.wait(scan_timeout + 10):
            raise TimeoutError("BLE link did not come up")
        if self._error:
            raise self._error

    # --- the asyncio side (one thread) ---
    def _main(self, scan_timeout):
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._connect(scan_timeout))
        except Exception as exc:  # surfaced to the caller
            self._error = exc
            self._ready.set()
            return
        self._ready.set()
        self._loop.run_forever()

    async def _connect(self, scan_timeout):
        from bleak import BleakClient, BleakScanner

        target = self.address
        if not target:
            # The scanner object rather than `discover()`: on this BlueZ the one-shot helper
            # can raise "No discovery started" while tearing the scan down, which loses the
            # results. Stopping explicitly (and ignoring that teardown error) keeps them.
            scanner = BleakScanner(service_uuids=[self.NUS_SERVICE])
            await scanner.start()
            deadline = time.time() + scan_timeout
            while time.time() < deadline and not scanner.discovered_devices:
                await asyncio.sleep(0.25)
            found = list(scanner.discovered_devices)
            try:
                await scanner.stop()
            except Exception:
                pass  # BlueZ complains if the discovery already ended on its own
            print(f"  BLE scan: {[(d.address, d.name) for d in found]}", flush=True)
            if not found:
                raise RuntimeError("no device advertising the Nordic UART service")
            target = found[0].address
        self._client = BleakClient(target)
        await self._client.connect()
        print(f"  BLE connected to {target}", flush=True)
        # A big MTU (the device chunks up to 480 stream bytes per notification); writes are
        # split to what this link accepts, and the device accumulates the prefix stream itself.
        mtu = getattr(self._client, "mtu_size", 23) or 23
        self._write_size = max(20, mtu - 3)
        await self._client.start_notify(self.NUS_TX, self._on_notify)

    def _on_notify(self, _char, data: bytearray):
        self._buf += data
        while len(self._buf) >= 2:
            n = self._buf[0] | (self._buf[1] << 8)
            if len(self._buf) < 2 + n:
                break
            self.rx += bytes(self._buf[2:2 + n])
            del self._buf[:2 + n]

    # --- the synchronous side ---
    def write_stream(self, stream):
        # Every transfer carries a uint16 LE length prefix followed by its packet-stream bytes
        # (the device reassembles them - BlueZ may split a write). Long streams become several
        # chunks; `_write` then splits those to whatever this link accepts.
        framed = bytearray()
        for off in range(0, len(stream), self.BLE_CHUNK):
            chunk = stream[off:off + self.BLE_CHUNK]
            framed += struct.pack("<H", len(chunk)) + chunk
        asyncio.run_coroutine_threadsafe(self._write(bytes(framed)), self._loop).result(5)

    def write_raw(self, data):
        """Writes bytes with no framing added - used to leave the device mid-chunk on purpose."""
        asyncio.run_coroutine_threadsafe(self._write(bytes(data)), self._loop).result(5)

    async def _write(self, data):
        for off in range(0, len(data), self._write_size):
            await self._client.write_gatt_char(
                self.NUS_RX, data[off:off + self._write_size], response=True)

    def read(self, timeout=0.05):
        deadline = time.time() + timeout
        while time.time() < deadline and not self.rx:
            time.sleep(0.002)
        out = bytes(self.rx)
        self.rx.clear()
        return out

    def close(self):
        try:
            if self._client:
                asyncio.run_coroutine_threadsafe(self._client.disconnect(),
                                                 self._loop).result(5)
        except Exception:
            pass
        self._loop.call_soon_threadsafe(self._loop.stop)


class Tamu:
    """One link to a node (the core is the one on USB or BLE; other nodes are reached through
    it). The protocol helpers below are transport-agnostic - only the link framing differs."""

    def __init__(self, port=None, timeout=2.0, link=None):
        self.link = link or UsbLink(port)
        self.port = getattr(self.link, "port", None) or getattr(self.link, "address", "ble")
        self.timeout = timeout
        self.buf = bytearray()
        self.txid = 0

    def close(self):
        self.link.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def send(self, packet, addr=1):
        """Sends a request to `addr` and returns the first response addressed back to us."""
        # The App owns the 0xF000-0xFFFF TRID range (Docs "Transaction IDs").
        self.txid = TRID_APP_BASE + ((self.txid + 1) & 0x0FFF)
        packet.trid = self.txid
        tag = packet.trid
        self.link.write_stream(packet.wire(addr))
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            data = self.link.read(0.05)
            if data:
                self.buf += data
            while True:
                pkt, used = parse_packet(self.buf)
                if used:
                    self.buf = self.buf[used:]
                if not pkt:
                    if not used:
                        break
                    continue
                # Responses echo the request's TRID, so match on that (not on the target
                # address, which for us is always the app placeholder).
                if (pkt.flags & FLAG_TYPE) and pkt.trid == tag:
                    return pkt
        raise TimeoutError(f"no reply (tag 0x{tag:04X})")

    # ---- convenience helpers -------------------------------------------------
    def ping(self, addr=1):
        p = self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Device,
                             DevCid.Ping, 0), addr)
        return len(p.payload) >= 0

    def read_field(self, addr, block_type, inst, field, key=0):
        """Register Read -> (meta_bytes, value_bytes)."""
        bi = block_info(block_type, inst, field, key)
        p = self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                             RegCid.Read, 0, struct.pack("<I", bi)), addr)
        if len(p.payload) < 8:
            return None, p.payload
        return p.payload[4:8], p.payload[8:]

    def write_field(self, addr, block_type, inst, field, key, type_tag, value, flags=0):
        bi = block_info(block_type, inst, field, key)
        # ValueInfo: Type(16) | Size(8) | Flags(8). The key is not on the wire (it is in the
        # BlockInfo), so the 3rd byte is the value length and the 4th the passive flags.
        meta = bytes([type_tag & 0xFF, (type_tag >> 8) & 0xFF,
                      len(value) & 0xFF, flags & 0xFF])
        payload = struct.pack("<I", bi) + meta + value
        return self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                                RegCid.Write, 0, payload), addr)

    def enumerate_types(self, addr=1):
        p = self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                             RegCid.EnumerateBlocks, 0, b""), addr)
        return list(p.payload[4:]) if len(p.payload) > 4 else []

    def instance_count(self, block_type, addr=1):
        """The number of instances of a static type (from the CID 0 block-type list)."""
        raw = self.enumerate_types(addr)
        for i in range(0, len(raw) - 1, 2):
            w = raw[i] | (raw[i + 1] << 8)
            t, max_i = (0x300 | (w >> 8), w & 0xFF) if (w >> 8) >= 0xF0 \
                else ((w >> 6) & 0x3FF, w & 0x3F)
            if t == block_type:
                return max_i + 1
        return 0

    def save(self, addr, block_type, inst=0):
        bi = block_info(block_type, inst, 0xFF, 0xFF)
        return self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                                RegCid.Save, 0, struct.pack("<I", bi)), addr)

    def recall(self, addr, block_type, inst=0):
        bi = block_info(block_type, inst, 0xFF, 0xFF)
        return self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                                RegCid.Recall, 0, struct.pack("<I", bi)), addr)

    def file_table(self, addr, timeout=4.0):
        """Storage read of '.TABLE' -> the raw FileEntry records (offset u32, size u32, name[8])."""
        name = b".TABLE  "
        p = self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Storage,
                             StoreCid.Read, 0, name), addr)
        data = p.payload
        if p.flags & FLAG_FRAG and len(data) > 4:
            data = data[4:]
            if data[:2] == b"\x00\x00" and len(data) >= 10:
                data = data[10:]
        return data

    @staticmethod
    def num(raw):
        """Q16.16 fixed point -> float."""
        return struct.unpack("<i", raw[:4])[0] / 65536.0


def ble_aborted_session_regression(address=None):
    """Regression for the fixed BLE receive bug: a session that ends mid-chunk must not poison
    the next one.

    The device accumulates each length-prefixed chunk across transfers. A session that died
    partway through one used to leave `haveLen`/`got` set, so the *next* session read its length
    prefix as payload - every frame was shifted, `PacketWireSize` saw garbage, nothing dispatched
    and nothing was CRC-rejected. Here we deliberately leave it mid-chunk (a prefix claiming 12
    stream bytes with only 5 delivered), drop the link, reconnect and ping.
    """
    aborted = BleLink(address=address)
    try:
        aborted.write_raw(struct.pack("<H", 12) + b"\x01\x02\x03\x04\x05")
        time.sleep(0.2)
    finally:
        aborted.close()
    time.sleep(1.0)  # let the device notice the loss and re-advertise

    link = BleLink(address=address)
    with Tamu(link=link) as t:
        ok = t.ping(1)
    print("aborted-session recovery ping:", ok)
    if not ok:
        raise SystemExit("FAIL: a session aborted mid-chunk poisoned the next one")
    print("PASS: the receive path recovered after a mid-chunk disconnect")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    use_ble = "--ble" in sys.argv
    node = int(args[0]) if args and args[0].isdigit() else 1
    # `--ble` scans for the Nordic UART service; `--ble <mac>` connects straight to a known
    # address, which sidesteps BlueZ's discovery races (and is faster on a known rig).
    # Look for the MAC anywhere (not just after a node number), so `--ble <mac>` and
    # `--ble <node> <mac>` both connect by address instead of scanning.
    ble_addr = next((a for a in args if ":" in a), None)
    addr = node
    if "--ble-abort-check" in sys.argv:
        ble_aborted_session_regression(ble_addr)
        raise SystemExit(0)
    link = BleLink(address=ble_addr) if use_ble else None
    with Tamu(link=link) as t:
        print(f"{'BLE' if use_ble else 'USB'} link: {t.port}")
        print("ping:", t.ping(addr))
        types = t.enumerate_types(addr)
        print("types:", [hex(x) for x in types])
        meta, val = t.read_field(addr, BlockType.System, 0, 6)
        print("System Name:", val.split(b"\x00")[0].decode(errors="replace"))
