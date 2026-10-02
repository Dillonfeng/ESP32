import subprocess, os, time, sys
os.chdir(r"D:\ESP32-S3")
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
PS  = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
PORT = "COM5"
LOG = open(r"D:\ESP32-S3\flash9.log", "w", encoding="utf-8", errors="replace")

def w(s):
    LOG.write(str(s) + "\n"); LOG.flush()

def run_pio(args, logfile, timeout):
    w("== %s ==" % " ".join(args))
    with open(logfile, "w", encoding="utf-8", errors="replace") as out:
        try:
            p = subprocess.run([PIO] + args, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
            w("%s exit=%s" % (args[0], p.returncode)); return p.returncode
        except subprocess.TimeoutExpired:
            w("%s TIMEOUT after %ds" % (args[0], timeout)); return None
        except Exception as e:
            w("%s exception: %r" % (args[0], e)); return None

me = os.getpid()
w("== start %s (pid %d) port=%s ==" % (time.strftime("%H:%M:%S"), me, PORT))
subprocess.run([PS, "-NoProfile", "-File", r"D:\ESP32-S3\scripts\_kill_port.ps1", str(me)],
               capture_output=True, timeout=40)
time.sleep(8)
w("port free")

# Incremental build (only main.cpp changed -> recompile 1 file + relink).
rb = run_pio(["run"], r"D:\ESP32-S3\build9.log", 500)

ru = None
if rb == 0:
    time.sleep(1)
    ru = run_pio(["run", "-t", "upload", "--upload-port", PORT], r"D:\ESP32-S3\upload9.log", 300)

if ru == 0:
    time.sleep(2)
    w("== boot ==")
    with open(r"D:\ESP32-S3\boot9.log", "w", encoding="utf-8", errors="replace") as bt:
        try:
            subprocess.run([sys.executable, r"D:\ESP32-S3\preview\_bootlog.py", PORT],
                           stdout=bt, stderr=subprocess.STDOUT, timeout=40)
        except Exception as e:
            w("boot exception: %r" % e)

w("build=%s upload=%s" % (rb, ru))
w("== END %s ==" % time.strftime("%H:%M:%S"))
LOG.close()