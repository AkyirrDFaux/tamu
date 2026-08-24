#!/usr/bin/env python3
import serial, sys, time

PORT = "/dev/ttyACM0"
BAUD = 115200

def main():
    args = sys.argv[1:]
    action = args[0] if args else "boot"
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
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