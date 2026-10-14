# -*- coding: utf-8 -*-
import json, io, sys
sys.stdout.reconfigure(encoding='utf-8')
d = json.load(open(r'f:\ESP32-S3\data\stations.json', encoding='utf-8'))
st = d['stations']
targets = ['15007','999','2629','2781','2644','2654','2892','1364','2773','2882','2899','2400','2802']
for i, s in enumerate(st):
    u = s.get('u', '')
    for q in targets:
        if ('/live/' + q + '/') in u:
            print('idx%-5d id=%-6s n=%s  u=%s' % (i, q, s.get('n', ''), u))
