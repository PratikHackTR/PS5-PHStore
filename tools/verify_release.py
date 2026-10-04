"""Verify the prepared release files without SDK or network access."""
import hashlib,json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
manifest=json.loads((ROOT/'release-manifest.json').read_text(encoding='utf-8'))
failures=[]
for row in manifest['files']:
    path=ROOT/row['file']
    if not path.is_file():failures.append(row['file']+' missing');continue
    if path.stat().st_size!=row['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest()!=row['sha256']:
        failures.append(row['file']+' hash/size mismatch')
if failures:
    print('\n'.join(failures));raise SystemExit(1)
print(f'RELEASE_FILES=PASS ({len(manifest["files"])} files)')
print('PS5_RUNTIME=NOT_TESTED; see docs/VERIFICATION.md')
