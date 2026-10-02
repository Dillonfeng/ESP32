import subprocess, os, time, sys
os.chdir(r"D:\ESP32-S3")
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
PS  = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
LOG = open(r"D:\ESP32-S3\flash2.log", "w", encoding="utf-8", errors="replace")

def w(s):
    LOG.write(str(s) + "\n")
    LOG.flush()

me = os.getpid()
w("== start %s (pid %d) ==" % (time.strftime("%H:%M:%S"), me))

# 1) Free the serial port: kill leftover esptool/pio/python (except self).
w("killing port holders...")
# pass our own PID so the kill script spares this process (it is a python.exe too)
subprocess.run([PS, "-NoProfile", "-File", r"D:\ESP32-S3\scripts\_kill_port.ps1", str(me)],
               capture_output=True, timeout=40)
time.sleep(6)
w("port free (waited 6s)")

# 2) Flash (stream to upload2.log so progress is visible via read_files).
w("== upload ==")
with open(r"D:\ESP32-S3\upload2.log", "w", encoding="utf-8", errors="replace") as up:
    try:
        p = subprocess.run([PIO, "run", "-t", "upload", "--upload-port", "COM5"],
                           stdout=up, stderr=subprocess.STDOUT, timeout=300)
    except Exception as e:
        w("upload exception: %r" % e)
        p = None
    w("upload exit=%s" % (p.returncode if p else "ERR"))

# 2b) Flash LittleFS (festival.json 改 http 等数据变更必须推上去).
w("== uploadfs ==")
with open(r"D:\ESP32-S3\uploadfs2.log", "w", encoding="utf-8", errors="replace") as uf:
    try:
        p2 = subprocess.run([PIO, "run", "-t", "uploadfs", "--upload-port", "COM5"],
                            stdout=uf, stderr=subprocess.STDOUT, timeout=180)
    except Exception as e:
        w("uploadfs exception: %r" % e)
        p2 = None
    w("uploadfs exit=%s" % (p2.returncode if p2 else "ERR"))

# 3) Boot capture.
time.sleep(2)
w("== boot ==")
with open(r"D:\ESP32-S3\boot2.log", "w", encoding="utf-8", errors="replace") as bt:
    try:
        subprocess.run([sys.executable, r"D:\ESP32-S3\preview\_bootlog.py", "COM5"],
                       stdout=bt, stderr=subprocess.STDOUT, timeout=40)
    except Exception as e:
        w("boot exception: %r" % e)

w("== END %s ==" % time.strftime("%H:%M:%S"))
LOG.close()
