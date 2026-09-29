#!/usr/bin/env python3
"""Direct protocol client for a Tamu node over USB Serial/JTAG (the App Interface).

The firmware's contract is the *wire protocol*, not the debug console: a packet is a 12-byte
PacketFrame header (crc8, flags, priority, payload-length-in-words, src, cmd, tgt, trid) plus a
payload padded to 4 bytes, sent crc8-first. Host and device wrap a stream of packets in
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
        print(t.read_field(1, 0xFF, 0, 0, 6))   # System Name
"""

import os
import select
import struct
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
    Enumerate = 0x00
    Read = 0x01
    Write = 0x02
    Save = 0x03
    Recall = 0x04
    CreateDynamic = 0x10
    DeleteDynamic = 0x11
    GetName = 0x12
    SetName = 0x13
    GetMemUsage = 0x14
    ReadBackup = 0x15


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
    Script = 0x3FE
    Dynamic = 0x3FF


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
        """12-byte header (crc8, flags, priority, len-words, src, cmd, tgt, trid) + padded payload."""
        pad = (-len(self.payload)) % 4
        body = self.payload + b"\x00" * pad
        words = len(body) // 4
        # SRC = the app placeholder (0xFFFE): replies addressed to it are routed straight back
        # over USB (the dispatcher forwards frames targeting 0xFFFE to the App interface).
        pkt = bytearray(bytes([0, self.flags, 0, words]) +
                        struct.pack("<HHHH", APP_ADDR, self.cmd, addr, self.trid) + body)
        pkt[0] = crc8(bytes(pkt[1:12 + len(body)]))  # flags..trid (+payload), as the firmware does
        return bytes(pkt)


def parse_packet(buf):
    """Returns (Packet, consumed) or (None, 0) when the buffer does not hold a full packet."""
    if len(buf) < 12:
        return None, 0
    words = buf[3]
    size = 12 + words * 4
    if buf[0] != crc8(bytes(buf[1:12 + words * 4])):
        return None, 1  # resync: drop a byte
    if len(buf) < size:
        return None, 0
    src, cmd, tgt, srv_src = struct.unpack("<HHHH", bytes(buf[4:12]))
    p = Packet(buf[1], src, cmd >> 8, cmd & 0xFF, srv_src, bytes(buf[12:size]))
    p.tgt = tgt
    p.srv_tgt = cmd          # the service/CID this packet addresses (a reply echoes our tag)
    return p, size


class Tamu:
    """One link to a node (the core is the one on USB; other nodes are reached through it)."""

    def __init__(self, port=None, timeout=2.0):
        self.port = port or self.find_port()
        self.ser = serial.Serial(self.port, 115200, timeout=0.05)
        self.timeout = timeout
        self.buf = bytearray()
        self.link = LinkParser()
        self.txid = 0
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

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _write_frame(self, stream):
        for off in range(0, len(stream), FRAME_MAX):
            chunk = stream[off:off + FRAME_MAX]
            body = bytes([len(chunk)]) + chunk
            self.ser.write(bytes([FRAME_START, crc8(body)]) + body + bytes([FRAME_STOP]))

    def send(self, packet, addr=1):
        """Sends a request to `addr` and returns the first response addressed back to us."""
        self.txid = (self.txid + 1) & 0xFF
        packet.trid = (Srv.App << 8) | self.txid   # srv_src: the app's service + transaction
        tag = packet.trid
        self._write_frame(packet.wire(addr))
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.ser], [], [], 0.05)
            if r:
                self.buf += self.link.feed(self.ser.read(self.ser.in_waiting or 1))
            while True:
                pkt, used = parse_packet(self.buf)
                if used:
                    self.buf = self.buf[used:]
                if not pkt:
                    if not used:
                        break
                    continue
                # A reply addresses the requester's tag: the device answers with
                # srv_tgt = the request's srv_src, so match on that (not on the target
                # address, which for us is always the app placeholder).
                if (pkt.flags & FLAG_TYPE) and pkt.srv_tgt == tag:
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

    def write_field(self, addr, block_type, inst, field, key, type_tag, value):
        bi = block_info(block_type, inst, field, key)
        meta = bytes([type_tag & 0xFF, (type_tag >> 8) & 0xFF, key & 0xFF, len(value)])
        payload = struct.pack("<I", bi) + meta + value
        return self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                                RegCid.Write, 0, payload), addr)

    def enumerate_types(self, addr=1):
        p = self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                             RegCid.Enumerate, 0, bytes([0, 0, 0, 0, 0])), addr)
        return list(p.payload[4:]) if len(p.payload) > 4 else []

    def instance_count(self, block_type, addr=1):
        bi = block_info(block_type, 0x3F, 0xFF, 0)
        p = self.send(Packet(FLAG_REQACK | FLAG_START | FLAG_STOP, 1, Srv.Register,
                             RegCid.Enumerate, 0, bytes([1]) + struct.pack("<I", bi)), addr)
        return p.payload[4] if len(p.payload) >= 5 else None

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


if __name__ == "__main__":
    import sys
    addr = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    with Tamu() as t:
        print(f"port {t.port}")
        print("ping:", t.ping(addr))
        types = t.enumerate_types(addr)
        print("types:", [hex(x) for x in types])
        meta, val = t.read_field(addr, BlockType.System, 0, 6)
        print("System Name:", val.split(b"\x00")[0].decode(errors="replace"))
