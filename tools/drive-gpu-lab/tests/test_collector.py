import datetime as dt
import hashlib
import errno
from unittest import mock
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('collector',str(Path(__file__).resolve().parents[1]/'android'/'gpu_lab_collector.py'))
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class Tests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name)
        self.kit=self.base/'GPU-Lab'
        self.docs=self.base/'Documents'
        self.logs=self.docs/'Winlator'/'trace'
        self.logs.mkdir(parents=True)
        self.run='testrun-000000000000001';self.token='ab'*32
        self.c=m.Collector(self.kit,self.run,self.token,self.docs,grace=0)

    def session(self,sid,game='game.exe',done=True):
        d=self.kit/'Logs'/sid/'Windows'
        d.mkdir(parents=True)
        now=m.utc()
        payload={'schema':'gpu-lab-session-v1','run':self.run,'token':self.token,'session':sid,'game':game,'state':'finished' if done else 'active','started_at':now,'ended_at':now}
        m.atomic_json(d/'session.json',payload)
        (d/'telemetry').mkdir();(d/'telemetry'/'game.csv').write_text('time,cpu\n1,5\n')
        return d

    def test_capture_category_and_hash(self):
        f=self.logs/'dxvk_game_d3d9.log';f.write_text('DXVK: v3.1.1')
        current,_=m.scan_sources(self.c.roots)
        picks,excluded=m.select_sources(current,self.c.baseline,0,10**11)
        work=self.base/'stage'
        found,errs,total=m.capture_sources(work,picks,excluded)
        self.assertEqual(len(found),1);self.assertFalse(errs)
        self.assertEqual(found[0]['category'],'DXVK')
        self.assertEqual(found[0]['sha256'],hashlib.sha256(f.read_bytes()).hexdigest())
        self.assertEqual(total,len(f.read_bytes()))

    def test_stale_file_excluded(self):
        f=self.logs/'box64-old.log';f.write_text('previous session')
        os.utime(f,(5,5)); current,_=m.scan_sources(self.c.roots)
        a,_=m.select_sources(current,self.c.baseline,100,200)
        self.assertEqual(a,[])

    def test_per_session_unique_zip(self):
        self.session('game.exe-20261015-001001-p100-aaabbb');self.c.tick();self.c.tick()
        self.session('game.exe-20261015-001002-p101-aaabbb');self.c.tick();self.c.tick()
        arches=list(self.c.archives.glob('*.zip'))
        self.assertEqual(len(arches),2)
        self.assertEqual(len(set(p.name for p in arches)),2)
        for p in arches:self.assertTrue(m.verified_zip(p,{}))

    def test_repeated_tick_idempotent(self):
        self.session('game.exe-20261015-001001-p100-aaabbb');[self.c.tick() for _ in range(4)]
        self.assertEqual(len(list(self.c.archives.glob('*.zip'))),1)

    def test_collision_no_overwrite(self):
        stage=self.base/'input';stage.mkdir();(stage/'file.log').write_text('new')
        dest=self.base/'archive';dest.mkdir()
        old=dest/'same.zip';old.write_bytes(b'PREVIOUS ARCHIVE!')
        with self.assertRaises((ValueError,zipfile.BadZipFile)):
            m.pack_immutable(stage,dest,'same.zip')
        self.assertEqual(old.read_bytes(),b'PREVIOUS ARCHIVE!')

    def test_android_sdcard_no_hardlinks_fallback(self):
        stage=self.base/'input';stage.mkdir();(stage/'source.log').write_text('evidence')
        destination=self.base/'Log Termux';destination.mkdir()
        with mock.patch.object(m.os,'link',side_effect=OSError(errno.EOPNOTSUPP,'Android FUSE no hardlinks')):
            published,sha,count=m.pack_immutable(stage,destination,'unique.zip')
        self.assertEqual(published.name,'unique.zip')
        self.assertEqual(sha,hashlib.sha256(published.read_bytes()).hexdigest())
        self.assertTrue(m.verified_zip(published,{'source.log':hashlib.sha256(b'evidence').hexdigest()}))

    def test_android_sdcard_fallback_refuses_overwrite(self):
        stage=self.base/'input';stage.mkdir();(stage/'source.log').write_text('new')
        destination=self.base/'Log Termux';destination.mkdir()
        old=destination/'unique.zip';old.write_text('previous')
        with mock.patch.object(m.os,'link',side_effect=OSError(errno.EOPNOTSUPP,'FUSE')):
            with self.assertRaises((ValueError,zipfile.BadZipFile)):
                m.pack_immutable(stage,destination,'unique.zip')
        self.assertEqual(old.read_text(),'previous')

    def test_corrupt_archive_rejected(self):
        stage=self.base/'input';stage.mkdir();(stage/'a.log').write_text('a')
        z,_,_=m.pack_immutable(stage,self.base/'archive','x.zip')
        data=bytearray(z.read_bytes());data[len(data)//2]^=0x02;z.write_bytes(data)
        with self.assertRaises((zipfile.BadZipFile,ValueError,OSError)):m.verified_zip(z,{})

    def test_missing_game_archive_only_when_session(self):
        self.c.tick();self.assertFalse(list(self.c.archives.glob('*.zip')))

    def test_wrong_run_token_excluded(self):
        p=self.session('game.exe-20261015-001003-p102-aaabbb');data=json.loads((p/'session.json').read_text());data['token']='ff'*32;m.atomic_json(p/'session.json',data)
        self.c.tick();self.assertFalse(list(self.c.archives.glob('*.zip')))

    def test_partial_archive_if_interrupted(self):
        self.session('game.exe-20261015-001004-p103-aaabbb',done=False);self.c.finalize()
        p=list(self.c.archives.glob('*.zip'))[0]
        with zipfile.ZipFile(p) as z:
            meta=json.loads(z.read('META/COLLECTION-REPORT.json'))
        self.assertEqual(meta['collection_completeness'],'PARTIAL')
        self.assertTrue(any('NOT_FINISHED' in w for w in meta['warnings']))

    def test_android_and_windows_logs_in_one_zip(self):
        f=self.logs/'wine_debug.log';f.write_bytes(b'WINE'*200)
        self.session('game.exe-20261015-001001-p100-aaabbb');self.c.tick();self.c.tick()
        z=list(self.c.archives.glob('*.zip'))[0]
        with zipfile.ZipFile(z) as arc:
            names=arc.namelist()
            self.assertTrue(any(x.startswith('Windows/telemetry/') for x in names))
            self.assertTrue(any(x.startswith('Android/logs/WINE/') for x in names))
            self.assertIn('META/FILE-INVENTORY.json',names)
            self.assertIn('META/SHA256SUMS.txt',names)
            self.assertEqual(len(names),len(set(names)))
            for line in arc.read('META/SHA256SUMS.txt').decode().splitlines():
                digest,path=line.split('  ',1)
                self.assertEqual(hashlib.sha256(arc.read(path)).hexdigest(),digest)

    def test_no_log_does_not_claim_universal(self):
        self.session('game.exe-20261015-001005-p104-aaabbb');self.c.tick();self.c.tick()
        z=list(self.c.archives.glob('*.zip'))[0]
        with zipfile.ZipFile(z) as f:r=json.loads(f.read('META/COLLECTION-REPORT.json'))
        self.assertTrue(r['external_logs_are_not_universal'])
        self.assertEqual(r['game_exit_cause'],'NOT_PROVEN')

    def test_source_symlink_not_followed(self):
        secret=self.base/'secret.txt';secret.write_text('secret')
        (self.logs/'bad.log').symlink_to(secret)
        current,_=m.scan_sources(self.c.roots)
        self.assertNotIn(str(self.logs/'bad.log'),current)

class TermuxRegressions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)

    def test_termux_without_os_link(self):
        work = self.base / "work"; work.mkdir()
        (work / "actual.log").write_bytes(b"critical evidence")
        out = self.base / "Log Termux"
        # Directly simulate the Python build shown in the user's traceback.
        with mock.patch.object(m.os, "link", None):
            published, sha, count = m.pack_immutable(work, out, "RETURN.zip")
        self.assertTrue(published.is_file())
        self.assertEqual(hashlib.sha256(published.read_bytes()).hexdigest(), sha)
        with zipfile.ZipFile(published) as z:
            self.assertEqual(z.read("actual.log"), b"critical evidence")
            self.assertIsNone(z.testzip())

    def test_termux_devfreq_permission_denied(self):
        original = Path.iterdir
        def denied(path):
            if str(path) == "/sys/class/devfreq":
                raise PermissionError(errno.EACCES, "Permission denied", str(path))
            return original(path)
        with mock.patch.object(Path, "iterdir", denied):
            metrics, _ = m.host_metrics()
        self.assertEqual(metrics["gpu_freq"], "N/D")
        self.assertIn("ram_host", metrics)

    def test_termux_both_errors_no_game_diagnostic_return(self):
        kit=self.base/"GPU-Lab"; docs=self.base/"Documents"; docs.mkdir()
        collector=m.Collector(kit,"test-77777777777777777","aa"*32,docs,grace=0)
        original=Path.iterdir
        def denied(path):
            if str(path) == "/sys/class/devfreq":
                raise PermissionError(errno.EACCES,"Permission denied",str(path))
            return original(path)
        with mock.patch.object(Path,"iterdir",denied), mock.patch.object(m.os,"link",None):
            collector.tick()
            m.guarded_finalize(collector,"TEST_TERMUX_COMPAT")
        archived=list((kit/"Logs").glob("NO-GAME-*.zip"))
        self.assertEqual(len(archived),1)
        self.assertEqual(len(list((self.base/"Log Termux").glob("GPU-Lab-RETURN-*.zip"))),1)
        with zipfile.ZipFile(archived[0]) as z:
            self.assertIn("META/COLLECTION-REPORT.json",z.namelist())
            self.assertIn("Android/host-live.json",z.namelist())
            self.assertIsNone(z.testzip())

    def test_last_resort_archive_when_normal_archiver_broken(self):
        kit=self.base/"GPU-Lab"; docs=self.base/"Documents"; docs.mkdir()
        collector=m.Collector(kit,"test-88888888888888888","aa"*32,docs,grace=0)
        (collector.host/"probe-error.log").write_text("EVENTUAL FAILURE")
        with mock.patch.object(m,"pack_immutable",side_effect=RuntimeError("archive failure")):
            with self.assertRaises(RuntimeError):
                m.guarded_finalize(collector,"TEST_FORCED_FAILURE")
        archives=list(collector.returns.glob("GPU-Lab-RETURN-*.zip"))
        self.assertEqual(len(archives),1)
        with zipfile.ZipFile(archives[0]) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(z.read("Android/probe-error.log"),b"EVENTUAL FAILURE")
            status=json.loads(z.read("META/RETURN-INDEX.json"))
            self.assertIn("FINALIZE_FAILED",status["trigger"])

    def test_missing_os_link_collision_preserves_prior_zip(self):
        work = self.base / "work"; work.mkdir()
        (work / "actual.log").write_bytes(b"this is new")
        out=self.base/"Log Termux";out.mkdir()
        old=out/"RETURN.zip";old.write_bytes(b"OLD UNIQUE BYTES")
        with mock.patch.object(m.os,"link",None):
            with self.assertRaises((ValueError,zipfile.BadZipFile)):
                m.pack_immutable(work,out,"RETURN.zip")
        self.assertEqual(old.read_bytes(),b"OLD UNIQUE BYTES")

if __name__=='__main__':unittest.main(verbosity=2)
