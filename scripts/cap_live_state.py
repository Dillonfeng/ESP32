# -*- coding: utf-8 -*-
"""Capture live serial state from the ESP32-S3 radio (read-only by default).
Usage: python cap_live_state.py [seconds] [send_char]
Opens COM7 WITHOUT toggling DTR/RTS (prevents ESP32 auto-reset on open),
reads for `seconds`, then optionally sends one char (e.g. '1') and keeps reading.
"""
import sys, time
import serial

PORT = "COM7"
BAUD = 115200

def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 35.0
    send = sys.argv[2] if len(sys.argv) > 2 else None

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    # Configure lines BEFORE open so SetCommState never asserts DTR/RTS
    # (DTR low pulses EN through the auto-reset transistor -> reboot).
    ser.dtr = False
    ser.rts = False
    ser.open()
    print(f"[cap] {PORT} opened (DTR/RTS kept high), reading {seconds:.0f}s ...", flush=True)
    t_end = time.time() + seconds
    buf = b""
    while time.time() < t_end:
        chunk = ser.read(1024)
        if chunk:
            buf += chunk
            try:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
            except Exception:
                pass
            sys.stdout.flush()

    if send:
        print(f"\n[cap] sending '{send}' then reading 45s ...", flush=True)
        ser.write(send.encode())
        ser.flush()
        t_end = time.time() + 45
        while time.time() < t_end:
            chunk = ser.read(1024)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    ser.close()
    print("\n[cap] done.", flush=True)

if __name__ == "__main__":
    main()
