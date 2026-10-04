"""One-time, read-only rclone Drive listing -> explicit IDs; no runtime dependency.
See https://rclone.org/commands/rclone_lsjson/ for backend-dependent ID fields.
"""
import argparse,json,re,subprocess
from collections import defaultdict
from pathlib import Path,PurePosixPath

def build(group,items):
    if group not in {'PS2Games','PS4Games','PS5Games'}:raise ValueError('unknown_group')
    rows=[];rejected=[];by_name=defaultdict(list);by_path=defaultdict(list)
    for item in items:
        if item.get('IsDir'):continue
        ident=item.get('ID');path=item.get('Path');size=item.get('Size')
        if not isinstance(ident,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}',ident) or not isinstance(path,str) or not path or path.startswith('/') or '..' in PurePosixPath(path).parts or '\\' in path or not isinstance(size,int) or isinstance(size,bool) or size<0:
            rejected.append({'path':path,'reason':'missing_or_invalid_drive_id_path_size'});continue
        row={'source_group':group,'path':path,'filename':PurePosixPath(path).name,'file_id':ident,'size_bytes':size}
        rows.append(row);by_name[row['filename']].append(row);by_path[path].append(row)
    # Never resolve duplicate names/paths by selecting the first item.
    unique_name={k:v[0]['file_id'] for k,v in by_name.items() if len(v)==1}
    unique_path={k:v[0]['file_id'] for k,v in by_path.items() if len(v)==1}
    return {'schema_version':1,'source_group':group,'files':rows,'filename_to_file_id':unique_name,
            'path_to_file_id':unique_path,'ambiguous_filenames':{k:v for k,v in by_name.items() if len(v)>1},
            'ambiguous_paths':{k:v for k,v in by_path.items() if len(v)>1},'rejected':rejected,
            'sha256_generated':False,'note':'IDs do not make private files public. SHA-256 must come from a trusted source; MD5 is not SHA-256.'}
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--group',required=True,choices=['PS2Games','PS4Games','PS5Games'])
    source=p.add_mutually_exclusive_group(required=True);source.add_argument('--remote');source.add_argument('--listing',type=Path)
    p.add_argument('--rclone',default='rclone');p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    if a.output.exists():p.error('output already exists; choose a new output path')
    if a.remote:
        if a.remote.startswith('-') or ':' not in a.remote:p.error('remote must be an existing rclone remote:path')
        # OAuth/rclone configuration remains on the VDS. No body download, mutation or share change.
        result=subprocess.run([a.rclone,'lsjson',a.remote,'--recursive','--files-only','--no-modtime','--no-mimetype'],check=True,capture_output=True,text=True)
        items=json.loads(result.stdout)
    else:items=json.loads(a.listing.read_text(encoding='utf-8-sig'))
    if not isinstance(items,list):p.error('lsjson input must be an array')
    value=build(a.group,items);a.output.parent.mkdir(parents=True,exist_ok=True)
    with a.output.open('x',encoding='utf-8') as f:json.dump(value,f,ensure_ascii=False,indent=2)
    print(json.dumps({'files':len(value['files']),'ambiguous_names':len(value['ambiguous_filenames']),'rejected':len(value['rejected'])}))
if __name__=='__main__':main()
