import serial
import time
import sys

port = 'COM5'  # adjust if needed
try:
    ser = serial.Serial(port, 115200, timeout=1)
except Exception as e:
    print(f"Cannot open {port}: {e}")
    sys.exit(1)

print(f"Listening on {port}... Ctrl-C to stop")
start = time.time()
keywords = ['AUDIOHEALTH', 'PES not', 'error', 'Guru', 'backtrace', 'NCHANS', 'INVALID_ADTS', 'LoadStore']
try:
    while time.time() - start < 60:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        # print everything with timestamp
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {line}")
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    print("Done")
