import importlib.util,unittest
from pathlib import Path
s=importlib.util.spec_from_file_location('metadata',Path(__file__).resolve().parents[1]/'tools/gdrive_metadata_generator.py')
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class Metadata(unittest.TestCase):
    def test_unique(self):
        v=m.build('PS2Games',[{'ID':'drive_id','Path':'sub/Avatar2.pkg','Size':100,'IsDir':False}]);self.assertEqual(v['filename_to_file_id'],{'Avatar2.pkg':'drive_id'});self.assertEqual(v['path_to_file_id'],{'sub/Avatar2.pkg':'drive_id'})
    def test_collision(self):
        v=m.build('PS5Games',[{'ID':'one','Path':'a/file.exfat','Size':1},{'ID':'two','Path':'b/file.exfat','Size':2}]);self.assertNotIn('file.exfat',v['filename_to_file_id']);self.assertEqual(len(v['ambiguous_filenames']['file.exfat']),2)
    def test_invalid_private_missing(self):
        v=m.build('PS4Games',[{'Path':'file.pkg','Size':100},{'ID':'id&bad','Path':'file.pkg','Size':100},{'ID':'good','Path':'../file.pkg','Size':100}]);self.assertEqual(len(v['rejected']),3);self.assertEqual(v['files'],[])
if __name__=='__main__':unittest.main()
