import serial, time, sys

def capture(ser, secs):
    buf = []
    end = time.time() + secs
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf.append(d)
    return b"".join(buf)

ser = serial.Serial(port="COM5", baudrate=115200, timeout=0.05)

# Try several reset sequences (native USB-Serial-JTAG vs UART bridge).
seqs = [
    ("dtr-pulse", lambda s: (s.__setattr__("dtr", True), time.sleep(0.08), s.__setattr__("dtr", False))),
    ("dtr+rts",   lambda s: (s.__setattr__("dtr", True), s.__setattr__("rts", False), time.sleep(0.08),
                             s.__setattr__("dtr", False), s.__setattr__("rts", True))),
    ("dtr-high-low", lambda s: (s.__setattr__("dtr", False), time.sleep(0.05),
                                s.__setattr__("dtr", True), time.sleep(0.12), s.__setattr__("dtr", False))),
]

best = b""
for name, fn in seqs:
    try:
        fn(ser)
    except Exception as e:
        print("seq %s err %s" % (name, e), file=sys.stderr)
    out = capture(ser, 4.0)
    if len(out) > len(best):
        best = out
    time.sleep(0.5)

ser.close()
sys.stdout.buffer.write(best)
sys.stdout.flush()
print("---- captured %d bytes ----" % len(best), file=sys.stderr)
