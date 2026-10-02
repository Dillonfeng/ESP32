import subprocess, os, time, sys, shutil
os.chdir(r"D:\ESP32-S3")
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
PS  = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
PORT = "COM5"
BUILD = r"D:\ESP32-S3\.pio\build"
LOG = open(r"D:\ESP32-S3\flash8.log", "w", encoding="utf-8", errors="replace")

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

# 1) Free serial port.
subprocess.run([PS, "-NoProfile", "-File", r"D:\ESP32-S3\scripts\_kill_port.ps1", str(me)],
               capture_output=True, timeout=40)
time.sleep(8)
w("port free")

# 2) Nuke ENTIRE build dir (fresh SCons DB -> no LDF scan hang).
w("removing %s ..." % BUILD)
try:
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD, onerror=lambda e, p, _: (os.chmod(p, 0o755), os.unlink(p)) if os.path.exists(p) else None)
        w("build dir removed")
    else:
        w("build dir already gone")
except Exception as e:
    w("remove exception: %r" % e)
time.sleep(3)

# 3) Fresh full build (generous timeout).
rb = run_pio(["run"], r"D:\ESP32-S3\build8.log", 900)

# 4) Upload.
ru = None
if rb == 0:
    time.sleep(1)
    ru = run_pio(["run", "-t", "upload", "--upload-port", PORT], r"D:\ESP32-S3\upload8.log", 300)

# 5) Boot log.
if ru == 0:
    time.sleep(2)
    w("== boot ==")
    with open(r"D:\ESP32-S3\boot8.log", "w", encoding="utf-8", errors="replace") as bt:
        try:
            subprocess.run([sys.executable, r"D:\ESP32-S3\preview\_bootlog.py", PORT],
                           stdout=bt, stderr=subprocess.STDOUT, timeout=40)
        except Exception as e:
            w("boot exception: %r" % e)

w("build=%s upload=%s" % (rb, ru))
w("== END %s ==" % time.strftime("%H:%M:%S"))
LOG.close()