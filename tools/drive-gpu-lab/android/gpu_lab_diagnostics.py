#!/usr/bin/env python3
"""GPU Lab diagnostics companion, isolated from native Win32 process.
Read-only; never modifies driver/game settings or stops game processes.
Android permissions are opportunistic; missing sources are explicitly reported.
"""
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import signal
import threading
import time

VERSION = '1.0.0'
FILTER = re.compile(r'gpu|vulkan|panvk|panthor|panfrost|mali|kbase|csf|drm|dxvk|vkd3d|box64|fex|wine|mesa|fatal|tombstone|crash|segfault|sigsegv|hang|timeout|fence|syncobj|device.lost|vkqueuesubmit|vkqueuepresent', re.I)
SYSFS_NAMES = ('cur_freq','min_freq','max_freq','available_frequencies','governor','load','utilization','busy_time','total_time','time_in_state')
MAX_EXPLICIT_DRIVER = 128*1024*1024


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec='milliseconds')


def write_json_x(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8') as fp:
        json.dump(obj, fp, ensure_ascii=False, indent=2, sort_keys=True)
        fp.write('\n')
        fp.flush()
        os.fsync(fp.fileno())


def run_bounded(argv, seconds=4, max_bytes=1024*1024):
    """Run fixed argv with bounded file-backed stdout/stderr, wall-clock timeout."""
    try:
        with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
            try:
                proc=subprocess.Popen(argv, stdout=stdout, stderr=stderr, start_new_session=True)
            except (OSError,ValueError) as exc:
                return {'status':'UNAVAILABLE','reason':type(exc).__name__,'output':b'', 'stderr':b'', 'exit_code':None}
            timed_out=False
            try:proc.wait(timeout=seconds)
            except subprocess.TimeoutExpired:
                timed_out=True
                try:os.killpg(proc.pid,signal.SIGKILL)
                except (OSError,AttributeError):
                    try:proc.kill()
                    except OSError:pass
                try:proc.wait(timeout=2)
                except subprocess.TimeoutExpired:pass
            stdout.seek(0);data=stdout.read(max_bytes+1)
            stderr.seek(0);err=stderr.read(4096)
            truncated=len(data)>max_bytes
            data=data[:max_bytes]
            status='TIMEOUT' if timed_out else ('CAPTURED' if proc.returncode==0 else 'UNAVAILABLE')
            return {'status':status,'reason':'COMMAND_TIMEOUT' if timed_out else ('OK' if status=='CAPTURED' else 'COMMAND_FAILED'),
                    'output':data,'stderr':err,'exit_code':proc.returncode,'truncated':truncated}
    except OSError as exc:
        return {'status':'UNAVAILABLE','reason':type(exc).__name__,'output':b'', 'stderr':b'', 'exit_code':None}


def read_sysfs(root=Path('/sys/class/devfreq')):
    out = {'status':'UNAVAILABLE', 'devices':{}, 'errors':[]}
    try:
        paths=list(root.iterdir())[:96]
    except (OSError, PermissionError) as exc:
        out['errors'].append(type(exc).__name__)
        return out
    count=0
    for path in paths:
        if not re.search(r'mali|gpu|panfrost|panthor|3d', path.name, re.I):
            continue
        if count >= 12:break
        count+=1
        values={}
        for filename in SYSFS_NAMES:
            try:
                raw=(path/filename).read_bytes()[:8192]
                values[filename]=raw.decode('utf-8','replace').strip()
            except (OSError, PermissionError):
                values[filename]='UNAVAILABLE'
        out['devices'][path.name]=values
    out['status']='CAPTURED' if out['devices'] else 'UNAVAILABLE'
    if not out['devices']:out['errors'].append('NO_MATCHING_GPU_DEVFREQ_ENTRIES')
    return out


def hash_driver_configured():
    """Only a specific path supplied by the tester can establish driver hash."""
    supplied=os.environ.get('GPU_LAB_DRIVER_PATH', '').strip()
    if not supplied:
        return {'status':'NOT_CONFIGURED','identity':'NOT_PROVEN','reason':'GPU_LAB_DRIVER_PATH_NOT_SET'}
    p=Path(supplied)
    try:
        if not p.is_file() or p.is_symlink():return {'status':'UNAVAILABLE','reason':'NOT_REGULAR_FILE'}
        size=p.stat().st_size
        if size>MAX_EXPLICIT_DRIVER:return {'status':'SKIPPED','reason':'FILE_LIMIT_128MB','bytes':size}
        h=hashlib.sha256()
        with p.open('rb') as f:
            for part in iter(lambda:f.read(1024*1024),b''):h.update(part)
        return {'status':'HASHED_FILE_ONLY','path':str(p),'bytes':size,'sha256':h.hexdigest(),
                'loaded_driver_verified':False,'reason':'A_PATH_HASH_CANNOT_PROVE_RUNTIME_LOADED_LIBRARY'}
    except OSError as exc:
        return {'status':'UNAVAILABLE','reason':type(exc).__name__}


class LogcatStream:
    def __init__(self, folder, mode, limit):
        self.folder=Path(folder)
        self.mode=mode
        self.limit=limit
        self.bytes=0
        self.dropped=0
        self.total_lines=0
        self.status='NOT_STARTED'
        self.error=''
        self.proc=None
        self.thread=None
        self.path=self.folder/'android-logcat.txt'
        self.stderr_path=self.folder/'logcat-command-stderr.txt'
        self._stop=threading.Event()

    def start(self):
        self.folder.mkdir(parents=True, exist_ok=True)
        try:
            err=self.stderr_path.open('xb')
            try:
                self.proc=subprocess.Popen(['logcat','-v','threadtime','-b','main','-b','system','-b','crash','-T','1'],
                    stdout=subprocess.PIPE,stderr=err,bufsize=0,start_new_session=True)
            finally:err.close()
        except (OSError, ValueError) as exc:
            self.status='UNAVAILABLE'
            self.error=type(exc).__name__
            return
        self.status='RUNNING'
        self.thread=threading.Thread(target=self._drain,daemon=True,name='gpu-lab-logcat')
        self.thread.start()

    def _drain(self):
        try:
            with self.path.open('xb') as out:
                # read in a separate thread; no work on UI/monitor process
                for raw in self.proc.stdout:
                    self.total_lines+=1
                    if self.mode != 'deep' and not FILTER.search(raw.decode('utf-8','replace')):continue
                    if self.bytes + len(raw) > self.limit:
                        self.dropped+=1
                        continue
                    out.write(raw)
                    self.bytes+=len(raw)
                out.flush()
        except (OSError, ValueError) as exc:
            self.error=type(exc).__name__
            self.status='PARTIAL'
        finally:
            try:self.proc.stdout.close()
            except (OSError,AttributeError):pass

    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                try:self.proc.terminate()
                except OSError:pass
            if self.thread is not None:self.thread.join(timeout=2)
            if self.thread is not None and self.thread.is_alive():
                try:self.proc.kill()
                except OSError:pass
                self.thread.join(timeout=2)
            try:self.proc.wait(timeout=2)
            except (OSError,subprocess.TimeoutExpired):
                try:self.proc.kill()
                except OSError:pass
            if self.status=='RUNNING':
                self.status='CAPTURED' if self.bytes else 'EMPTY_OR_UNAVAILABLE'
        return {'status':self.status,'bytes_captured':self.bytes,'lines_seen':self.total_lines,
                'lines_omitted_by_size':self.dropped, 'truncated':self.dropped>0,
                'exit_code':self.proc.returncode if self.proc else None,'error':self.error,
                'mode':self.mode, 'note':'LOGCAT_ACCESS_AND_ANDROID_BUFFERS_ARE_NOT_UNIVERSAL'}


class DiagnosticManager:
    def __init__(self, root, run, token, sysfs_root=None):
        self.root=Path(root)
        self.run=run
        self.token=token
        mode=os.getenv('GPU_LAB_DIAG_MODE','standard').strip().lower()
        self.mode=mode if mode in ('standard','deep') else 'standard'
        self.root_access=os.getenv('GPU_LAB_DIAG_ROOT_ACCESS','0')=='1' and self.mode=='deep'
        self.sysfs_root=Path(sysfs_root) if sysfs_root else Path('/sys/class/devfreq')
        self.host=self.root/'Logs'/'_control'/run/'android'
        self.host.mkdir(parents=True, exist_ok=True)
        self.live={}
        self.completed=set()
        self.errors=[]
        self.limit=(16 if self.mode=='deep' else 4)*1024*1024
        self.last_tick=0
        write_json_x(self.host/'diag-capabilities.json',{
            'schema':'gpu-lab-diag-v1', 'version':VERSION,'mode':self.mode,
            'root_access_opt_in':self.root_access,
            'game_process_mutation':False, 'driver_setting_mutation':False,
            'all_log_sources_guaranteed':False,'captured_at':utc(),
            'driver':hash_driver_configured(),
            'sources':{'logcat':'LIVE_WHEN_AVAILABLE','kernel':'READ_ONLY_SNAPSHOT',
                       'vulkan_trace':'EXTERNAL_LAYER_REQUIRED_NOT_AUTOMATIC',
                       'panvk':'EXTERNAL_MESA_INSTRUMENTATION_REQUIRED_NOT_AUTOMATIC',
                       'box64_fex':'EXTERNAL_LOGGING_REQUIRED_NOT_AUTOMATIC',
                       'fps':'UNAVAILABLE_WITHOUT_FRAME_TIMESTAMPS'},
            'privacy':'DIAGNOSTIC_FILES_MAY_CONTAIN_PRIVATE_DATA_REVIEW_BEFORE_SHARING'})

    def session_open(self, sid):
        if sid in self.live or sid in self.completed:return
        sessiondir=self.host/'diag-sessions'/sid
        sessiondir.mkdir(parents=True,exist_ok=False)
        start={'schema':'gpu-lab-diag-session-v1','session':sid,'run':self.run,'started_at':utc(),
               'mode':self.mode,'captured_at':[],'errors':[]}
        write_json_x(sessiondir/'start-sysfs.json',read_sysfs(self.sysfs_root))
        write_json_x(sessiondir/'start-state.json',start)
        stream=LogcatStream(sessiondir,self.mode,self.limit)
        stream.start()
        self.live[sid]={'dir':sessiondir,'logcat':stream,'started':time.monotonic()}

    def tick(self,sessions):
        # Called by host-side collector. Its failure must never affect game monitor.
        for directory in sessions:
            p=directory/'session.json'
            try: meta=json.loads(p.read_text())
            except (OSError,ValueError):continue
            if meta.get('token')!=self.token or meta.get('run')!=self.run:continue
            sid=directory.parent.name
            if sid not in self.live and sid not in self.completed:
                try:self.session_open(sid)
                except Exception as exc:self.errors.append({'session':sid,'error':type(exc).__name__})

    def export_session(self,sid,work):
        dest=Path(work)/'Android'/'DriverDiagnostics'
        dest.mkdir(parents=True,exist_ok=True)
        if sid not in self.live:
            try:self.session_open(sid)
            except Exception as exc:self.errors.append({'session':sid,'error':type(exc).__name__})
        status={'schema':'gpu-lab-diag-coverage-v1','session':sid,'mode':self.mode,
                'generated_at':utc(),'logcat':{'status':'UNAVAILABLE'}, 'kernel':{'status':'NOT_REQUESTED'},
                'sysfs':{'status':'UNAVAILABLE'}, 'driver_loaded_hash':'NOT_PROVEN',
                'vulkan_api_trace':'NOT_INSTALLED_OR_NOT_INSTRUMENTED',
                'panvk_submit_trace':'NOT_INSTRUMENTED',
                'manual_deep_required':True,
                'coverage_not_equivalent_to_driver_completeness':True}
        live=self.live.pop(sid,None)
        if live:
            d=live['dir']
            try:status['logcat']=live['logcat'].stop()
            except Exception as exc:status['logcat']={'status':'FAILED','reason':type(exc).__name__}
            try:write_json_x(d/'end-sysfs.json',read_sysfs(self.sysfs_root))
            except Exception as exc:self.errors.append({'session':sid,'error':type(exc).__name__})
            for p in sorted(d.iterdir()):
                if p.is_file() and not p.is_symlink():
                    shutil.copyfile(p,dest/p.name)
            if (dest/'end-sysfs.json').exists():
                try:status['sysfs']=json.loads((dest/'end-sysfs.json').read_text())
                except (OSError,ValueError):pass
        # Kernel log: non-root attempt; optionally root only with explicit DEEP opt-in.
        # Do not inject debug layers, change setprop, WINEDEBUG, driver or kernel flags.
        args=['su','-c','dmesg'] if self.root_access else ['dmesg']
        snapshot=run_bounded(args, seconds=4, max_bytes=2*1024*1024)
        status['kernel']={k:v for k,v in snapshot.items() if k not in ('output','stderr')}
        if snapshot['output']:
            with (dest/'kernel-dmesg.txt').open('xb') as f:f.write(snapshot['output'])
        if snapshot['stderr']:
            with (dest/'kernel-dmesg-stderr.txt').open('xb') as f:f.write(snapshot['stderr'])
        write_json_x(dest/'COVERAGE.json',status)
        self.completed.add(sid)
        return status

    def finish(self):
        for sid,live in list(self.live.items()):
            try:live['logcat'].stop()
            except Exception:self.errors.append({'session':sid,'error':'LOGCAT_STOP_FAILED'})
        self.live.clear()
        path=self.host/'diag-final-status.json'
        if not path.exists():
            write_json_x(path,{'schema':'gpu-lab-diag-final-v1','finished_at':utc(),
                               'mode':self.mode,'sessions':sorted(self.completed),'errors':self.errors})
