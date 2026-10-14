# faad2 verification: jjzs 180s -> 905 90s -> dszs 90s. All lines -> log file,
# key lines echoed live.
import serial, time, os

port = 'COM5'
os.makedirs('logs', exist_ok=True)
logf = open(f"logs/faad2_{time.strftime('%H%M%S')}.log", 'w', encoding='utf-8', errors='replace')
ser = serial.Serial(port, 115200, timeout=1)

PHASES = [(None, 180, 'jjzs(boot)'), ('2', 90, '905'), ('3', 90, 'dszs')]
KEY = ('AAC decode error', 'Guru', 'backtrace', 'DEBUG', 'AUDIOHEALTH',
       'syncword', 'Spectral', 'HeaderFormat', 'Channels:', 'SampleRate:', 'reboot', 'abort')

def pump(dur):
    end = time.time() + dur
    while time.time() < end:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        ts = time.strftime("%H:%M:%S")
        logf.write(f"[{ts}] {line}\n"); logf.flush()
        if any(k in line for k in KEY):
            print(f"[{ts}] {line[:150]}")

try:
    for ch, dur, name in PHASES:
        if ch:
            print(f">>> switch {name} ({ch})")
            ser.write(ch.encode())
        else:
            print(f"PHASE {name} {dur}s")
        pump(dur)
finally:
    ser.close(); logf.close()
    print("Done. Log:", logf.name)
