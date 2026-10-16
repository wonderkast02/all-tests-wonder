#!/usr/bin/env python3
"""GPU Lab driver evidence report. Post-capture, bounded and read-only.
No tracing hooks, injection, process control or guessed Vulkan outcomes.
"""
import collections
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re

SCHEMA='gpu-lab-driver-evidence-v1'
MAX_SOURCE_BYTES=48*1024*1024
MAX_TOTAL_BYTES=96*1024*1024
MAX_LINE_BYTES=65536
MAX_SAMPLES=6
MAX_LIBRARY_BYTES=256*1024*1024
FUNCTIONS=('vkQueueSubmit2KHR','vkQueueSubmit2','vkQueueSubmit','vkQueuePresentKHR','vkAcquireNextImage2KHR','vkAcquireNextImageKHR','vkWaitForFences','vkGetFenceStatus','vkResetFences','vkWaitSemaphoresKHR','vkWaitSemaphores','vkSignalSemaphoreKHR','vkSignalSemaphore','vkCreateSwapchainKHR','vkDestroySwapchainKHR','vkDeviceWaitIdle','vkQueueWaitIdle')
FUNC_RE=re.compile(r'(?<![A-Za-z0-9])('+'|'.join(re.escape(n) for n in sorted(FUNCTIONS,key=len,reverse=True))+r')(?![A-Za-z0-9_])')
ERRORS=('VK_ERROR_DEVICE_LOST','VK_ERROR_OUT_OF_DATE_KHR','VK_ERROR_SURFACE_LOST_KHR','VK_TIMEOUT','VK_NOT_READY','VK_SUBOPTIMAL_KHR','VK_ERROR_INITIALIZATION_FAILED','VK_ERROR_OUT_OF_HOST_MEMORY','VK_ERROR_OUT_OF_DEVICE_MEMORY')
ERROR_RE=re.compile(r'(?<![A-Z0-9_])('+'|'.join(ERRORS)+r')(?![A-Z0-9_])')
VUID_RE=re.compile(r'\bVUID-[A-Za-z0-9_-]{5,110}\b')
DRIVER_RE=re.compile(r'panvk|panthor|panfrost|gralloc-PANVK|\[MESA-PANVK\]|kbase|mali|csf',re.I)
WARN_RE=re.compile(r'\b(?:error|failed|fatal|panic|device lost|hang|timeout|invalid|cannot create texture)\b',re.I)
PRIVACY='Log previews may include paths/IDs. Review before sharing.'


