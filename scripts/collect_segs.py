# -*- coding: utf-8 -*-
# Run alongside a device capture: poll the live m3u8 and download every .ts
# segment that appears, saving each by sequence number into a folder, so a
# frame dumped from the device can later be located exactly.
# Usage: python collect_segs.py <duration_seconds> <out_dir>
import urllib.request, os, sys, time

root = "http://ngcdn002.cnr.cn/live/jjzs/"
dur = int(sys.argv[1]) if len(sys.argv)>1 else 240
outdir = sys.argv[2] if len(sys.argv)>2 else r"D:\ESP32-S3\segs_cap"
os.makedirs(outdir, exist_ok=True)

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=15).read()

t0=time.time(); seen=set()
while time.time()-t0 < dur:
    try:
        txt = g(root+"index.m3u8").decode()
        names = [l.strip() for l in txt.splitlines() if l.strip().endswith(".ts")]
        for n in names:
            if n in seen: continue
            path=os.path.join(outdir, n)
            if not os.path.exists(path):
                data = g(root+n)
                with open(path,"wb") as f: f.write(data)
                print("saved", n, len(data), "t=%.0f" % (time.time()-t0), flush=True)
            seen.add(n)
    except Exception as e:
        print("err", e, flush=True)
    time.sleep(2)
print("collector done, %d segments" % len(os.listdir(outdir)))
