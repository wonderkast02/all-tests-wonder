import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'android'))
from gpu_lab_collector import Collector


class LifecycleR54(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.base=Path(self.tmp.name)
        self.lab=self.base/'Download'/'GPU-Lab'
        self.docs=self.base/'Documents'
        self.run='run-unit-12345678900'
        self.token='be'*32
        self.ctrl=self.lab/'Logs'/'_control'/self.run/'windows'
        self.ctrl.mkdir(parents=True)

    def add_session(self,sid,run,token,ended=False):
        directory=self.lab/'Logs'/sid/'Windows';directory.mkdir(parents=True)
        now=dt.datetime.now(dt.timezone.utc).isoformat()
        meta={'schema':'gpu-lab-session-v1','run':run,'token':token,'session':sid,
              'game':'speed.exe','game_title':'Need For Speed','started_at':now,
              'state':'finished' if ended else 'active',
              'reason':'PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN' if ended else 'OBSERVING',
              'pid':123}
        if ended:meta['ended_at']=now
        (directory/'session.json').write_text(json.dumps(meta))
        (directory/'events.jsonl').write_text('')
        return directory

    def test_foreign_sessions_logged_once_and_never_archived(self):
        self.add_session('old.exe-20261001-p11-hhh','prior-run','cc'*32,True)
        c=Collector(self.lab,self.run,self.token,self.docs,[],0)
        try:
            for _ in range(6):c.tick()
            data=(c.host/'collector-events.jsonl').read_text()
            self.assertEqual(data.count('session_token_mismatch_ignored'),1)
            self.assertEqual(len(list((c.root/'Logs').glob('*.zip'))),0)
        finally:
            if c.diag: c.diag.finish()

    def test_stale_worker_returns_partial_without_claiming_game_exit(self):
        self.add_session('speed.exe-20261016-p123-xyz',self.run,self.token,False)
        (self.ctrl/'started.ok').write_text(self.token+'\n')
        status=self.ctrl/'run-status.json';status.write_text('{"state":"capturing"}')
        stale=time.time()-96;os.utime(status,(stale,stale))
        cmd=[sys.executable,str(ROOT/'android'/'gpu_lab_collector.py'),'--root',str(self.lab),
             '--run',self.run,'--token',self.token,'--documents',str(self.docs),'--grace','0']
        proc=subprocess.run(cmd,capture_output=True,text=True,timeout=25)
        self.assertEqual(proc.returncode,0,proc.stderr)
        self.assertIn('GPU_LAB_RETURN=',proc.stdout)
        zips=list((self.lab/'Logs').glob('speed.exe*.zip'))
        self.assertEqual(len(zips),1)
        with zipfile.ZipFile(zips[0]) as f:
            self.assertIsNone(f.testzip())
            report=json.loads(f.read('META/COLLECTION-REPORT.json'))
            self.assertIn('WINDOWS_SESSION_NOT_FINISHED_OR_INTERRUPTED',report['warnings'])
        ret=list((self.base/'Download'/'Log Termux').glob('*.zip'))
        self.assertEqual(len(ret),1)
        with zipfile.ZipFile(ret[0]) as f:
            self.assertIsNone(f.testzip())
            self.assertIn('WINDOWS_OBSERVER_STALE',f.read('META/RETURN-INDEX.json').decode())

    def test_windows_source_contains_default_auto_exit_and_exclusive_controller(self):
        src=(ROOT/'windows'/'main_windows.go').read_text()
        self.assertIn('post_game_idle_auto_finish',src)
        self.assertIn('15*time.Second',src)
        self.assertIn('controller.lock',src)
        self.assertIn('os.Exit(88)',src)
        self.assertIn('enumerateProcesses()',src)

    def test_global_win32_hotkey_registration_is_never_called(self):
        text=(ROOT/'windows'/'gui_windows.go').read_text()
        block=text.split('func setupGlobalHotkeys(',1)[1].split('func keyRegisteredGlobally(',1)[0]
        self.assertNotIn('registerHotKey.Call(',block)
        self.assertIn('DISABLED_AFTER_WINE_NATIVE_CRASH_IN_R5_3',block)

if __name__=='__main__':unittest.main()