def sha_file(path):
    h=hashlib.sha256()
    with open(path,'rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()


def candidate_identity():
    """Configured file hash; process mapping proof only for explicit Android-native PID.
    Even mapping proof doesn't establish the ICD selected by a Windows game.
    """
    source=os.environ.get('GPU_LAB_DRIVER_PATH','').strip()
    pid_text=os.environ.get('GPU_LAB_NATIVE_PID','').strip()
    result={'candidate_path_configured':bool(source),'native_pid_configured':bool(pid_text),
            'candidate_file_status':'NOT_CONFIGURED', 'loaded_library_in_game':'NOT_PROVEN',
            'mapping_status':'NOT_CHECKED', 'identity_limit':'HASH_ALONE_CANNOT_PROVE_THE_DRIVER_USED_BY_THE_GAME'}
    if not source:
        return result
    p=Path(source)
    try:
        if p.is_symlink() or not p.is_file():
            result['candidate_file_status']='INVALID_OR_SYMLINK'
            return result
        st=p.stat()
        if st.st_size>MAX_LIBRARY_BYTES:
            result.update(candidate_file_status='SKIPPED_SIZE_LIMIT',candidate_bytes=st.st_size)
            return result
        result.update(candidate_file_status='HASHED_CANDIDATE_ONLY',candidate_path=str(p),
                      candidate_sha256=sha_file(p),candidate_bytes=st.st_size)
    except OSError as exc:
        result.update(candidate_file_status='UNAVAILABLE',candidate_error=type(exc).__name__)
        return result
    if not pid_text:
        result['mapping_status']='NOT_CHECKED_NO_NATIVE_PID'
        return result
    if not pid_text.isdecimal() or int(pid_text)<1 or int(pid_text)>2**31-1:
        result['mapping_status']='INVALID_PID'
        return result
    pid=int(pid_text)
    result['native_pid']=pid
    maps=Path('/proc')/str(pid)/'maps'
    try:
        with maps.open('rb') as f:
            data=f.read(4*1024*1024+1)
    except OSError as exc:
        result.update(mapping_status='UNAVAILABLE',mapping_error=type(exc).__name__)
        return result
    if len(data)>4*1024*1024:
        result['mapping_status']='INCOMPLETE_MAPS_SIZE_LIMIT'
        return result
    wanted_dev=(os.major(st.st_dev),os.minor(st.st_dev))
    for line in data.decode('utf-8','replace').splitlines():
        fields=line.split(None,5)
        if len(fields)<6:continue
        try:
            device=tuple(int(x,16) for x in fields[3].split(':'))
            inode=int(fields[4])
        except (ValueError,IndexError):continue
        if device==wanted_dev and inode==st.st_ino and fields[5].removesuffix(' (deleted)')==str(p):
            result['mapping_status']='FILE_INODE_MAPPED_IN_SPECIFIED_NATIVE_PROCESS'
            result['mapped_in_native_pid']=True
            result['loaded_library_in_game']='NOT_PROVEN'
            return result
    result['mapping_status']='FILE_INODE_NOT_IDENTIFIED_IN_SPECIFIED_PROCESS_MAPS'
    return result


def scan_log(path, cap, counters, samples):
    """Bounded byte streaming with correct partial/huge-line classification."""
    scanned=0
    truncated=False
    line_buf=b''
    with Path(path).open('rb') as f:
        while scanned<cap:
            block=f.read(min(65536,cap-scanned))
            if not block:break
            scanned+=len(block)
            content=line_buf+block
            pieces=content.split(b'\n')
            line_buf=pieces.pop()
            for raw in pieces:
                analyze_line(raw,counters,samples)
            if len(line_buf)>MAX_LINE_BYTES:
                analyze_line(line_buf[:MAX_LINE_BYTES],counters,samples)
                counters['partial_oversize_lines']+=1
                line_buf=b''
        if line_buf:analyze_line(line_buf[:MAX_LINE_BYTES],counters,samples)
        if f.read(1):truncated=True
    return scanned,truncated


def analyze_line(raw,counters,samples):
    s=raw.decode('utf-8','replace')
    counters['lines_examined']+=1
    for m in FUNC_RE.finditer(VUID_RE.sub('',s)):counters['vulkan_function_mentions'][m.group(1)]+=1
    for m in ERROR_RE.finditer(s):counters['vk_result_mentions'][m.group(1)]+=1
    vuids=VUID_RE.findall(s)
    for v in vuids:counters['validation_vuids'][v]+=1
    if DRIVER_RE.search(s):
        counters['driver_related_lines']+=1
        if len(samples['driver'])<MAX_SAMPLES:samples['driver'].append(s[:220].rstrip())
    if WARN_RE.search(s):
        counters['warning_error_lines']+=1
        if len(samples['warnings'])<MAX_SAMPLES:samples['warnings'].append(s[:220].rstrip())
    if vuids and len(samples['validation'])<MAX_SAMPLES:samples['validation'].append(s[:220].rstrip())


def analyze_session(work, sid, file_entries=None):
    work=Path(work)
    path=work/'Android'/'DriverDiagnostics'/'DRIVER-EVIDENCE.json'
    path.parent.mkdir(parents=True,exist_ok=True)
    totals={'lines_examined':0,'partial_oversize_lines':0,'driver_related_lines':0,
            'warning_error_lines':0, 'vulkan_function_mentions':collections.Counter(),
            'vk_result_mentions':collections.Counter(),'validation_vuids':collections.Counter()}
    samples={'driver':[],'warnings':[],'validation':[]}
    files=[]
    remaining=MAX_TOTAL_BYTES
    for obj in file_entries or []:
        if obj.get('category') not in ('WINE','DXVK','VULKAN','VKD3D','PANVK','MESA','BOX64','FEX'):continue
        relative=obj.get('archive_path','')
        source=work/relative
        try:
            # Defense against traversal in manifest entries, never follow an arbitrary symlink.
            if not source.resolve().is_relative_to(work.resolve()) or source.is_symlink():
                files.append({'path':relative,'status':'SKIPPED_UNTRUSTED_PATH'});continue
            if not source.is_file():
                files.append({'path':relative,'status':'UNAVAILABLE','reason':'FILE_MISSING'});continue
            cap=min(MAX_SOURCE_BYTES,remaining)
            if cap<=0:
                files.append({'path':relative,'status':'SKIPPED_BUDGET_EXHAUSTED'});continue
            read_bytes, truncated=scan_log(source,cap,totals,samples)
            remaining-=read_bytes
            files.append({'path':relative,'category':obj.get('category'),'status':'PARTIAL_SCAN' if truncated else 'SCANNED',
                          'scanned_bytes':read_bytes,'file_bytes':source.stat().st_size})
        except (OSError,ValueError) as exc:
            files.append({'path':relative,'status':'UNAVAILABLE','error':type(exc).__name__})
    idinfo=candidate_identity()
    result={'schema':SCHEMA,'session':sid,'generated_at':dt.datetime.now(dt.timezone.utc).isoformat(),
            'source_files':files,'evidence_status':'BOUNDED_LOG_ANALYSIS' if files else 'NO_ELIGIBLE_LOGS',
            'functions_are_log_mentions_not_verified_driver_calls':True,
            'vk_results_are_string_mentions_not_verified_return_values':True,
            'synchronization_validation':'NOT_PROVEN_UNLESS_VALIDATION_VUID_PRESENT',
            'driver_internal_submit_trace':'NOT_PROVEN_WITHOUT_INSTRUMENTATION',
            'driver_identity':idinfo,
            'counts':{k:(dict(v.most_common(80)) if isinstance(v,collections.Counter) else v) for k,v in totals.items()},
            'samples':samples,'privacy_warning':PRIVACY,
            'limits':{'per_file_bytes':MAX_SOURCE_BYTES,'total_scan_bytes':MAX_TOTAL_BYTES,
                      'window':'PREFIX_ONLY','not_a_vulkan_api_call_trace':True}}
    with path.open('x',encoding='utf-8') as f:
        json.dump(result,f,indent=2,ensure_ascii=False,sort_keys=True)
        f.write('\n');f.flush();os.fsync(f.fileno())
    return result
