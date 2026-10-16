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
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'payload_base'))
import gpu_lab_diagnostics as d
import gpu_lab_collector as c

class DiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name)
        self.root=self.base/'Download'/'GPU-Lab';self.docs=self.base/'Documents'/'Bannerlator';self.docs.mkdir(parents=True)
        self.sysfs=self.base/'sys'/'class'/'devfreq';self.sysfs.mkdir(parents=True)
        self.run='run-diag-test-12345678';self.token='12'*32
    def fakebin(self, name, data):
        b=self.base/'bin';b.mkdir(exist_ok=True)
        f=b/name;f.write_text('#!/bin/sh\n'+data+'\n');f.chmod(0o755)
    def session(self, sid='speed.exe-20261015-235835-p424-abc'):
        w=self.root/'Logs'/sid/'Windows';w.mkdir(parents=True,exist_ok=True)
        now=dt.datetime.now(dt.timezone.utc)
        c.atomic_json(w/'session.json',{'run':self.run,'token':self.token,'session':sid,'game':'speed.exe','state':'active','reason':'GAME_PROCESS_EXITED','started_at':now.isoformat(),'ended_at':now.isoformat()})
        (w/'telemetry').mkdir(exist_ok=True);(w/'telemetry'/'game.csv').write_text('pid,cpu,ram\n424,10,500')
        return w
    def prepare_env(self):
        self.fakebin('logcat',"echo '10-15 23:58:03.000  100  100 I PanVK: vkQueueSubmit error'; echo '10-15 23:58:04.000  100  100 I OtherApp: private unrelated'; sleep 1")
        self.fakebin('dmesg',"echo 'panthor: gpu reset due to timeout'")
        self.patch=mock.patch.dict(os.environ,{'PATH':str(self.base/'bin')+os.pathsep+os.environ.get('PATH',''),'GPU_LAB_DIAG_MODE':'standard'},clear=False)
        self.patch.start();self.addCleanup(self.patch.stop)
    def test_filter_standard(self):
        self.prepare_env()
        manager=d.DiagnosticManager(self.root,self.run,self.token,sysfs_root=self.sysfs)
        self.session();manager.tick([self.root/'Logs'/'speed.exe-20261015-235835-p424-abc'/'Windows'])
        time.sleep(.15)
        w=self.base/'work';w.mkdir()
        status=manager.export_session('speed.exe-20261015-235835-p424-abc',w)
        captured=(w/'Android'/'DriverDiagnostics'/'android-logcat.txt').read_text()
        self.assertIn('vkQueueSubmit',captured)
        self.assertNotIn('OtherApp',captured)
        self.assertEqual(status['kernel']['status'],'CAPTURED')
        self.assertIn('panthor',(w/'Android'/'DriverDiagnostics'/'kernel-dmesg.txt').read_text())
        self.assertTrue((w/'Android'/'DriverDiagnostics'/'COVERAGE.json').exists())
        manager.finish()
    def test_deep_full_logcat_and_no_root_unless_opt_in(self):
        self.prepare_env()
        with mock.patch.dict(os.environ, {'GPU_LAB_DIAG_MODE':'deep','GPU_LAB_DIAG_ROOT_ACCESS':'0'}):
            m=d.DiagnosticManager(self.root,self.run,self.token,sysfs_root=self.sysfs)
            self.session();m.tick([self.root/'Logs'/'speed.exe-20261015-235835-p424-abc'/'Windows']);time.sleep(.15)
            w=self.base/'work';w.mkdir();m.export_session('speed.exe-20261015-235835-p424-abc',w)
            self.assertIn('OtherApp',(w/'Android'/'DriverDiagnostics'/'android-logcat.txt').read_text())
            self.assertFalse(m.root_access)
    def test_sysfs_denied_and_missing_commands_are_nonfatal(self):
        with mock.patch.object(Path,'iterdir',side_effect=PermissionError('restricted')):
            self.assertEqual(d.read_sysfs(self.sysfs)['status'],'UNAVAILABLE')
        with mock.patch.object(subprocess,'Popen',side_effect=FileNotFoundError('missing')):
            m=d.DiagnosticManager(self.root,self.run,self.token,sysfs_root=self.sysfs)
            self.session();m.tick([self.root/'Logs'/'speed.exe-20261015-235835-p424-abc'/'Windows'])
            w=self.base/'work';w.mkdir();m.export_session('speed.exe-20261015-235835-p424-abc',w)
            coverage=json.loads((w/'Android'/'DriverDiagnostics'/'COVERAGE.json').read_text())
            self.assertEqual(coverage['logcat']['status'],'UNAVAILABLE')
            self.assertEqual(coverage['kernel']['status'],'UNAVAILABLE')
    def test_sha_does_not_claim_loaded_so(self):
        p=self.base/'test.so';p.write_bytes(b'driver')
        with mock.patch.dict(os.environ,{'GPU_LAB_DRIVER_PATH':str(p)}):
            result=d.hash_driver_configured()
            self.assertEqual(result['status'],'HASHED_FILE_ONLY')
            self.assertFalse(result['loaded_driver_verified'])
    def test_real_collector_archives_diag_files_and_return(self):
        self.prepare_env()
        # replace sysfs with a safe empty folder only in DiagnosticManager for this test
        with mock.patch.object(c,'DiagnosticManager',side_effect=lambda *a:d.DiagnosticManager(*a,sysfs_root=self.sysfs)):
            collector=c.Collector(self.root,self.run,self.token,self.docs.parent,grace=0)
            win=self.session();collector.tick()
            time.sleep(.2)
            meta=json.loads((win/'session.json').read_text());meta['state']='finished';meta['ended_at']=dt.datetime.now(dt.timezone.utc).isoformat()
            c.atomic_json(win/'session.json',meta)
            collector.tick();collector.tick()
            c.guarded_finalize(collector,'TEST')
            dest=self.root/'Logs'/(win.parent.name+'.zip')
            self.assertTrue(dest.is_file())
            self.assertTrue((self.root/'Logs'/win.parent.name/'Android'/'DriverDiagnostics'/'COVERAGE.json').exists())
            with zipfile.ZipFile(dest) as z:
                self.assertIsNone(z.testzip())
                self.assertIn('Android/DriverDiagnostics/COVERAGE.json',z.namelist())
                self.assertIn('Android/DriverDiagnostics/android-logcat.txt',z.namelist())
                for entry in z.read('META/SHA256SUMS.txt').decode().splitlines():
                    h,n=entry.split('  ',1)
                    import hashlib
                    self.assertEqual(h,hashlib.sha256(z.read(n)).hexdigest())
            self.assertEqual(len(list((self.base/'Download'/'Log Termux').glob('GPU-Lab-RETURN-*.zip'))),1)
    def test_logcat_limit_cannot_exceed_quota(self):
        self.prepare_env()
        self.fakebin('logcat', "i=0; while [ $i -lt 1500 ]; do echo 'panvk: vkQueueSubmit pending GPU'; i=$((i+1)); done")
        p=d.LogcatStream(self.base/'cap','standard',1024);p.start()
        time.sleep(.2)
        r=p.stop()
        self.assertLessEqual(r['bytes_captured'],1024)
        self.assertTrue(r['truncated'])

if __name__=='__main__':unittest.main(verbosity=2)
