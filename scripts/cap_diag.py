# DIAG capture: boot auto-plays jjzs (经济之声). Capture ~240s, then send '2'
# to switch to 环球资讯 (CRI905) and capture ~120s more. All lines -> log file,
# matching lines echoed to console live.
import serial, time, sys, os

port = 'COM5'
os.makedirs('logs', exist_ok=True)
logf = open(f"logs/diag_{time.strftime('%H%M%S')}.log", 'w', encoding='utf-8', errors='replace')
ser = serial.Serial(port, 115200, timeout=1)

PHASE1 = 240  # 经济之声
PHASE2 = 120  # 环球资讯
KEY = ('NCHANS', 'AACERR', 'error', 'Guru', 'backtrace', 'DEBUG', 'AUDIOHEALTH', 'syncword')

def pump(dur):
    end = time.time() + dur
    while time.time() < end:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        ts = time.strftime("%H:%M:%S")
        logf.write(f"[{ts}] {line}\n"); logf.flush()
        if any(k in line for k in KEY):
            print(f"[{ts}] {line}")

try:
    print("PHASE1 jjzs 240s ...")
    pump(PHASE1)
    print(">>> sending '2' (switch to CRI905)")
    ser.write(b'2')
    pump(PHASE2)
finally:
    ser.close(); logf.close()
    print("Done. Log:", logf.name)
