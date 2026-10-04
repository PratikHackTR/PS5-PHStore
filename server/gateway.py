#!/usr/bin/env python3
"""External PH/SP catalogues and catalogue-bound HTTPS Range gateway.

No arbitrary URL endpoint; Method1 source_group/Google Drive configuration is
kept in the native PH adapter. This service is independent of the PH relay.
"""
import argparse
import base64
import copy
import hashlib
import json
import re
import ssl
import threading
import time
import urllib.parse
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DATA = Path(__file__).resolve().parent / 'data'
MAX_RANGE = 64 * 1024 * 1024
MAX_JSON = 8 * 1024 * 1024
SLOTS = threading.BoundedSemaphore(16)
TLS = ssl.create_default_context()
USER_AGENT = 'Mozilla/5.0 (PlayStation; PlayStation 5/10.00)'
HOSTS = {'api.spectrumlibrary.online', 'store.spectrumlibrary.online', 'dlc.spectrumlibrary.online',
         'github.com', 'objects.githubusercontent.com', 'release-assets.githubusercontent.com',
         'raw.githubusercontent.com', 'archive.org', 'download.akirabox.com'}

def permitted(url):
    u = urllib.parse.urlsplit(url)
    h = (u.hostname or '').lower()
    try:
        return (u.scheme == 'https' and not u.username and not u.password
                and u.port in (None, 443) and not u.fragment
                and not any(ord(c) < 32 for c in url)
                and (h in HOSTS or h.endswith('.archive.org')))
    except ValueError:
        return False

class Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not permitted(newurl):
            fp.close()
            raise ValueError('redirect_host_not_supported')
        return super().redirect_request(req, fp, code, msg, headers, newurl)

def opener():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                      urllib.request.HTTPSHandler(context=TLS), Redirects())

def resolve_url(url):
    """Equivalent of Spectrum streamer's /dl?u= resolution, without TLS downgrade."""
    u = urllib.parse.urlsplit(url)
    if u.hostname == 'api.spectrumlibrary.online' and u.path == '/dl':
        value = urllib.parse.parse_qs(u.query).get('u', [''])[0]
        if not value:raise ValueError('unresolved_piece_url')
        decoded = base64.b64decode(value + '=' * (-len(value) % 4), validate=True).decode('utf-8')
        if not permitted(decoded):
            raise ValueError('decoded_host_not_supported')
        return decoded
    if not permitted(url):
        raise ValueError('source_host_not_supported')
    return url

def get_json(url, headers=None):
    # PH VDS is the one explicit HTTP catalogue endpoint.
    if url != 'http://148.135.181.3/phstore/catalog.json' and not permitted(url):
        raise ValueError('catalogue_source_not_supported')
    req = urllib.request.Request(url, headers={'Accept-Encoding': 'identity', 'User-Agent': USER_AGENT, **(headers or {})})
    with opener().open(req, timeout=45) as response:
        data = response.read(MAX_JSON + 1)
    if len(data) > MAX_JSON:
        raise ValueError('json_too_large')
    return json.loads(data)

def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    temporary.replace(path)

