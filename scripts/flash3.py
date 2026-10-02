import subprocess, os, time, sys
os.chdir(r"D:\ESP32-S3")
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
PS  = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
PORT = "COM5"
LOG = open(r"D:\ESP32-S3\flash3.log", "w", encoding="utf-8", errors="replace")

def w(s):
    LOG.write(str(s) + "\n")
    LOG.flush()

def run_pio(target, logfile, timeout):
    w("== %s -> %s ==" % (target, logfile))
    with open(logfile, "w", encoding="utf-8", errors="replace") as up:
        try:
            p = subprocess.run([PIO, "run", "-t", target, "--upload-port", PORT],
                               stdout=up, stderr=subprocess.STDOUT, timeout=timeout)
            w("%s exit=%s" % (target, p.returncode))
            return p.returncode
        except Exception as e:
            w("%s exception: %r" % (target, e))
            return None

me = os.getpid()
w("== start %s (pid %d) port=%s ==" % (time.strftime("%H:%M:%S"), me, PORT))

# 1) Free the serial port: kill leftover esptool/pio/python (except self).
w("killing port holders...")
subprocess.run([PS, "-NoProfile", "-File", r"D:\ESP32-S3\scripts\_kill_port.ps1", str(me)],
               capture_output=True, timeout=40)
time.sleep(6)
w("port free (waited 6s)")

# 2) Upload filesystem FIRST (radio_meta.bin / stations.bin / fonts), then firmware.
r1 = run_pio("uploadfs", r"D:\ESP32-S3\uploadfs2.log", 300)
r2 = run_pio("upload", r"D:\ESP32-S3\upload2.log", 300)

# 3) Boot capture.
time.sleep(2)
w("== boot ==")
with open(r"D:\ESP32-S3\boot2.log", "w", encoding="utf-8", errors="replace") as bt:
    try:
        subprocess.run([sys.executable, r"D:\ESP32-S3\preview\_bootlog.py", PORT],
                       stdout=bt, stderr=subprocess.STDOUT, timeout=40)
    except Exception as e:
        w("boot exception: %r" % e)

w("uploadfs=%s upload=%s" % (r1, r2))
w("== END %s ==" % time.strftime("%H:%M:%S"))
LOG.close()
