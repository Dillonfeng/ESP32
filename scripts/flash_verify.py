import subprocess, os, time, sys
os.chdir(r"D:\ESP32-S3")
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
LOG = open(r"D:\ESP32-S3\flash_verify.log", "w", encoding="utf-8", errors="replace")

def w(s):
    LOG.write(str(s) + "\n")
    LOG.flush()

w("== start %s ==" % time.strftime("%H:%M:%S"))
# Free COM5: kill leftover flasher procs (NOT python.exe, that would kill this script)
for img in ("esptool.exe", "pio.exe"):
    subprocess.run(["taskkill", "/F", "/IM", img], capture_output=True)
time.sleep(2)

w("== upload ==")
try:
    p = subprocess.run([PIO, "run", "-t", "upload", "--upload-port", "COM5"],
                       capture_output=True, text=True, errors="replace", timeout=300)
    if p.stdout:
        w(p.stdout[-4000:])
    if p.stderr:
        w("STDERR: " + p.stderr[-2000:])
    w("upload exit=%d" % p.returncode)
except Exception as e:
    w("upload exception: %r" % e)
time.sleep(2)

w("== boot ==")
try:
    p2 = subprocess.run([sys.executable, r"D:\ESP32-S3\preview\_bootlog.py", "COM5"],
                        capture_output=True, text=True, errors="replace", timeout=40)
    if p2.stdout:
        w(p2.stdout[-4000:])
    if p2.stderr:
        w("BOOT STDERR: " + p2.stderr[-1000:])
except Exception as e:
    w("boot exception: %r" % e)

w("== done %s ==" % time.strftime("%H:%M:%S"))
LOG.close()
