# -*- coding: utf-8 -*-
import json, sys
sys.stdout.reconfigure(encoding='utf-8')
d = json.load(open(r'f:\ESP32-S3\.cache\fwmeta.json', encoding='utf-8'))
print('name:', d.get('name'))
vers = d.get('version', {}).get('versions', [])
for v in vers:
    ver = v.get('version', '')
    if ver.startswith('3.20016'):
        print('version:', ver)
        for f in v.get('files', []):
            print('  file:', f.get('name'),
                  ' size=%.1fMB' % (f.get('size', 0) / 1048576),
                  ' system=', f.get('system'))
            print('  url :', f.get('url'))
