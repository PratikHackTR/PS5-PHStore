#!/usr/bin/env python3
"""Desktop UI preview with explicit fake console status, never an installer."""
import json
import argparse
import urllib.parse
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--dev-gdrive',action='store_true');parser.add_argument('--port',type=int,default=8763)
args=parser.parse_args()
catalog_path=ROOT/('config/examples/dev-catalog-gdrive.json' if args.dev_gdrive else 'config/generated/embedded-catalog.json')
html=(ROOT/'frontend/index.html').read_text(encoding='utf-8')
html=html.replace('/*PHSTORE_CSS*/',(ROOT/'frontend/src/style.css').read_text(encoding='utf-8'))
html=html.replace('/*PHSTORE_JS*/',(ROOT/'frontend/src/virtual-grid.js').read_text(encoding='utf-8')+'\n'+(ROOT/'frontend/src/app.js').read_text(encoding='utf-8'))
html=html.replace('KÜTÜPHANE&nbsp; / &nbsp;PH STORE','MASAÜSTÜ ÖNİZLEME · PS5 TESTİ DEĞİL')
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        u=urllib.parse.urlsplit(self.path);path=u.path
        mime='application/json';status=200
        if path=='/':body=html.encode();mime='text/html; charset=utf-8'
        elif path=='/assets/phstore-logo.png':
            icon=ROOT/'assets/generated/icon0.png'
            body=(icon if icon.is_file() else ROOT/'resim/phstorelogo.png').read_bytes();mime='image/png'
        elif path=='/api/store/catalog':body=catalog_path.read_bytes()
        else:
            value={'ok':True,'preview':True}
            if path=='/api/store/install/status':value.update(state='idle',package_id='')
            elif path=='/api/store/spectrum/status':value.update(active=False,package_id='',state='idle')
            elif path=='/api/store/installed':value.update(is_installed=False,title_id=urllib.parse.parse_qs(u.query).get('title_id',[''])[0])
            elif path=='/api/store/images/index':value.update(images=[])
            elif path in ('/api/store/info','/api/store/status','/api/store/catalog/status'):value.update(catalog_state='ready',catalog_status='ready',catalog_version=1,game_count=len(json.loads(catalog_path.read_text(encoding="utf-8"))["games"]),version='PHStore2 preview')
            elif path=='/api/store/shortcut/status':value.update(installed=False)
            body=json.dumps(value).encode()
        self.send_response(status);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    def do_POST(self):
        data=b'{"ok":false,"error":"desktop_preview_has_no_installer"}'
        self.send_response(409);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
if __name__=='__main__':
    print(f'Desktop UI preview http://127.0.0.1:{args.port} (no console installation)',flush=True)
    ThreadingHTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
