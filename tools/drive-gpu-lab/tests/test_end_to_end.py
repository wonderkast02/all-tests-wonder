import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

COLLECTOR=str(Path(__file__).resolve().parents[1]/'android'/'gpu_lab_collector.py')

class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.base=Path(self.tmp.name)
        self.root=self.base/'Downloads'/'GPU-Lab'
        self.docs=self.base/'Documents'/'Winlator'/'trace';self.docs.mkdir(parents=True)
        self.run='runtest-9999999999999999';self.token='11'*32
        self.win=self.root/'Logs'/'_control'/self.run/'windows';self.win.mkdir(parents=True)
        self.cmd=[sys.executable,COLLECTOR,'--root',str(self.root),'--run',self.run,'--token',self.token,'--documents',str(self.base/'Documents'),'--grace','0']

    def until(self,test,timeout=15):
        until=time.time()+timeout
        while time.time()<until:
            if test():return
            time.sleep(0.05)
        self.fail('timeout waiting for host collector readiness')

    def make_session(self,name):
        d=self.root/'Logs'/name/'Windows';d.mkdir(parents=True)
        p={'schema':'gpu-lab-session-v1','run':self.run,'token':self.token,'session':name,'game':name.split('-')[0],'started_at':(dt.datetime.now(dt.timezone.utc)-dt.timedelta(seconds=2)).isoformat(),'ended_at':dt.datetime.now(dt.timezone.utc).isoformat(),'state':'finished','reason':'PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN'}
        (d/'session.json').write_text(json.dumps(p));(d/'game.log').write_text('pid=1234')

    def test_two_sessions_then_close(self):
        proc=subprocess.Popen(self.cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            self.until(lambda:(self.root/'Logs'/'_control'/self.run/'android'/'android-device.json').exists())
            (self.docs/'wine_debug.log').write_text('game1')
            self.make_session('one.exe-20261015-001001-p100-aaa')
            self.until(lambda:len(list((self.root/'Logs').glob('*.zip')))==1)
            self.until(lambda: (self.root/'Logs'/'_control'/self.run/'android'/'ARCHIVED-one.exe-20261015-001001-p100-aaa.json').exists())
            time.sleep(.2)
            (self.docs/'wine_debug.log').write_text('game2')
            (self.docs/'speed_d3d9.log').write_text('DXVK: v2.0')
            self.make_session('two.exe-20261015-001002-p101-bbb')
            self.until(lambda:len(list((self.root/'Logs').glob('*.zip')))==2)
            (self.win/'finished.ok').write_text(self.token+'\n')
            out,err=proc.communicate(timeout=10)
            self.assertEqual(proc.returncode,0,err)
            self.assertEqual(len(list((self.base/'Downloads'/'Log Termux').glob('GPU-Lab-RETURN-*.zip'))),1)
            self.assertIn('GPU_LAB_SESSIONS_ARCHIVED=2',out)
            with zipfile.ZipFile(next(x for x in (self.root/'Logs').glob('*.zip') if 'two' in x.name)) as z:
                meta=json.loads(z.read('META/COLLECTION-REPORT.json'))
                self.assertEqual(meta['categories_copied']['DXVK'],1)
                self.assertEqual(meta['categories_copied']['WINE'],1)
        finally:
            if proc.poll() is None:proc.kill();proc.communicate()

    def test_closed_gui_with_finished_session_not_marked_partial(self):
        proc=subprocess.Popen(self.cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            self.until(lambda:(self.root/'Logs'/'_control'/self.run/'android'/'android-device.json').exists())
            self.make_session('fast-stop.exe-20261015-001003-p102-ccc')
            (self.win/'finished.ok').write_text(self.token+'\n')
            out,err=proc.communicate(timeout=10)
            self.assertEqual(proc.returncode,0,err)
            z=list((self.root/'Logs').glob('*.zip'))[0]
            with zipfile.ZipFile(z) as f:r=json.loads(f.read('META/COLLECTION-REPORT.json'))
            self.assertNotEqual(r['collection_completeness'],'PARTIAL')
        finally:
            if proc.poll() is None:proc.kill();proc.communicate()

    def test_batch_crash_emits_diagnostic_zip(self):
        proc=subprocess.Popen(self.cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            self.until(lambda:(self.root/'Logs'/'_control'/self.run/'android'/'android-device.json').exists())
            (self.win/'bat-exit.txt').write_text('EXIT_CODE=2\n')
            out,err=proc.communicate(timeout=10)
            self.assertEqual(proc.returncode,0,err)
            self.assertEqual(len(list((self.root/'Logs').glob('NO-GAME-*.zip'))),1)
            self.assertEqual(len(list((self.base/'Downloads'/'Log Termux').glob('GPU-Lab-RETURN-*.zip'))),1)
        finally:
            if proc.poll() is None:proc.kill();proc.communicate()

    def test_no_game_has_diagnostic_zip(self):
        proc=subprocess.Popen(self.cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            self.until(lambda:(self.root/'Logs'/'_control'/self.run/'android'/'android-device.json').exists())
            (self.win/'finished.ok').write_text(self.token+'\n')
            out,err=proc.communicate(timeout=10)
            self.assertEqual(proc.returncode,0,err)
            zipfiles=list((self.root/'Logs').glob('NO-GAME-*.zip'))
            self.assertEqual(len(zipfiles),1)
            with zipfile.ZipFile(zipfiles[0]) as z:
                meta=json.loads(z.read('META/COLLECTION-REPORT.json'))
                self.assertEqual(meta['collection_completeness'],'PARTIAL')
        finally:
            if proc.poll() is None:proc.kill();proc.communicate()

if __name__=='__main__':unittest.main(verbosity=2)
