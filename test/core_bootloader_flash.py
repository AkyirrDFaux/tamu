#!/usr/bin/env python3
"""Flash the core (ESP32-C3) main app over its own USB bootloader.

The core bootloader speaks the raw v2 bootloader frames (Docs/Services/Bootloader.md)
directly on the USB Serial/JTAG port - no packet format, no app-interface framing. This
harness identifies the bootloader, writes the image into ota_0, drains the write
acknowledgements, then reads the image back and reports mismatches.

Run with the bootloader in update mode: hold GPIO2 at reset, or use the
TAMU_BOOTLOADER_FORCE test build.

Usage:
    python3 test/core_bootloader_flash.py [port] [image]

    port   default /dev/ttyACM1
    image  default firmware/.pio/build/Tamu_v2_0A/firmware.bin
"""
import os
import sys
import time

import serial

START, END = 0xCA, 0xBC
PAYLOAD = 32
SECTOR = 4096
WRITE_REQUEST = 39
READ_REQUEST = 7
WRITE_RESPONSE = 7
READ_RESPONSE = 39
IDENTIFY_REQUEST = 7
IDENTIFY_RESPONSE = 39

# v2 command values: a 3-bit field in the control byte (bits 2..0), even parity in bit 3.
CMD_IDENTIFY_REQ = 0b000
CMD_IDENTIFY_RESP = 0b001
CMD_WRITE_REQ = 0b010
CMD_WRITE_RESP = 0b011
CMD_READ_REQ = 0b100
CMD_READ_RESP = 0b101


def _parity(frame):
    """Even parity over the command (3 bits), offset and payload."""
    p = frame[1] & 0x07
    p ^= p >> 1
    p ^= p >> 2
    for b in frame[2:-1]:
        b ^= b >> 4
        b ^= b >> 2
        b ^= b >> 1
        p ^= b & 1
    return p & 1


def build(command, offset, payload=b"", total=WRITE_REQUEST):
    """A v2 frame: command in bits 2..0 of the control byte, even parity in bit 3."""
    f = bytearray(total)
    f[0] = START
    f[1] = command & 0x07
    f[2:6] = offset.to_bytes(4, "little")
    if payload:
        f[6:6 + len(payload)] = payload
    f[-1] = END
    f[1] |= _parity(f) << 3
    return bytes(f)


def read_frame(port, want_cmd, size, timeout=0.5):
    """Reads one frame of `size` bytes with command `want_cmd`; returns its bytes or None."""
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        b = port.read(1)
        if not b:
            continue
        if not buf:
            if b[0] == START:
                buf.append(b[0])
            continue
        buf.append(b[0])
        if len(buf) == 2 and (buf[1] & 0x07) != want_cmd:  # not the frame we want
            buf.clear()
            continue
        if len(buf) == size:
            if buf[-1] == END and (buf[1] & 0x07) == want_cmd:
                return bytes(buf)
            buf.clear()
    return None


def drain_write_responses(port, seconds):
    """Counts the write acknowledgements still arriving; returns (count, last offset)."""
    deadline = time.time() + seconds
    count = 0
    last = None
    buf = bytearray()
    while time.time() < deadline:
        b = port.read(1)
        if not b:
            continue
        if not buf:
            if b[0] == START:
                buf.append(b[0])
            else:
                continue
        buf.append(b[0])
        if len(buf) == 2 and (buf[1] & 0x07) != CMD_WRITE_RESP:
            buf.clear()
            continue
        if len(buf) == WRITE_RESPONSE:
            if buf[-1] == END and (buf[1] & 0x07) == CMD_WRITE_RESP:
                count += 1
                last = int.from_bytes(buf[2:6], "little")
            buf.clear()
    return count, last


def main():
    port_path = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
    image_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "..",
        "firmware/.pio/build/Tamu_v2_0A/firmware.bin")
    image = open(os.path.abspath(image_path), "rb").read()
    print("image %s: %d bytes" % (image_path, len(image)))

    port = serial.Serial(port_path, 115200, timeout=0.05)
    try:
        # Identify: proves the v2 wire and that the bootloader is in update mode.
        port.reset_input_buffer()
        port.write(build(CMD_IDENTIFY_REQ, 0, total=IDENTIFY_REQUEST))
        port.flush()
        ident = read_frame(port, CMD_IDENTIFY_RESP, IDENTIFY_RESPONSE, timeout=1.0)
        if ident is None:
            print("no identify response - is the core in bootloader update mode "
                  "(GPIO2 held at reset)?")
            return 2
        app_size = int.from_bytes(ident[2:6], "little")
        name = ident[6:38].split(b"\x00", 1)[0].decode("ascii", "replace")
        print("bootloader: app space %d bytes, name %r" % (app_size, name))
        if len(image) > app_size:
            print("image larger than the app space; refusing")
            return 3

        # Write phase: 32-byte chunks, paced so the 4 KB sector erase (on the first chunk
        # of each sector) finishes before the next frame arrives.
        chunks = (len(image) + PAYLOAD - 1) // PAYLOAD
        for i in range(chunks):
            off = i * PAYLOAD
            if off % SECTOR == 0:
                time.sleep(0.05)  # let the previous sector erase complete
            data = image[off:off + PAYLOAD].ljust(PAYLOAD, b"\xff")
            port.write(build(CMD_WRITE_REQ, off, data))
            port.flush()
            time.sleep(0.002)
        time.sleep(0.3)
        print("wrote %d chunks" % chunks)

        # Drain the write acknowledgements (v2 answers every write request with the offset
        # of the last chunk written successfully).
        acks, last_off = drain_write_responses(port, min(10.0, 2.0 + chunks * 0.0005))
        print("write acks drained: %d, last offset %s" % (acks, last_off))

        # Verify phase: read every chunk back and compare.
        mism = 0
        for i in range(chunks):
            off = i * PAYLOAD
            port.reset_input_buffer()
            port.write(build(CMD_READ_REQ, off, total=READ_REQUEST))
            port.flush()
            got = read_frame(port, CMD_READ_RESP, READ_RESPONSE)
            want = image[off:off + PAYLOAD].ljust(PAYLOAD, b"\xff")
            if got is None or got[6:38] != want:
                mism += 1
        print("verify: %d/%d mismatches" % (mism, chunks))
        return 0 if mism == 0 else 1
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
