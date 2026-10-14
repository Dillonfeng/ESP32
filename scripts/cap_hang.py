# -*- coding: utf-8 -*-
# Listen-only serial capture (does NOT reset the device).
# User performs a manual hardware reset; full boot -> play -> hang sequence is
# streamed to disk so the log can be tailed while capturing.
# usage: python cap_hang.py [COM5] [seconds=150] [out=cap_hang.log]
import serial, time, sys, traceback

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
dur  = int(sys.argv[2]) if len(sys.argv) > 2 else 150
out  = sys.argv[3] if len(sys.argv) > 3 else r"D:\ESP32-S3\cap_hang.log"

f = open(out, "wb", buffering=0)
try:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 1
    s.dtr = False      # GPIO0 high
    s.rts = False      # do NOT pulse reset
    s.open()

    end = time.time() + dur
    f.write(("capture start on %s for %ds at %s (waiting for manual reset)\n"
             % (port, dur, time.strftime("%H:%M:%S"))).encode())
    f.flush()
    while time.time() < end:
        try:
            d = s.read(4096)
            if d:
                f.write(d)
                f.flush()
        except Exception as e:
            # device reset may re-enumerate the USB CDC port; log and retry open
            f.write(("\n[READ ERROR] %r\n%s\n" % (e, traceback.format_exc())).encode())
            f.flush()
            time.sleep(0.5)
            try:
                s.close()
            except Exception:
                pass
            try:
                s = serial.Serial(port, 115200, timeout=1)
                s.dtr = False
                s.rts = False
                f.write(b"[REOPENED]\n")
                f.flush()
            except Exception as e2:
                f.write(("[REOPEN FAILED] %r\n" % e2).encode())
                f.flush()
                time.sleep(1)
    s.close()
except Exception:
    f.write(("\n[FATAL]\n" + traceback.format_exc()).encode())
finally:
    f.close()
