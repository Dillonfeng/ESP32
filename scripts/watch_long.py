import serial
import time
import sys

port = 'COM5'
try:
    ser = serial.Serial(port, 115200, timeout=0.5)
except Exception as e:
    print(f"Cannot open {port}: {e}")
    sys.exit(1)

# Reset
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)
ser.dtr = True

print(f"Listening on {port} for 180s. Switch to 经济之声 now.")
start = time.time()
count = 0
try:
    while time.time() - start < 180:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        count += 1
        ts = time.strftime("%H:%M:%S")
        # Highlight important lines
        important = any(k in line for k in ['经济之声', 'AUDIOHEALTH', 'PES', 'error', 'Guru', 'backtrace', 'NCHANS', 'INVALID_ADTS', 'playing'])
        prefix = ">>>" if important else "   "
        print(f"{prefix}[{ts}] {line}", flush=True)
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    print(f"Done. {count} lines captured.")
