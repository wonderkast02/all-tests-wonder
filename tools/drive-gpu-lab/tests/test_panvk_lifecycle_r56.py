import datetime as dt
import json
from pathlib import Path
import tempfile
import pytest
from gpu_lab_collector import Collector
from gpu_lab_panvk import analyze_panvk

@pytest.fixture
def env(tmp_path):
    root=tmp_path/'Download'/'GPU-Lab';run='run-20261016-tested';token='ac'*32
    c=Collector(root,run,token,tmp_path/'Documents',[],0)
    yield c
    if c.diag:c.diag.finish()

def session(c,sid, *,token=None, run=None, state='finished', reason='PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN',age=45):
    folder=c.archives/sid/'Windows';folder.mkdir(parents=True,exist_ok=True)
    at=dt.datetime.now(dt.timezone.utc)-dt.timedelta(seconds=age)
    rec={'session':sid,'run':run or c.run,'token':token or c.token,
         'game':'speed.exe','state':state,'reason':reason,'ended_at':at.isoformat(),
         'started_at':(at-dt.timedelta(seconds=20)).isoformat()}
    (folder/'session.json').write_text(json.dumps(rec),encoding='utf-8')
    return rec

def test_host_stop_after_proven_archive(env):
    sid='speed.exe-20261016-010919-p440-unique'
    session(env,sid);env.seen[sid]={};env.archived.add(sid)
    r=env.completed_current_sessions()
    assert r['sessions']==[sid]
    env.request_worker_stop_after_game(r)
    assert env.token in (env.win/'stop.request').read_text()
    env.request_worker_stop_after_game(r) # duplicate is safe
    assert env.completed_current_sessions()

def test_host_stop_refused_for_running_game(env):
    sid='speed.exe-20261016-010919-p440-unique'
    session(env,sid,state='active')
    env.seen[sid]={};env.archived.add(sid)
    assert env.completed_current_sessions() is None
    assert not (env.win/'stop.request').exists()

def test_host_stop_refused_for_wrong_token(env):
    sid='speed.exe-20261016-010919-p440-unique'
    session(env,sid,token='bb'*32);env.seen[sid]={};env.archived.add(sid)
    assert env.completed_current_sessions() is None

def test_host_stop_refused_for_incomplete_archive(env):
    sid='speed.exe-20261016-010919-p440-unique'
    session(env,sid);env.seen[sid]={}
    assert env.completed_current_sessions() is None

def test_host_stop_refused_for_bad_exit_reason(env):
    sid='speed.exe-20261016-010919-p440-unique'
    session(env,sid,reason='TIMEOUT_WATCHDOG');env.seen[sid]={};env.archived.add(sid)
    assert env.completed_current_sessions() is None

def test_host_stop_refused_for_recent_exit(env):
    sid='speed.exe-20261016-010919-p440-unique'
    session(env,sid,age=3);env.seen[sid]={};env.archived.add(sid)
    assert env.completed_current_sessions() is None

def test_host_stop_refused_for_second_active(env):
    sid='speed.exe-20261016-010919-p440-unique';sid2='speed.exe-20261016-011900-p441-next'
    session(env,sid);session(env,sid2,state='active')
    env.seen[sid]={};env.seen[sid2]={};env.archived.add(sid)
    assert env.completed_current_sessions() is None

def test_host_stop_refused_for_unproven_end_time(env):
    sid='speed.exe-20261016-010919-p440-unique'
    rec=session(env,sid);rec.pop('ended_at');(env.archives/sid/'Windows/session.json').write_text(json.dumps(rec))
    env.seen[sid]={};env.archived.add(sid)
    assert env.completed_current_sessions() is None

def test_panvk_wine_thunks_not_internal_trace(tmp_path):
    p=tmp_path/'Android/logs/WINE/wine_debug.log';p.parent.mkdir(parents=True)
    p.write_text('trace:vulkan:thunk_vkQueueSubmit2 vkQueuePresentKHR\n[MESA-PANVK] fallback gralloc\n')
    r=analyze_panvk(tmp_path,'speed.exe-p424',[{'category':'WINE','archive_path':'Android/logs/WINE/wine_debug.log'}])
    assert r['status']=='PANVK_MESA_MESSAGES_ONLY'
    assert r['driver_internal_submit_trace']=='NOT_PROVEN'
    assert r['counts']['panvk_mesa_lines']==1
    assert r['counts']['panvk_internal_marker_lines']==0

def test_panvk_startup_flags_and_no_fake_csf(tmp_path):
    p=tmp_path/'Android/logs/PANVK/panvk.txt';p.parent.mkdir(parents=True)
    p.write_text('panvk_debug: startup,sync\nPANVK_INTERNAL_SYNC event submission=4\n')
    r=analyze_panvk(tmp_path,'s',[{'category':'PANVK','archive_path':'Android/logs/PANVK/panvk.txt'}])
    assert r['status']=='INTERNAL_MARKERS_IN_CAPTURED_LOGS'
    assert r['panvk_debug_activation'].startswith('ONLY_CONFIRMED')
    assert r['synchronization_cause']=='NOT_PROVEN_FROM_TEXT_LOGS'

def test_panvk_missing_file_no_crash(tmp_path):
    r=analyze_panvk(tmp_path,'s',[{'category':'PANVK','archive_path':'Android/logs/PANVK/absent.log'}])
    assert r['status']=='NO_PANVK_DRIVER_MESSAGES_CAPTURED'
    assert r['loaded_panvk_library']=='NOT_PROVEN'

def test_panvk_reject_path_escape(tmp_path):
    r=analyze_panvk(tmp_path,'s',[{'category':'PANVK','archive_path':'../../etc/passwd'}])
    assert r['sources']==[]

def test_panvk_duplicate_report_refused(tmp_path):
    analyze_panvk(tmp_path,'s',[])
    with pytest.raises(FileExistsError):analyze_panvk(tmp_path,'s',[])

def test_controller_contains_postgame_crash_suppression():
    main=(Path(__file__).resolve().parents[1]/'windows/main_windows.go').read_text()
    assert 'gameSessionAlreadyFinished(c)' in main
    assert 'postgame_gui_relaunch_suppressed' in main
    assert '23 * time.Second' in main
