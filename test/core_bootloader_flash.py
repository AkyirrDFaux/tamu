#!/usr/bin/env python3
"""Flash the core (ESP32-C3) main app over its own USB bootloader.

The core factory bootloader speaks the raw bootloader frames (Docs/Services/Bootloader.md)
directly on the USB Serial/JTAG port - no packet format, no app-interface framing. This
harness writes the image into `ota_0`, then reads it back and reports mismatches.

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
WRITE_SIZE = 39
READ_REQ_SIZE = 7


def _parity(frame):
    p = 0
    cmd = frame[1] & 0x03
    p ^= (cmd & 1) ^ ((cmd >> 1) & 1)
    for b in frame[2:-1]:
        b ^= b >> 4
        b ^= b >> 2
        b ^= b >> 1
        p ^= b & 1
    return p & 1


def write_frame(offset, payload):
    f = bytearray(WRITE_SIZE)
    f[0] = START
    f[1] = 0x01  # command: write
    f[2:6] = offset.to_bytes(4, "little")
    f[6:38] = payload
    f[38] = END
    f[1] |= _parity(f) << 2
    return bytes(f)


def read_request(offset):
    f = bytearray(READ_REQ_SIZE)
    f[0] = START
    f[1] = 0x02  # command: read request
    f[2:6] = offset.to_bytes(4, "little")
    f[6] = END
    f[1] |= _parity(f) << 2
    return bytes(f)


def read_response(port, timeout=0.5):
    """Reads one 39-byte read-response frame; returns the 32 payload bytes or None."""
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
        if len(buf) == 2:
            if (buf[1] & 0x03) != 0x03:  # not a read-response
                buf.clear()
                continue
        if len(buf) == WRITE_SIZE:
            if buf[-1] == END and (buf[1] & 0x03) == 0x03:
                return bytes(buf[6:38])
            buf.clear()
    return None


def main():
    port_path = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
    image_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "..",
        "firmware/.pio/build/Tamu_v2_0A/firmware.bin")
    image = open(os.path.abspath(image_path), "rb").read()
    print("image %s: %d bytes" % (image_path, len(image)))

    port = serial.Serial(port_path, 115200, timeout=0.05)
    try:
        # Write phase: 32-byte chunks, paced so the bootloader's 4 KB sector erase (on the
        # first chunk of each sector) finishes before the next frame arrives.
        chunks = (len(image) + PAYLOAD - 1) // PAYLOAD
        for i in range(chunks):
            off = i * PAYLOAD
            if off % SECTOR == 0:
                time.sleep(0.05)  # let the previous sector erase complete
            data = image[off:off + PAYLOAD].ljust(PAYLOAD, b"\xff")
            port.write(write_frame(off, data))
            port.flush()
            time.sleep(0.002)
        time.sleep(0.3)
        print("wrote %d chunks" % chunks)

        # Verify phase: read every chunk back and compare.
        mism = 0
        for i in range(chunks):
            off = i * PAYLOAD
            port.reset_input_buffer()
            port.write(read_request(off))
            port.flush()
            got = read_response(port)
            want = image[off:off + PAYLOAD].ljust(PAYLOAD, b"\xff")
            if got is None or got != want:
                mism += 1
        print("verify: %d/%d mismatches" % (mism, chunks))
        return 0 if mism == 0 else 1
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
