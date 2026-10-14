import serial, time, os, sys
port = sys.argv[2] if len(sys.argv) > 2 else 'COM7'
ser = serial.Serial(port, 115200, timeout=1)
ser.dtr = False; ser.rts = False  # avoid reset on next open
start = time.time()
os.makedirs('logs', exist_ok=True)
fn = 'logs/hlsprobe_%s.log' % time.strftime('%H%M%S')
out = open(fn, 'w', encoding='utf-8', errors='replace')
dur = int(sys.argv[1]) if len(sys.argv) > 1 else 100
cmd = sys.argv[3] if len(sys.argv) > 3 else None
cmd_sent = False
while time.time() - start < dur:
    if cmd and not cmd_sent and time.time() - start > 3:
        ser.write(cmd.encode()); ser.flush()
        print('SENT CMD:', cmd)
        cmd_sent = True
    l = ser.readline().decode('utf-8', errors='replace').strip()
    if not l:
        continue
    out.write(l + '\n'); out.flush()
    if any(k in l for k in ('SETDEFAULTS', 'TS_RESET', 'TIMEOUT_RECONNECT', 'I2SGAP', 'AAC PID', 'PES header', 'AUDIOSTALL', 'connect to', 'serial switch', 'AUDIO_INFO')):
        print(time.strftime('%H:%M:%S'), l[:140])
out.close(); ser.close(); print('DONE', fn)
