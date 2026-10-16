import datetime as dt
import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('r51', ROOT/'android'/'gpu_lab_collector.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ArchiveRouting(unittest.TestCase):
 def setUp(self):
  t=tempfile.TemporaryDirectory();self.addCleanup(t.cleanup)
  self.base=pathlib.Path(t.name)
  self.kit=self.base/'Download'/'GPU-Lab'
  self.docs=self.base/'Documents';self.docs.mkdir()
  self.run='run-20261015-test-routing-1';self.token='ab'*32
  self.c=m.Collector(self.kit,self.run,self.token,self.docs,grace=0)
 def make_session(self,sid):
  win=self.kit/'Logs'/sid/'Windows';win.mkdir(parents=True)
  now=dt.datetime.now(dt.timezone.utc)
  m.atomic_json(win/'session.json',{'run':self.run,'token':self.token,'session':sid,'game':sid.split('-2026')[0],
    'state':'finished','reason':'PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN',
    'started_at':(now-dt.timedelta(seconds=1)).isoformat(), 'ended_at':now.isoformat()})
  (win/'telemetry').mkdir();(win/'telemetry'/'metrics.csv').write_text('cpu,ram\n12,512\n')
  return win
 def test_game_zips_inside_logs_and_return_only_at_diagnostic_folder(self):
  sids=['speed.exe-20261015-213001-p111-aa11','dirt3.exe-20261015-213101-p112-bb22']
  for sid in sids:self.make_session(sid)
  self.c.tick();self.c.tick()
  m.guarded_finalize(self.c,'TEST_FINISHED')
  zips=sorted(self.c.archives.glob('*.zip'))
  self.assertEqual([z.name for z in zips],sorted([s+'.zip' for s in sids]))
  self.assertEqual(len(list(self.c.returns.glob('*.zip'))),1)
  r=next(self.c.returns.glob('GPU-Lab-RETURN-*.zip'))
  with zipfile.ZipFile(r) as z:
   self.assertIsNone(z.testzip())
   index=json.loads(z.read('META/RETURN-INDEX.json'))
   self.assertEqual(index['game_zips_embedded'],False)
   self.assertEqual(sorted(x['file'] for x in index['archives']),sorted(s+'.zip' for s in sids))
   self.assertTrue(all(x['relative_path'].startswith('GPU-Lab/Logs/') for x in index['archives']))
   self.assertFalse(any(n.startswith('Windows/telemetry/') for n in z.namelist()))
  for z in zips:
   self.assertEqual(z.stem,z.stem)
   self.assertTrue((self.kit/'Logs'/z.stem/'Windows'/'session.json').exists())
   with zipfile.ZipFile(z) as f:
    self.assertIsNone(f.testzip())
    self.assertIn('Windows/telemetry/metrics.csv', f.namelist())
    for line in f.read('META/SHA256SUMS.txt').decode().splitlines():
     h,name=line.split('  ',1)
     self.assertEqual(h,hashlib.sha256(f.read(name)).hexdigest())
 def test_no_game_produces_logs_zip_and_separate_return(self):
  m.guarded_finalize(self.c,'NO_GAME_TEST')
  self.assertEqual(len(list(self.c.archives.glob('NO-GAME-*.zip'))),1)
  returns=list(self.c.returns.glob('GPU-Lab-RETURN-*.zip'))
  self.assertEqual(len(returns),1)
  with zipfile.ZipFile(returns[0]) as z:
   self.assertEqual(json.loads(z.read('META/RETURN-INDEX.json'))['sessions_reported'],1)
 def test_return_idempotent_and_never_overwrites(self):
  m.guarded_finalize(self.c,'FIRST')
  ret=next(self.c.returns.glob('*.zip'))
  original=ret.read_bytes()
  m.create_return(self.c,'SECOND')
  self.assertEqual(original,ret.read_bytes())
  self.assertEqual(len(list(self.c.returns.glob('*.zip'))),1)
 def test_return_independent_from_game_packager_failure(self):
  self.make_session('speed.exe-20261015-213002-p113-cc22')
  with mock.patch.object(m,'pack_immutable',side_effect=RuntimeError('PROBE_PACKAGER_BROKEN')):
   with self.assertRaises(RuntimeError):
    m.guarded_finalize(self.c,'FAIL_PACKAGER')
  ret=next(self.c.returns.glob('*.zip'))
  with zipfile.ZipFile(ret) as z:
   self.assertIsNone(z.testzip())
   self.assertIn('Android/collector-events.jsonl',z.namelist())
  self.assertEqual(len(list(self.c.archives.glob('*.zip'))),0)
 def test_game_zip_never_overwritten_during_return(self):
  self.make_session('speed.exe-20261015-213003-p114-dd22')
  self.c.tick();self.c.tick()
  game=next(self.c.archives.glob('*.zip'))
  preserved=game.read_bytes()
  m.guarded_finalize(self.c,'STOP')
  self.assertEqual(game.read_bytes(),preserved)
  self.assertEqual(len(list(self.c.archives.glob('*.zip'))),1)
 def test_without_os_link(self):
  self.make_session('speed.exe-20261015-213004-p115-ee22')
  with mock.patch.object(m.os,'link',None):
   self.c.tick();self.c.tick();m.guarded_finalize(self.c,'NO_OS_LINK')
  self.assertEqual(len(list(self.c.archives.glob('*.zip'))),1)
  self.assertEqual(len(list(self.c.returns.glob('*.zip'))),1)

if __name__=='__main__':unittest.main()
