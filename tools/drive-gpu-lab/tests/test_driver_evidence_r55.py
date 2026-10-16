import json
import os
from pathlib import Path
import tempfile

import pytest
from gpu_lab_evidence import analyze_session,candidate_identity, MAX_SOURCE_BYTES


def test_counts_without_claiming_real_driver_calls(tmp_path,monkeypatch):
    monkeypatch.delenv('GPU_LAB_DRIVER_PATH',raising=False)
    p=tmp_path/'Android/logs/WINE/game.log';p.parent.mkdir(parents=True)
    p.write_text('trace:vulkan:thunk32_vkQueueSubmit2KHR 0xabc\n'
                 'trace:vulkan:thunk32_vkQueuePresentKHR 0xabc\n'
                 'warn:panvk: VK_ERROR_DEVICE_LOST\n'
                 'Validation Error: VUID-vkQueueSubmit-pSubmits-00010\n',encoding='utf-8')
    r=analyze_session(tmp_path,'speed.exe-abc',[{'category':'WINE','archive_path':'Android/logs/WINE/game.log'}])
    c=r['counts']
    assert c['vulkan_function_mentions']['vkQueueSubmit2KHR']==1
    assert 'vkQueueSubmit' not in c['vulkan_function_mentions']
    assert c['vk_result_mentions']['VK_ERROR_DEVICE_LOST']==1
    assert c['validation_vuids']['VUID-vkQueueSubmit-pSubmits-00010']==1
    assert r['functions_are_log_mentions_not_verified_driver_calls'] is True
    assert r['driver_identity']['loaded_library_in_game']=='NOT_PROVEN'
    assert json.loads((tmp_path/'Android/DriverDiagnostics/DRIVER-EVIDENCE.json').read_text())['session']=='speed.exe-abc'


def test_missing_and_untrusted_path_dont_escape(tmp_path):
    r=analyze_session(tmp_path,'sid',[{'category':'WINE','archive_path':'../../etc/passwd'},
                                      {'category':'PANVK','archive_path':'Android/logs/PANVK/missing.log'}])
    assert {x['status'] for x in r['source_files']}=={'SKIPPED_UNTRUSTED_PATH','UNAVAILABLE'}


def test_input_size_bounded_and_partial(tmp_path,monkeypatch):
    import gpu_lab_evidence
    monkeypatch.setattr(gpu_lab_evidence,'MAX_SOURCE_BYTES',100)
    p=tmp_path/'Android/logs/WINE/w.log';p.parent.mkdir(parents=True)
    p.write_bytes(b'vkQueueSubmit\n'*150)
    r=analyze_session(tmp_path,'sid',[{'category':'WINE','archive_path':'Android/logs/WINE/w.log'}])
    assert r['source_files'][0]['status']=='PARTIAL_SCAN'
    assert r['source_files'][0]['scanned_bytes']==100
    assert r['counts']['vulkan_function_mentions']['vkQueueSubmit']>=1


def test_hash_is_only_candidate(tmp_path,monkeypatch):
    p=tmp_path/'libpanvk.so';p.write_bytes(b'ELF-test-candidate')
    monkeypatch.setenv('GPU_LAB_DRIVER_PATH',str(p))
    monkeypatch.delenv('GPU_LAB_NATIVE_PID',raising=False)
    r=candidate_identity()
    assert r['candidate_file_status']=='HASHED_CANDIDATE_ONLY'
    assert r['mapping_status']=='NOT_CHECKED_NO_NATIVE_PID'
    assert r['loaded_library_in_game']=='NOT_PROVEN'


def test_malformed_native_pid_is_nonfatal(tmp_path,monkeypatch):
    p=tmp_path/'panvk.so';p.write_bytes(b'candidate')
    monkeypatch.setenv('GPU_LAB_DRIVER_PATH',str(p))
    monkeypatch.setenv('GPU_LAB_NATIVE_PID','xyz')
    assert candidate_identity()['mapping_status']=='INVALID_PID'


def test_empty_source_is_not_full_trace(tmp_path):
    r=analyze_session(tmp_path,'sid',[])
    assert r['evidence_status']=='NO_ELIGIBLE_LOGS'
    assert r['driver_internal_submit_trace'].startswith('NOT_PROVEN')


def test_never_replace_existing_report(tmp_path):
    analyze_session(tmp_path,'sid',[])
    with pytest.raises(FileExistsError):analyze_session(tmp_path,'sid',[])
