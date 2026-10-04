import unittest,sys,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'server'))
from gateway import normalize,validate_manifest
class EmbeddedTests(unittest.TestCase):
 def test_alternatives_merge(self):
  a={'titleId':'PPSA001','title':'Test','type':'exfat','downloadLinks':[{'url':'https://api.spectrumlibrary.online/a.json','urlbackport':'https://api.spectrumlibrary.online/b.json'}]}
  b=dict(a,downloadLinks=[{'url':'https://api.spectrumlibrary.online/c.json'}])
  value=normalize({'games':[]},{'packages':[a,b,a]})
  self.assertEqual(len(value['games']),1)
  packages=value['games'][0]['packages'];self.assertEqual(len(packages),3)
  self.assertIn('Backport',packages[1]['variant_label']);self.assertIn('Kaynak 3',packages[2]['variant_label'])
  self.assertEqual(len({p['id'] for p in packages}),3)
 def test_embedded_manifest_integrity(self):
  catalog=json.loads((ROOT/'config/generated/embedded-catalog.json').read_text(encoding='utf-8'))
  manifests=json.loads((ROOT/'config/generated/embedded-manifests.json').read_text(encoding='utf-8'))
  packages={p['id']:p for g in catalog['games'] for p in g['packages']}
  self.assertEqual(len(catalog['games']),1542);self.assertEqual(len(manifests),305)
  for pid,value in manifests.items():
   self.assertEqual(packages[pid]['action'],'download');self.assertTrue(packages[pid]['installable']);validate_manifest(value)
  self.assertEqual(packages['deneme-dosyasi-demo-ps4-base']['file_id'],'1H_iIPrvBa8NSgsutbnkxr_a_JHMi-fN3')
  elf=(ROOT/'build/phstr.elf').read_bytes()
  self.assertIn((ROOT/'config/generated/embedded-catalog.json').read_bytes(),elf)
  for manifest in manifests.values():self.assertIn(json.dumps(manifest,ensure_ascii=False,separators=(',',':')).encode(),elf)
if __name__=='__main__':unittest.main()

