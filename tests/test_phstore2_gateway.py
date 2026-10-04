"""Tiny byte fixtures: no game download and no PS5 runtime simulation claim."""
import base64
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gateway',ROOT/'server/gateway.py')
g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)

def piece(offset,size,url='https://github.com/a/b/releases/download/c/d'):
    return {'fileOffset':offset,'fileSize':size,'url':url,'hashValue':'a'*40}

class GatewayTests(unittest.TestCase):
    def test_source_partition_preserves_every_ph_package(self):
        ph=json.loads((ROOT/'server/data/ph-catalog.json').read_text(encoding='utf-8'))
        sp=json.loads((ROOT/'server/data/client-catalog.json').read_text(encoding='utf-8'))
        combined=g.normalize(ph,sp)
        actual=[x for x in combined['games'] if x['catalog_source']=='ph']
        self.assertEqual([x['packages'] for x in actual],[x['packages'] for x in ph['games']])
        self.assertEqual(len(actual),len(ph['games']))
        spectrum=[x for x in combined['games'] if x['catalog_source']=='sp']
        self.assertEqual(len(spectrum),531) # One byte-identical catalogue duplicate.
        self.assertEqual(sum(len(x['packages']) for x in spectrum),534) # Includes all three backport URLs.
        ids=[p['id'] for game in combined['games'] for p in game['packages']]
        self.assertEqual(len(ids),len(set(ids)))
        self.assertTrue(all(x['cover'].startswith('https://') for x in spectrum))
        for game in spectrum:
            for p in game['packages']:
                self.assertEqual(p['action'],'install' if p['filename'].endswith('.pkg') else 'download')

    def test_url_wrapper_and_host_boundary(self):
        original='https://github.com/a/b/releases/download/c/d'
        encoded=base64.b64encode(original.encode()).decode()
        self.assertEqual(g.resolve_url('https://api.spectrumlibrary.online/dl?u='+encoded),original)
        for url in ['http://github.com/a','https://github.com.evil.test/a','https://user:pass@github.com/a','https://127.0.0.1/a']:
            self.assertFalse(g.permitted(url))
        bad=base64.b64encode(b'https://127.0.0.1/secret').decode()
        with self.assertRaises(ValueError):g.resolve_url('https://api.spectrumlibrary.online/dl?u='+bad)

    def test_manifest_overlap_hash_bounds_and_gaps(self):
        total,pieces=g.validate_manifest({'originalFileSize':15,'pieces':[piece(10,5),piece(0,5)]})
        plan=list(g.read_plan(pieces,3,12))
        self.assertEqual([(p is None,a,b) for p,a,b in plan],[(False,3,4),(True,5,9),(False,0,2)])
        for manifest in [
            {'originalFileSize':10,'pieces':[piece(0,6),piece(5,5)]},
            {'originalFileSize':10,'pieces':[piece(9,2)]},
            {'originalFileSize':10,'pieces':[{**piece(0,10),'hashValue':'invalid'}]},
        ]:
            with self.assertRaises(ValueError):g.validate_manifest(manifest)

    def test_all_saved_manifests_validate(self):
        files=list((ROOT/'server/data/manifests').glob('*.json'))
        self.assertEqual(len(files),363)
        invalid=[]
        for file in files:
            try:g.validate_manifest(json.loads(file.read_text(encoding='utf-8')))
            except ValueError as error:invalid.append(str(error))
        self.assertEqual(invalid,['unresolved_piece_url'])

    def test_resolved_disk_manifests_keep_hashes_and_offsets(self):
        state=g.State(ROOT/'server/data')
        files=list((ROOT/'server/data/resolved-manifests').glob('*.json'))
        self.assertEqual(len(files),305)
        for file in files:
            value=json.loads(file.read_text(encoding='utf-8'))
            self.assertEqual(value,state.manifest(file.stem))
            self.assertTrue(all('/dl?u=' not in p['url'] for p in value['pieces']))
            g.validate_manifest(value)

    def test_exact_upstream_content_range_required(self):
        class Response(io.BytesIO):
            status=206
            headers={'Content-Range':'bytes 2-4/8','Content-Length':'3'}
        class Opener:
            def open(self,*args,**kwargs):return Response(b'234')
        with patch.object(g,'opener',return_value=Opener()):
            with g.open_range('https://github.com/a',2,4,8) as r:self.assertEqual(r.read(),b'234')
            with self.assertRaises(ValueError):g.open_range('https://github.com/a',1,4,8)

    def test_direct_size_head_fallback_and_cache(self):
        pid='sp-test';url='https://github.com/a/pkg'
        state=g.State(ROOT/'server/data')
        state.packages[pid]={'action':'install','download_url':url,'installable':True}
        calls=[]
        class Response(io.BytesIO):
            status=206;headers={'Content-Range':'bytes 0-0/12345'}
        class Opener:
            def open(self,request,**kwargs):
                calls.append(request.get_method())
                if request.get_method()=='HEAD':raise urllib.error.HTTPError(url,405,'Method Not Allowed',{},None)
                return Response(b'x')
        with patch.object(g,'opener',return_value=Opener()):
            self.assertEqual(state.source(pid)[0],12345)
            self.assertEqual(state.source(pid)[0],12345)
        self.assertEqual(calls,['HEAD','GET'])

    def test_http_range_crosses_pieces_and_zero_gap(self):
        pid='sp-'+'a'*24+'-0'
        pieces=[{'start':0,'size':5,'url':'https://github.com/first'},
                {'start':10,'size':5,'url':'https://github.com/last'}]
        class State:
            def source(self,requested):
                if requested!=pid:raise KeyError(requested)
                return 15,pieces
        server=g.ThreadingHTTPServer(('127.0.0.1',0),g.Handler);server.state=State()
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        calls=[]
        def fetch(url,a,b,total):
            calls.append((url,a,b,total))
            data=b'ABCDE' if url.endswith('first') else b'KLMNO'
            return io.BytesIO(data[a:b+1])
        base=f'http://127.0.0.1:{server.server_port}'
        try:
            with patch.object(g,'open_range',side_effect=fetch):
                req=urllib.request.Request(base+'/phstore2/pkg/'+pid,headers={'Range':'bytes=3-12'})
                with urllib.request.urlopen(req) as response:
                    self.assertEqual(response.status,206)
                    self.assertEqual(response.headers['Content-Range'],'bytes 3-12/15')
                    self.assertEqual(response.read(),b'DE'+bytes(5)+b'KLM')
                self.assertEqual([(x[1],x[2]) for x in calls],[(3,4),(0,2)])
                req=urllib.request.Request(base+'/phstore2/pkg/'+pid,method='HEAD')
                with urllib.request.urlopen(req) as response:
                    self.assertEqual(response.headers['Content-Length'],'15')
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    urllib.request.urlopen(base+'/phstore2/pkg/'+pid+'unknown')
                self.assertEqual(caught.exception.code,404)
                caught.exception.close()
                req=urllib.request.Request(base+'/phstore2/pkg/'+pid,headers={'Range':'bytes=15-20'})
                with self.assertRaises(urllib.error.HTTPError) as caught:urllib.request.urlopen(req)
                self.assertEqual(caught.exception.code,416);caught.exception.close()
        finally:server.shutdown();server.server_close();thread.join(timeout=3)

if __name__=='__main__':unittest.main()