def normalize(ph, sp):
    result = copy.deepcopy(ph)
    games = result['games']
    for game in games:
        game['catalog_source'] = 'ph'
        if any(p['id'].startswith('sp-') for p in game.get('packages', [])):
            raise ValueError('ph_package_namespace_collision')
    # Merge metadata-identical rows without dropping distinct source choices.
    merged = {}
    for row in sp.get('packages', []):
        identity = {k: row.get(k) for k in ('titleId','title','version','category','region','type','standard','backport')}
        signature = json.dumps(identity, sort_keys=True)
        if signature not in merged:
            merged[signature] = copy.deepcopy(row)
            merged[signature]['downloadLinks'] = []
        for link in row.get('downloadLinks') or []:
            if link not in merged[signature]['downloadLinks']:
                merged[signature]['downloadLinks'].append(copy.deepcopy(link))
    for record in {'packages':list(merged.values())}.get('packages', []):
        kind = str(record.get('type', '')).lower().lstrip('.')
        if kind not in ('pkg', 'fpkg', 'exfat', 'ffpfsc', 'ffpkg', 'ffpfs'):
            continue
        identity = {k: record.get(k) for k in ('titleId','title','version','category','region','type','standard','backport')}
        key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:24]
        title_id = str(record.get('titleId') or '')
        platform = 'ps4' if title_id.startswith('CUSA') else 'ps5'
        links = []
        for link in record.get('downloadLinks') or []:
            if isinstance(link,dict):
                for field in ('url','urlbackport'):
                    if link.get(field):links.append((link[field],field=='urlbackport'))
            elif isinstance(link,str):links.append((link,False))
        links = list(dict.fromkeys((u,b) for u,b in links if isinstance(u,str)))
        packages = []
        for index, (url, backport) in enumerate(links):
            if not isinstance(url, str) or not permitted(url):
                continue
            pid = f'sp-{key}-{index}'
            ext = 'pkg' if kind in ('pkg','fpkg') else kind
            safe_title = re.sub(r'[^A-Za-z0-9_-]', '_', title_id)[:30] or 'SP'
            package = {'id':pid, 'type':'dlc' if record.get('category') == 'dlc' else 'base',
                       'filename':f'{safe_title}_{key[:8]}_{index}{"_backport" if backport else ""}.{ext}',
                       'version':str(record.get('version') or '1.00'), 'platform':platform,
                       'size_bytes':max(0,int(record.get('sizeBytes') or 0)),
                       'source_type':'direct_http', 'download_url':url, 'installable':True,
                       'action':'install' if ext == 'pkg' else 'download', 'file_type':kind,
                       'variant_label':('Backport' if backport else 'Standart') + (f' · Kaynak {index+1} ({urllib.parse.urlsplit(url).hostname})' if len(links)>1 else '')}
            packages.append(package)
        games.append({'id':'sp-'+key, 'catalog_source':'sp', 'platform':platform,
                      'title':str(record.get('title') or title_id), 'title_id':title_id,
                      'version':str(record.get('version') or ''), 'region':str(record.get('region') or ''),
                      'description':str(record.get('description') or ''),
                      'cover':record.get('posterUrl') or '', 'background':record.get('backUrl') or '',
                      'turkish':False, 'localization_type':'none', 'packages':packages,
                      'size_bytes':max(0,int(record.get('sizeBytes') or 0)),
                      'categories':[record.get('category') or 'game'],
                      'spectrum_type':kind,'backport':str(record.get('backport') or '')})
    result['catalog_version'] = int(result.get('catalog_version') or 1)
    result['sources'] = {'ph':'http://148.135.181.3/phstore/catalog.json',
                         'sp':'https://store.spectrumlibrary.online/api/catalog'}
    return result

def validate_manifest(manifest):
    total = manifest.get('originalFileSize')
    pieces = manifest.get('pieces')
    if type(total) is not int or total <= 0 or not isinstance(pieces, list) or not 0 < len(pieces) <= 100000:
        raise ValueError('invalid_manifest')
    result=[]
    for item in pieces:
        start,size = item.get('fileOffset'),item.get('fileSize')
        digest=item.get('hashValue')
        if (type(start) is not int or type(size) is not int or start<0 or size<=0
                or start+size>total or not isinstance(digest,str) or not re.fullmatch(r'[A-Fa-f0-9]{40}',digest)):
            raise ValueError('invalid_piece')
        result.append({'start':start,'size':size,'url':resolve_url(item['url']),'sha1':digest})
    result.sort(key=lambda p:p['start'])
    end=0
    for piece in result:
        if piece['start']<end:
            raise ValueError('overlapping_pieces')
        end=piece['start']+piece['size']
    return total,result

def read_plan(pieces, start, end):
    cursor=start
    for piece in pieces:
        left,right=piece['start'],piece['start']+piece['size']-1
        if right<cursor or left>end: continue
        if cursor<left:
            gap_end=min(end,left-1);yield None,cursor,gap_end;cursor=gap_end+1
        a,b=max(cursor,left),min(end,right)
        if a<=b: yield piece,a-left,b-left;cursor=b+1
        if cursor>end:return
    if cursor<=end:yield None,cursor,end

def parse_range(value,total):
    if not value:return 0,total-1
    match=re.fullmatch(r'bytes=(\d+)-(\d*)',value)
    if not match:raise ValueError('invalid_range')
    a=int(match[1]);b=int(match[2]) if match[2] else total-1
    if a>b or a>=total:raise ValueError('invalid_range')
    return a,min(b,total-1)

