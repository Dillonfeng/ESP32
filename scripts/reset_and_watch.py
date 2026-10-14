import serial
import time

port = 'COM5'
try:
    ser = serial.Serial(port, 115200, timeout=1)
except Exception as e:
    print(f"Cannot open {port}: {e}")
    exit(1)

# Reset the board
print("Resetting board...")
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)
ser.dtr = True

print("Listening for boot output...")
start = time.time()
while time.time() - start < 120:
    line = ser.readline().decode('utf-8', errors='replace').strip()
    if line:
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {line}")

ser.close()
print("Done")
