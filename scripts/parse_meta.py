# -*- coding: utf-8 -*-
import json, sys
sys.stdout.reconfigure(encoding='utf-8')

def show(path, want_prefix):
    d = json.load(open(path, encoding='utf-8'))
    print('### package:', d.get('name'))
    for v in d.get('versions', []):
        ver = v.get('name', '')
        if ver.startswith(want_prefix):
            print('version:', ver, ' unpacked=%.0fMB' % (v.get('unpacked_size',0)/1048576))
            for f in v.get('files', []):
                print('  name   :', f.get('name'))
                print('  size   : %.1f MB (%d bytes)' % (f.get('size', 0)/1048576, f.get('size', 0)))
                print('  system :', f.get('system'))
                print('  checksum:', f.get('checksum'))
                print('  url    :', f.get('download_url') or f.get('url'))
                print()

show(r'f:\ESP32-S3\.cache\meta_framework-arduinoespressif32.json', '3.20016')
show(r'f:\ESP32-S3\.cache\meta_tool-esptoolpy.json', '1.40501')
