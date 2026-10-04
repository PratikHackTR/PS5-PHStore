"""Generate the embedded local/dev snapshot using saved inputs only."""
import copy,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server'))
from gateway import normalize,validate_manifest,resolve_url

def main():
    data=ROOT/'server/data'; target=ROOT/'config/generated';target.mkdir(parents=True,exist_ok=True)
    value=normalize(json.loads((ROOT/'config/examples/ph-live-snapshot.json').read_text(encoding='utf-8')),json.loads((data/'client-catalog.json').read_text(encoding='utf-8')))
    # Saved PS5 metadata stays under PH; no extra source/UI is introduced.
    extra=json.loads((ROOT/'config/examples/ph-ps5-extra-source.json').read_text(encoding='utf-8'))
    covers=[]
    for r in extra:
        gid='ph-ps5-'+r['gameId'];pid='ph-ps5-'+r['id']
        package={'id':pid,'type':'base','filename':r['filename'],'version':r.get('version') or '1.00',
                 'platform':'ps5','size_bytes':r['sizeBytes'],'source_type':'archive_public',
                 'action':'download','action_type':'download_file','download_url':r['url'],'installable':True}
        if r.get('sha256'):package['sha256']=r['sha256']
        game={'id':gid,'title':r['title'],'title_id':r['titleId'],'platform':'ps5','turkish':False,
              'localization_type':'none','catalog_source':'ph','metadata_origin':'ph_ps5_extra',
              'cover':r['cover'],'background':r.get('hero',''),'description':r.get('description',''),
              'genre':r.get('genre',''),'publisher':r.get('publisher',''),'release':r.get('releaseDate',''),
              'size_bytes':r['sizeBytes'],'packages':[package]}
        if any(g['id']==gid for g in value['games']):raise ValueError('duplicate_ph_ps5_game')
        value['games'].append(game);covers.append(r['cover'])
    (target/'ph-ps5-covers.txt').write_text(''.join(url+'\n' for url in sorted(set(covers))),encoding='utf-8')
    index={r['manifest_url']:r['file'] for r in json.loads((data/'manifest-index.json').read_text(encoding='utf-8')) if r.get('file')}
    manifests={}
    for game in value['games']:
        for package in game['packages']:
            if game.get('catalog_source')!='sp' or package.get('action')!='download':continue
            source=index.get(package['download_url'])
            if not source:continue
            try:
                manifest=json.loads((data/source).read_text(encoding='utf-8'));validate_manifest(manifest)
                for piece in manifest['pieces']:piece['url']=resolve_url(piece['url'])
                manifests[package['id']]=manifest
            except ValueError as error:
                package['installable']=False;package['availability_error']=str(error)
    (target/'embedded-catalog.json').write_text(json.dumps(value,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
    (target/'embedded-manifests.json').write_text(json.dumps(manifests,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
    print(f'EMBEDDED_SNAPSHOT games={len(value["games"])} manifests={len(manifests)}')
if __name__=='__main__':main()
