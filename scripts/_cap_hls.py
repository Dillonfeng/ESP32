# -*- coding: utf-8 -*-
"""Open COM8, send a trigger key (default '2'=环球资讯), then timestamped-capture
ALL serial lines for N seconds to logs\cap_hls_<key>_<dur>.log (no filtering).
No device reset - just inject key and listen."""
import serial, time, sys

PORT = "COM8"
KEY = sys.argv[1] if len(sys.argv) > 1 else "2"
DUR = int(sys.argv[2]) if len(sys.argv) > 2 else 90
OUT = r"D:\ESP32-S3\logs\cap_hls_%s_%d.log" % (KEY.replace(":", "h"), DUR)

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.dtr = False
ser.rts = False
ser.timeout = 0.05
ser.open()
try:
    ser.setDTR(False)
    ser.setRTS(False)
except Exception:
    pass
ser.reset_input_buffer()
ser.write(KEY.encode())
print("[TRIG %s] capturing %ds -> %s" % (KEY, DUR, OUT), flush=True)

t0 = time.monotonic()
n = 0
buf = []
while time.monotonic() - t0 < DUR:
    try:
        l = ser.readline().decode("utf-8", "replace").rstrip()
        if l:
            ts = time.monotonic() - t0
            buf.append("t=%9.3f %s" % (ts, l))
            n += 1
            if n % 200 == 0:
                print("[%d lines]" % n, flush=True)
    except Exception:
        pass
ser.close()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(buf) + "\n")
print("[DONE n=%d -> %s]" % (n, OUT))
