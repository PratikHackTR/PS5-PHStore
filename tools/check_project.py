"""Offline release checks. Build first; no PS5, provider or download claims."""
from pathlib import Path
import subprocess,sys,shutil

ROOT=Path(__file__).resolve().parents[1]
def run(args):
    subprocess.run(args,cwd=ROOT,check=True)

for pattern in ['test_phstore2_gateway.py','test_gdrive_metadata.py','test_embedded_catalog.py']:
    run([sys.executable,'-m','unittest','discover','-s','tests','-p',pattern,'-v'])
node=shutil.which('node')
if node:
    for name in ['test_virtual_grid.js','turkish_category_test.js','download_ui_test.js']:
        run([node,'tests/'+name])
else:
    print('FRONTEND_TESTS=SKIP (Node.js is not installed)')
print('OFFLINE_PROJECT_CHECKS=PASS; PS5_RUNTIME=NOT_TESTED')