class State:
    def __init__(self,data):
        self.data=Path(data); self.lock=threading.RLock(); self.manifests={};self.direct={}
        self.reload()
    def reload(self):
        value=json.loads((self.data/'catalog.json').read_text(encoding='utf-8'))
        packages={p['id']:p for g in value['games'] if g.get('catalog_source')=='sp' for p in g['packages']}
        index=json.loads((self.data/'manifest-index.json').read_text(encoding='utf-8'))
        saved={r['manifest_url']:r['file'] for r in index if r.get('file')}
        encoded=json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()
        with self.lock:
            if getattr(self,'catalog',None)!=encoded:self.manifests.clear();self.direct.clear()
            self.catalog=encoded;self.packages=packages;self.saved=saved
    def manifest(self,pid):
        with self.lock:package=self.packages.get(pid)
        if not package or package.get('installable') is False:raise KeyError('package_not_found')
        url=package['download_url']
        if not urllib.parse.urlsplit(url).path.lower().endswith('.json'):
            raise ValueError('manifest_required')
        with self.lock:saved=self.saved.get(url)
        value=json.loads((self.data/saved).read_text(encoding='utf-8')) if saved else get_json(url)
        validate_manifest(value)
        value=copy.deepcopy(value)
        for piece in value['pieces']:piece['url']=resolve_url(piece['url'])
        return value
    def source(self,pid):
        with self.lock: package=self.packages.get(pid)
        if not package or package.get('installable') is False or package.get('action')!='install':raise KeyError('package_not_found')
        url=package['download_url']
        if urllib.parse.urlsplit(url).path.lower().endswith('.json'):
            with self.lock:
                if url in self.manifests:return self.manifests[url]
                saved=self.saved.get(url)
            value=self.manifest(pid)
            source=validate_manifest(value)
            with self.lock:self.manifests[url]=source
            return source
        url=resolve_url(url)
        with self.lock:cached=self.direct.get(url)
        if cached and time.monotonic()-cached[0]<600:return cached[1]
        total=0
        try:
            request=urllib.request.Request(url,method='HEAD',headers={'User-Agent':USER_AGENT,'Accept-Encoding':'identity'})
            with opener().open(request,timeout=30) as response:
                total=int(response.headers.get('Content-Length','0'))
        except urllib.error.HTTPError as error:error.close()
        except Exception:pass
        if total<=0:
            request=urllib.request.Request(url,headers={'Range':'bytes=0-0','User-Agent':USER_AGENT,'Accept-Encoding':'identity'})
            with opener().open(request,timeout=30) as response:
                match=re.fullmatch(r'bytes 0-0/(\d+)',response.headers.get('Content-Range',''))
                if response.status==206 and match and response.headers.get('Content-Encoding','identity')=='identity':
                    total=int(match[1])
        if total<=0:raise ValueError('source_size_unknown')
        source=total,[{'start':0,'size':total,'url':url}]
        with self.lock:self.direct[url]=(time.monotonic(),source)
        return source

def open_range(url,start,end,total):
    req=urllib.request.Request(url,headers={'Range':f'bytes={start}-{end}','Accept-Encoding':'identity','User-Agent':USER_AGENT})
    response=opener().open(req,timeout=45)
    expected=f'bytes {start}-{end}/{total}'
    if response.status!=206 or response.headers.get('Content-Range')!=expected:
        response.close();raise ValueError('upstream_range_mismatch')
    if response.headers.get('Content-Encoding','identity')!='identity':
        response.close();raise ValueError('upstream_encoding_mismatch')
    length=response.headers.get('Content-Length')
    if length is not None and int(length)!=end-start+1:
        response.close();raise ValueError('upstream_length_mismatch')
    return response

