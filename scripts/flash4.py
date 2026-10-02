import subprocess, os, time, sys
os.chdir(r"D:\ESP32-S3")
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
PS  = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
PORT = "COM5"
LOG = open(r"D:\ESP32-S3\flash4.log", "w", encoding="utf-8", errors="replace")

def w(s):
    LOG.write(str(s) + "\n")
    LOG.flush()

def run(args, logfile, timeout):
    w("== %s ==" % " ".join(args[1:4]))
    with open(logfile, "w", encoding="utf-8", errors="replace") as out:
        try:
            p = subprocess.run([PIO] + args, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
            w("exit=%s" % p.returncode)
            return p.returncode
        except Exception as e:
            w("exception: %r" % e)
            return None

me = os.getpid()
w("== start %s (pid %d) port=%s ==" % (time.strftime("%H:%M:%S"), me, PORT))

# 1) Free the serial port.
w("killing port holders...")
subprocess.run([PS, "-NoProfile", "-File", r"D:\ESP32-S3\scripts\_kill_port.ps1", str(me)],
               capture_output=True, timeout=40)
time.sleep(6)
w("port free")

# 2) Build first (captures any real compile error into build4.log).
rb = run(["run"], r"D:\ESP32-S3\build4.log", 400)

# 3) If build ok, upload firmware (FS already flashed successfully).
ru = None
if rb == 0:
    time.sleep(1)
    ru = run(["run", "-t", "upload", "--upload-port", PORT], r"D:\ESP32-S3\upload4.log", 300)

# 4) Boot capture.
if ru == 0:
    time.sleep(2)
    w("== boot ==")
    with open(r"D:\ESP32-S3\boot4.log", "w", encoding="utf-8", errors="replace") as bt:
        try:
            subprocess.run([sys.executable, r"D:\ESP32-S3\preview\_bootlog.py", PORT],
                           stdout=bt, stderr=subprocess.STDOUT, timeout=40)
        except Exception as e:
            w("boot exception: %r" % e)

w("build=%s upload=%s" % (rb, ru))
w("== END %s ==" % time.strftime("%H:%M:%S"))
LOG.close()
