import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
BAUD = 115200
DUR  = int(sys.argv[2]) if len(sys.argv) > 2 else 20

print(f"[capture] opening {PORT} @ {BAUD} for {DUR}s ...")
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.open()

# Reset ESP32 (CH340/CP210x DTR/RTS pulse) so boot self-test re-runs
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.05)
ser.dtr = True
time.sleep(0.05)
ser.reset_input_buffer()
ser.reset_output_buffer()

end = time.time() + DUR
buf = b""
while time.time() < end:
    d = ser.read(4096)
    if d:
        buf += d
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()

ser.close()
print("\n[capture] done")
