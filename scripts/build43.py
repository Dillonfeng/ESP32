import subprocess, os, sys, time

os.chdir(r"d:\ESP32-S3")
pio = r"C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe"
out = open(r"d:\ESP32-S3\build43.log", "w", encoding="utf-8", errors="replace")
err = open(r"d:\ESP32-S3\build43_err.log", "w", encoding="utf-8", errors="replace")

log = open(r"d:\ESP32-S3\build43_prog.log", "w", encoding="utf-8")
log.write("build started %s\n" % time.strftime("%H:%M:%S"))
log.flush()

p = subprocess.Popen([pio, "run"], stdout=out, stderr=err, cwd=r"d:\ESP32-S3")
while p.poll() is None:
    time.sleep(2)
    log.write("still running... %s\n" % time.strftime("%H:%M:%S"))
    log.flush()

log.write("build finished exit=%d %s\n" % (p.returncode, time.strftime("%H:%M:%S")))
log.close()
out.close()
err.close()
