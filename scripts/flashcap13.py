import subprocess, os, sys, time
os.chdir(r"D:\ESP32-S3")
me = os.getpid()
PIO = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
PS  = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
PORT = "COM5"
L = open(r"D:\ESP32-S3\flashonly.log", "w", encoding="utf-8", errors="replace")
def w(s):
    L.write(str(s) + "\n"); L.flush()
w("flash-only start pid=%d %s" % (me, time.strftime("%H:%M:%S")))
subprocess.run([PS, "-NoProfile", "-File", r"D:\ESP32-S3\scripts\_kill_port.ps1", str(me)], capture_output=True, timeout=40)
time.sleep(6)
w("port free")
with open(r"D:\ESP32-S3\upload_only.log", "w", encoding="utf-8", errors="replace") as up:
    try:
        p = subprocess.run([PIO, "run", "-t", "upload", "--upload-port", PORT], stdout=up, stderr=subprocess.STDOUT, timeout=300)
        w("upload exit=%s" % p.returncode)
    except subprocess.TimeoutExpired:
        w("upload TIMEOUT")
    except Exception as e:
        w("upload exception %r" % (e,))
w("== END %s ==" % time.strftime("%H:%M:%S"))
L.close()
