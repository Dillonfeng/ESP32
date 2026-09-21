# -*- coding: utf-8 -*-
# 通过 DTR/RTS 硬复位 ESP32 (EN=RTS, GPIO0=DTR)
# 进入下载模式: DTR=True(GPIO0=LOW), RTS=False(EN=HIGH) -> 再 RTS=True(EN=LOW) -> 再 RTS=False(EN=HIGH 释放)
# 复位运行: RTS=True(EN=LOW) -> RTS=False(EN=HIGH)
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
duration = int(sys.argv[3]) if len(sys.argv) > 3 else 25

try:
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.1
    s.dtr = False
    s.rts = False
    s.open()
except Exception as e:
    print(f"[open fail] {e}")
    sys.exit(1)

print(f"[reset {port} and listen {duration}s]")
# 硬复位: 拉低 EN (RTS=True) 50ms, 然后释放 (RTS=False)
s.dtr = False
s.rts = True   # EN=LOW -> reset
time.sleep(0.1)
s.rts = False  # EN=HIGH -> run
time.sleep(0.05)

t0 = time.time()
while time.time() - t0 < duration:
    b = s.read(256)
    if b:
        try:
            print(b.decode("utf-8", "replace"), end="", flush=True)
        except Exception:
            print(repr(b), end="", flush=True)
s.close()
print("\n[done]")