class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,fmt,*args):
        # URLs and access query parameters are excluded from logs.
        pass
    def respond(self,status,body,mime='application/json'):
        self.send_response(status);self.send_header('Content-Type',mime)
        self.send_header('Content-Length',str(len(body)));self.send_header('Connection','close')
        self.send_header('Cache-Control','no-store');self.end_headers()
        if self.command!='HEAD':self.wfile.write(body)
        self.close_connection=True
    def do_HEAD(self):self.do_GET()
    def do_GET(self):
        path=urllib.parse.urlsplit(self.path).path
        if path in ('/phstore2/catalog.json','/catalog.json'):
            self.server.state.reload();self.respond(200,self.server.state.catalog);return
        if path in ('/health','/phstore2/health'):
            self.respond(200,b'{"ok":true,"service":"PHStore2","catalogues_embedded":false}');return
        if re.fullmatch(r'/phstore2/manifest/sp-[a-f0-9]{24}-\d+\.json',path):
            try:
                pid=path.rsplit('/',1)[-1][:-5]
                value=self.server.state.manifest(pid)
                self.respond(200,json.dumps(value,separators=(',',':')).encode())
            except KeyError:self.respond(404,b'{"error":"package_not_found"}')
            except Exception:self.respond(502,b'{"error":"manifest_unavailable"}')
            return
        if not re.fullmatch(r'/phstore2/pkg/sp-[a-f0-9]{24}-\d+',path):
            self.respond(404,b'{"error":"not_found"}');return
        if not SLOTS.acquire(blocking=False):self.respond(503,b'{"error":"busy"}');return
        begun=False;source=None
        try:
            total,pieces=self.server.state.source(path.rsplit('/',1)[-1])
            a,b=parse_range(self.headers.get('Range'),total)
            if self.command!='HEAD' and b-a+1>MAX_RANGE:
                self.respond(416,b'{"error":"range_too_large"}');return
            plan=list(read_plan(pieces,a,b))
            # Open/validate the first upstream response before admitting a range.
            if self.command!='HEAD' and plan and plan[0][0] is not None:
                piece,left,right=plan[0];source=open_range(piece['url'],left,right,piece['size'])
            self.send_response(206 if self.headers.get('Range') else 200)
            self.send_header('Content-Length',str(b-a+1));self.send_header('Accept-Ranges','bytes')
            self.send_header('Content-Type','application/octet-stream');self.send_header('Connection','close')
            if self.headers.get('Range'):self.send_header('Content-Range',f'bytes {a}-{b}/{total}')
            self.end_headers();begun=True;self.close_connection=True
            if self.command=='HEAD':return
            for index,(piece,left,right) in enumerate(plan):
                remaining=right-left+1
                if piece is None:
                    while remaining:
                        count=min(remaining,65536);self.wfile.write(bytes(count));remaining-=count
                    continue
                if source is None:source=open_range(piece['url'],left,right,piece['size'])
                while remaining:
                    data=source.read(min(remaining,65536))
                    if not data:raise ValueError('upstream_truncated')
                    self.wfile.write(data);remaining-=len(data)
                source.close();source=None
            self.wfile.flush()
        except KeyError:
            if not begun:self.respond(404,b'{"error":"package_not_found"}')
        except ValueError as error:
            self.close_connection=True
            if not begun:
                if str(error)=='invalid_range':self.respond(416,b'{"error":"invalid_range"}')
                else:self.respond(502,b'{"error":"upstream_invalid_response"}')
        except (BrokenPipeError,ConnectionResetError):
            self.close_connection=True
        except Exception:
            self.close_connection=True
            if not begun:self.respond(502,b'{"error":"upstream_unavailable"}')
        finally:
            if source is not None:source.close()
            SLOTS.release()

def prepare(data,ph_url,refresh_sp=False,token_file=None):
    data=Path(data)
    ph=get_json(ph_url)
    atomic_json(data/'ph-catalog.json',ph)
    if refresh_sp:
        if not token_file:raise ValueError('spectrum_token_file_required')
        token=Path(token_file).read_text(encoding='utf-8').strip()
        if not token or '\n' in token or '\r' in token:raise ValueError('invalid_token_file')
        sp=get_json('https://store.spectrumlibrary.online/api/catalog',{'X-API-Token':token})
        if not sp.get('ok') or not sp.get('packages'):raise ValueError('invalid_spectrum_catalogue')
        atomic_json(data/'client-catalog.json',sp)
    sp=json.loads((data/'client-catalog.json').read_text(encoding='utf-8'))
    combined=normalize(ph,sp)
    atomic_json(data/'catalog.json',combined)
    # Deployable, local PS5 fallback; catalogue and manifests stay external.
    state=State(data);resolved=0;issues=[]
    for pid,package in state.packages.items():
        if package.get('action')!='download' or package['download_url'] not in state.saved:continue
        try:
            value=state.manifest(pid)
        except ValueError as error:
            issues.append({'package_id':pid,'error':str(error)})
            continue
        atomic_json(data/'resolved-manifests'/f'{pid}.json',value);resolved+=1
    rejected={item['package_id']:item['error'] for item in issues}
    for game in combined['games']:
        for package in game['packages']:
            if package['id'] in rejected:
                package['installable']=False;package['availability_error']=rejected[package['id']]
    atomic_json(data/'catalog.json',combined)
    atomic_json(data/'availability-issues.json',issues)
    print(json.dumps({'ph':len(ph['games']),'sp':sum(g.get('catalog_source')=='sp' for g in combined['games']),
                      'catalogue_bytes':(data/'catalog.json').stat().st_size,'resolved_disk_manifests':resolved}))

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--data',type=Path,default=DATA)
    parser.add_argument('--bind',default='127.0.0.1');parser.add_argument('--port',type=int,default=8762)
    parser.add_argument('--prepare',action='store_true');parser.add_argument('--refresh-sp',action='store_true')
    parser.add_argument('--token-file',type=Path)
    parser.add_argument('--ph-url',default='http://148.135.181.3/phstore/catalog.json')
    args=parser.parse_args()
    if args.prepare:
        prepare(args.data,args.ph_url,args.refresh_sp,args.token_file);return
    server=ThreadingHTTPServer((args.bind,args.port),Handler);server.daemon_threads=True
    server.state=State(args.data)
    print(f'PHStore2 gateway listening on {args.bind}:{args.port}')
    server.serve_forever()

if __name__=='__main__':main()
