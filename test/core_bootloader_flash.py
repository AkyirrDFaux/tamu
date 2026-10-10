#!/usr/bin/env python3
"""Flash the core (ESP32-C3) main app over its own USB bootloader.

The core bootloader speaks the raw v2 bootloader frames (Docs/Services/Bootloader.md)
directly on the USB Serial/JTAG port - no packet format, no app-interface framing. This
harness identifies the bootloader, writes the image into ota_0 with an adaptive pipeline
driven by the v2 write acknowledgements, then reads the image back and reports mismatches.

Run with the bootloader in update mode: hold GPIO2 at reset (the red LED stays lit), or use
the TAMU_BOOTLOADER_FORCE test build.

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

# Adaptive write pipeline: the window is how many chunks may be in flight. It doubles while
# the bootloader keeps acknowledging and halves whenever it falls behind, so the run is as
# fast as the link and the flash allow without overrunning either buffer. The bootloader
# blocks on a 4 KB sector erase, which the acks expose as a slower window - no fixed pacing.
WINDOW_START = 4
WINDOW_MIN = 1
WINDOW_MAX = 256


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


def write_image(port, image):
    """Writes the whole image, paced entirely by the write acknowledgements.

    The bootloader answers every write request with the offset of the last chunk written
    successfully, so the harness keeps a window of chunks in flight, grows it while the acks
    keep up, halves it when they fall behind and resends from the last acknowledged chunk
    when one is rejected. Returns (chunks, resends, peak window, seconds).
    """
    chunks = (len(image) + PAYLOAD - 1) // PAYLOAD
    window = WINDOW_START
    peak = window
    sent = 0
    acked = 0
    resends = 0
    last_ack_off = None
    started = time.time()

    def payload_of(i):
        off = i * PAYLOAD
        return image[off:off + PAYLOAD].ljust(PAYLOAD, b"\xff")

    while acked < chunks:
        # Fill the window.
        while sent < chunks and (sent - acked) < window:
            port.write(build(CMD_WRITE_REQ, sent * PAYLOAD, payload_of(sent)))
            sent += 1
        port.flush()

        frame = read_frame(port, CMD_WRITE_RESP, WRITE_RESPONSE, timeout=2.0)
        if frame is None:
            # The bootloader is behind (a 4 KB sector erase) or a chunk is lost: back off.
            window = max(WINDOW_MIN, window // 2)
            if sent > acked:
                sent = acked
                resends += 1
            continue

        ack_off = int.from_bytes(frame[2:6], "little")
        if ack_off == last_ack_off and sent > acked:
            # The same offset twice with chunks outstanding: the chunk after it was rejected
            # (bad parity or a short buffer). Resync to it and resend from there.
            resends += 1
            sent = acked
            window = max(WINDOW_MIN, window // 2)
            continue
        last_ack_off = ack_off
        acked = min(ack_off // PAYLOAD + 1, sent)
        # Grow while the bootloader keeps up; the link and the flash set the pace.
        if (sent - acked) <= window // 2 and window < WINDOW_MAX:
            window = min(WINDOW_MAX, window * 2)
            peak = max(peak, window)

    return chunks, resends, peak, time.time() - started


def main():
    port_path = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
    image_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "..",
        "firmware/.pio/build/Tamu_v2_0A/firmware.bin")
    image = open(os.path.abspath(image_path), "rb").read()
    print("image %s: %d bytes" % (image_path, len(image)))

    port = serial.Serial(port_path, 115200, timeout=0.02)
    try:
        # Identify: proves the v2 wire and that the bootloader is in update mode.
        port.reset_input_buffer()
        port.write(build(CMD_IDENTIFY_REQ, 0, total=IDENTIFY_REQUEST))
        port.flush()
        ident = read_frame(port, CMD_IDENTIFY_RESP, IDENTIFY_RESPONSE, timeout=1.0)
        if ident is None:
            print("no identify response - is the core in bootloader update mode "
                  "(GPIO2 held at reset, red LED lit)?")
            return 2
        app_size = int.from_bytes(ident[2:6], "little")
        name = ident[6:38].split(b"\x00", 1)[0].decode("ascii", "replace")
        print("bootloader: app space %d bytes, name %r" % (app_size, name))
        if len(image) > app_size:
            print("image larger than the app space; refusing")
            return 3

        chunks, resends, peak, secs = write_image(port, image)
        print("wrote %d chunks in %.1f s (%.1f KiB/s), resends %d, peak window %d"
              % (chunks, secs, len(image) / secs / 1024.0, resends, peak))

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
