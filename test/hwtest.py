#!/usr/bin/env python3
import serial, sys, time

def detect_port(vendor_id):
    """Finds a ttyACM port whose USB device carries `vendor_id` (e.g. '303a'
    for Espressif / the Tamu console, '1a86' for WCH-Link / the DAS).
    USB enumeration order is not stable, so paths must never be hardcoded."""
    import glob, os
    for tty in sorted(glob.glob('/sys/class/tty/ttyACM*')):
        p = os.path.realpath(f'{tty}/device')
        for _ in range(4):
            if os.path.exists(f'{p}/idVendor'):
                try:
                    with open(f'{p}/idVendor') as f:
                        if f.read().strip().lower() == vendor_id:
                            return f'/dev/{os.path.basename(tty)}'
                except OSError:
                    pass
                break
            p = os.path.dirname(p)
    return None

TAMU_PORT = detect_port('303a') or '/dev/ttyACM0'
BAUD = 115200

def main():
    args = sys.argv[1:]
    action = args[0] if args else "boot"
    ser = serial.Serial(TAMU_PORT, BAUD, timeout=0.2)
    if action == "boot":
        end = time.time() + 5
        buf = b""
        while time.time() < end:
            data = ser.read(ser.in_waiting or 1)
            if data:
                buf += data
        sys.stdout.write(buf.decode("utf-8", "replace"))
        ser.close()
        return
    elif action == "cmd":
        dur = float(args[1])
        for cmd in args[2:]:
            ser.reset_input_buffer()
            ser.write((cmd + "\n").encode())
            end = time.time() + dur
            buf = b""
            while time.time() < end:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    buf += data
            out = buf.decode("utf-8", "replace")
            print(f"=== CMD: {cmd} ===")
            print(out)
        ser.close()
        return
    elif action == "read":
        dur = float(args[1])
        end = time.time() + dur
        buf = b""
        while time.time() < end:
            data = ser.read(ser.in_waiting or 1)
            if data:
                buf += data
        sys.stdout.write(buf.decode("utf-8", "replace"))
        ser.close()

if __name__ == "__main__":
    main()