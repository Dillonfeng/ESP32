# -*- coding: utf-8 -*-
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
duration = int(sys.argv[3]) if len(sys.argv) > 3 else 25

try:
    s = serial.Serial(port, baud, timeout=0.1)
except Exception as e:
    print(f"[open fail] {e}")
    sys.exit(1)

print(f"[listening {port} @{baud} for {duration}s]")
t0 = time.time()
buf = b""
while time.time() - t0 < duration:
    b = s.read(256)
    if b:
        try:
            print(b.decode("utf-8", "replace"), end="", flush=True)
        except Exception:
            print(repr(b), end="", flush=True)
s.close()
print("\n[done]")
