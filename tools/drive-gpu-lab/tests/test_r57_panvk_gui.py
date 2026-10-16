"""R5.7 regressions: full-stream evidence and conservative GUI recovery."""
from pathlib import Path
import json
import pytest
import gpu_lab_panvk as p


def write_wine(root, content):
    f=root/'Android/logs/WINE/wine_debug.log';f.parent.mkdir(parents=True,exist_ok=True)
    f.write_bytes(content)
    return [{'archive_path':f.relative_to(root).as_posix(),'category':'WINE'}]

def test_stream_beyond_previous_16m_limit(tmp_path):
    prefix=b'vulkan:thunk_vkQueueSubmit2\n' * 600000
    assert len(prefix)>16*1024*1024
    lines=prefix+b'[MESA-PANVK] kbase: SYNC-A3 native GPU internal wait path active (3 cells)\n'*32
    r=p.analyze_panvk(tmp_path,'game',write_wine(tmp_path,lines))
    assert r['counts']['sync_a3_native_wait_markers']==32
    assert r['counts']['panvk_mesa_lines']==32
    assert r['counts']['panvk_internal_marker_lines']==32
    assert r['counts']['internal_submit_marker_lines']==0
    assert r['driver_internal_submit_trace']=='NOT_PROVEN'
    assert r['loaded_panvk_library']=='NOT_PROVEN'
    assert r['source_scan_completeness']=='ALL_SELECTED_FILES_FULLY_SCANNED'
    assert r['sources'][0]['bytes_processed_approx']==len(lines)
    assert r['sources'][0]['unscanned_bytes']==0
    assert r['sources'][0]['attribution']=='SHARED_WINE_NOT_SESSION_ISOLATED'

def test_sync_a3_not_proof_fence_or_csf(tmp_path):
    entry=write_wine(tmp_path,b'[MESA-PANVK] kbase: SYNC-A3 native GPU internal wait path active (3 cells)\n')
    r=p.analyze_panvk(tmp_path,'s',entry)
    assert r['native_wait_implementation_marker'].startswith('TEXT_MARKER')
    assert r['synchronization_cause']=='NOT_PROVEN_FROM_TEXT_LOGS'
    assert r['kernel_csf_trace']=='NOT_PROVEN'
    assert r['panvk_debug_activation']=='NOT_PROVEN'

def test_shared_log_does_not_fake_provenance(tmp_path):
    r=p.analyze_panvk(tmp_path,'speed.exe-p424',write_wine(tmp_path,b'[MESA-PANVK] buffer\n'))
    assert 'NOT_SESSION_ISOLATED' in r['sources'][0]['attribution']
    assert 'not proof' in r['source_attribution_note']

def test_file_budget_exhaustion_reported(tmp_path,monkeypatch):
    monkeypatch.setattr(p,'LIMIT_PER_FILE',100)
    r=p.analyze_panvk(tmp_path,'s',write_wine(tmp_path,b'x\n'*100))
    f=r['sources'][0]
    assert f['unscanned_bytes']==100 and f['stop_reason']=='FILE_BYTES_LIMIT'
    assert r['source_scan_completeness']=='PARTIAL_LIMIT_REACHED'

def test_total_budget_skipped_files_reported(tmp_path,monkeypatch):
    monkeypatch.setattr(p,'LIMIT_TOTAL',5)
    f=tmp_path/'Android/logs/DXVK/a.log';f.parent.mkdir(parents=True);f.write_text('a'*10)
    w=tmp_path/'Android/logs/WINE/b.log';w.parent.mkdir(parents=True);w.write_text('b'*10)
    entries=[{'category':'DXVK','archive_path':f.relative_to(tmp_path).as_posix()}, {'category':'WINE','archive_path':w.relative_to(tmp_path).as_posix()}]
    r=p.analyze_panvk(tmp_path,'s',entries)
    assert r['unscanned_sources'] and r['source_scan_completeness']=='PARTIAL_LIMIT_REACHED'

def test_panvk_vendor_bracket_matches(tmp_path):
    r=p.analyze_panvk(tmp_path,'s',write_wine(tmp_path,b'[gralloc-PANVK] importing AHB\nPanVK is not conformant\n'))
    assert r['counts']['panvk_mesa_lines']==2

def test_large_unbroken_line_bounded(tmp_path):
    r=p.analyze_panvk(tmp_path,'s',write_wine(tmp_path,b'a'*(70000)+b'\n[MESA-PANVK] buffer\n'))
    assert r['counts']['panvk_mesa_lines']==1
    assert r['source_scan_completeness']=='ALL_SELECTED_FILES_FULLY_SCANNED'

def test_gui_recovers_without_affecting_worker_environment():
    s=(Path(__file__).resolve().parents[1]/'windows/main_windows.go').read_text()
    assert 'if role == "gui"' in s and 'recoverySafeMode' in s
    assert 'startChild(c, "worker", false)' in s
    assert 'startChild(c, "gui", failures > 0)' in s
    assert 'gui-last-exit.json' in s
    assert 'WINEDEBUG=-all,+seh' in s
    assert 'root_cause": "NOT_PROVEN"' in s

def test_gui_safe_mode_disables_only_optional_native_readback():
    s=(Path(__file__).resolve().parents[1]/'windows/gui_windows.go').read_text()
    assert 'UNVERIFIED_SAFE_MODE' in s
    assert 'if !guiSafeMode {' in s
    assert 'nativeText("header:status"' in s
    assert 'nativeText(pre+":value", val)' in s

def test_legacy_report_failure_does_not_break_archive():
    s=(Path(__file__).resolve().parents[1]/'android'/'gpu_lab_collector.py').read_text()
    assert 'PANVK_EVIDENCE_PARTIAL:' in s
    assert 'pack_immutable(work, self.archives, desired)' in s
