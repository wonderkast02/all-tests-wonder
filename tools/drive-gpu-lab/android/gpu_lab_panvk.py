#!/usr/bin/env python3
"""PanVK evidence qualification from files *already captured* in a session.

Never claims vkQueueSubmit, CSF, native ICD identity or kernel events from
Wine API-call mentions. Bounded, opt-in debug profiles are documented separately.
The application / driver's environment is never modified by this module.
"""
import datetime as dt
import json
from pathlib import Path
import re

SCHEMA = 'gpu-lab-panvk-evidence-v2'
LIMIT_PER_FILE = 256 * 1024 * 1024  # explicit post-game safety budget
LIMIT_TOTAL = 512 * 1024 * 1024
MAX_LINES = 2_000_000  # explicit safety budget per file
MAX_SAMPLE = 5
MESAPANVK = re.compile(r'\[MESA-PANVK\]|\[gralloc-PANVK\]|\bpanvk_debug\s*:|\bPANVK(?:\b|[:_-])', re.I)
SYNC = re.compile(r'\b(sync|semaphore|fence|submit|queue|timeline|acquire|present|csf|syncobj)\b', re.I)
KERNEL = re.compile(r'\b(kbase|panthor|mali[_ -]kbase|csf|GPU fault|GPU reset)\b',re.I)
# Deliberately narrow: do not infer an *internal* trace from Wine vulkan thunk output.
PANVK_INTERNAL = re.compile(r'\b(?:PANVK_(?:INTERNAL_(?:SUBMIT|SYNC|CSF)|TRACE_EVENT)|SYNC-A3)\b', re.I)
INTERNAL_SUBMIT = re.compile(r'\bPANVK_INTERNAL_SUBMIT\b',re.I)
NATIVE_SYNC_A3 = re.compile(r'\bSYNC-A3\b[^\r\n]{0,200}\bnative GPU internal wait path active\b', re.I)
STARTUP = re.compile(r'\bpanvk_debug\s*:\s*(.{0,200})', re.I)
CATEGORIES = {'PANVK','MESA','WINE','VULKAN','SYSTEM','OTHER','DXVK','VKD3D'}


def _safe_files(root, sources):
    # No symlink traversal, absolute paths or user-supplied ../ entries.
    chosen = {}
    for item in sources:
        if item.get('category') not in CATEGORIES: continue
        name = item.get('archive_path','')
        rel = Path(name)
        if not name or rel.is_absolute() or '..' in rel.parts:continue
        p = root / rel
        if p.is_symlink() or not p.is_file():continue
        try:
            if p.resolve().is_relative_to(root.resolve()):
                chosen[rel.as_posix()] = p
        except OSError:continue
    logcat = root / 'Android/DriverDiagnostics/android-logcat.txt'
    if logcat.exists() and logcat.is_file() and not logcat.is_symlink():
        chosen['Android/DriverDiagnostics/android-logcat.txt'] = logcat
    return chosen


def _bounded_lines(path, limit):
    """Read a full source in constant memory until a *reported* byte/line limit.

    Each yield is (limited decoded fragment, raw byte count). No 16-MiB head-only
    truncation. Long untrusted lines are drained in <=64-KiB chunks; bounded text
    decoding never receives more than 2-KiB per fragment.
    """
    used = 0
    lines = 0
    with path.open('rb') as fp:
        while used < limit and lines < MAX_LINES:
            raw = fp.readline(min(65536, limit - used))
            if not raw:
                break
            used += len(raw)
            lines += 1
            yield raw[:2048].decode('utf-8','replace'), len(raw)


def analyze_panvk(work, session, source_entries):
    root = Path(work)
    stats = {'panvk_mesa_lines':0, 'panvk_sync_context_lines':0,
             'panvk_internal_marker_lines':0, 'sync_a3_native_wait_markers':0,
             'internal_submit_marker_lines':0, 'kernel_context_lines':0,
             'logcat_lines_observed':0, 'files_scanned':0}
    previews = {'panvk':[], 'kernel':[], 'startup_debug_flags':[], 'sync_a3':[]}
    files = []
    remaining = LIMIT_TOTAL
    skipped_due_to_budget = []
    for name, path in sorted(_safe_files(root,source_entries).items()):
        if remaining <= 0:
            skipped_due_to_budget.append(name)
            continue
        cap = min(remaining, LIMIT_PER_FILE)
        used = 0
        count = 0
        markers = {'panvk':0,'kernel':0,'internal':0, 'sync_a3':0}
        for row, size in _bounded_lines(path,cap):
            used += size
            count += 1
            is_panvk = bool(MESAPANVK.search(row))
            if is_panvk:
                stats['panvk_mesa_lines']+=1
                markers['panvk']+=1
                if len(previews['panvk']) < MAX_SAMPLE:
                    previews['panvk'].append(row.strip()[:240])
                if SYNC.search(row):
                    stats['panvk_sync_context_lines']+=1
                if PANVK_INTERNAL.search(row):
                    stats['panvk_internal_marker_lines']+=1
                    markers['internal']+=1
                if NATIVE_SYNC_A3.search(row):
                    stats['sync_a3_native_wait_markers']+=1
                    markers['sync_a3']+=1
                    if len(previews['sync_a3']) < MAX_SAMPLE:
                        previews['sync_a3'].append(row.strip()[:240])
                if INTERNAL_SUBMIT.search(row):
                    stats['internal_submit_marker_lines']+=1
                matched=STARTUP.search(row)
                if matched and len(previews['startup_debug_flags']) < MAX_SAMPLE:
                    previews['startup_debug_flags'].append(matched.group(1).strip()[:160])
            # Only kernel/logcat may count as kernel-context evidence. A Wine
            # string containing `kbase` MUST NOT qualify as a kernel trace.
            if ('logcat' in name.lower() or 'kernel' in name.lower() or 'dmesg' in name.lower()) and KERNEL.search(row):
                stats['kernel_context_lines']+=1
                markers['kernel']+=1
                if len(previews['kernel'])<MAX_SAMPLE:
                    previews['kernel'].append(row.strip()[:240])
            if 'logcat' in name.lower():
                stats['logcat_lines_observed']+=1
        total_bytes = path.stat().st_size
        unscanned_bytes = max(0,total_bytes-used)
        stop_reason = ('TOTAL_BYTES_LIMIT' if remaining == cap and total_bytes > cap else
                       'FILE_BYTES_LIMIT' if total_bytes > cap else
                       'LINE_COUNT_LIMIT' if count >= MAX_LINES and unscanned_bytes else
                       'NONE')
        remaining -= used
        stats['files_scanned'] += 1
        is_shared = 'WINE/' in name.upper() or 'WINE_DEBUG' in name.upper()
        files.append({'file':name,'file_bytes':total_bytes,
                      'scan_budget_bytes':cap,'bytes_processed_approx':used,
                      'unscanned_bytes':unscanned_bytes, 'bounded_scan': bool(unscanned_bytes),
                      'line_fragments_scanned':count, 'stop_reason':stop_reason,
                      'attribution':'SHARED_WINE_NOT_SESSION_ISOLATED' if is_shared else 'SOURCE_FILE_CAPTURED',
                      'panvk_lines':markers['panvk'],'internal_markers':markers['internal'],
                      'sync_a3_markers':markers['sync_a3'],'kernel_lines':markers['kernel']})
    complete = not skipped_due_to_budget and not any(f['unscanned_bytes'] for f in files)
    status = ('INTERNAL_MARKERS_IN_CAPTURED_LOGS' if stats['panvk_internal_marker_lines'] else
              'PANVK_MESA_MESSAGES_ONLY' if stats['panvk_mesa_lines'] else
              'NO_PANVK_DRIVER_MESSAGES_CAPTURED')
    report = {
        'schema':SCHEMA,'session':session,
        'generated_at':dt.datetime.now(dt.timezone.utc).isoformat(),
        'status':status,'counts':stats,'sources':files,'samples':previews,
        'source_scan_completeness': 'ALL_SELECTED_FILES_FULLY_SCANNED' if complete else 'PARTIAL_LIMIT_REACHED',
        'unscanned_sources':skipped_due_to_budget,
        'source_attribution_note':'Wine debug logs may be shared across processes and sessions. A PanVK line in such a file is not proof of the specific game PID or a successful GPU operation.',
        'panvk_debug_activation':'ONLY_CONFIRMED_IF_PANVK_DEBUG_STARTUP_MESSAGE_PRESENT' if previews['startup_debug_flags'] else 'NOT_PROVEN',
        'native_wait_implementation_marker':'TEXT_MARKER_OBSERVED_NOT_FENCE_COMPLETION_PROOF' if stats['sync_a3_native_wait_markers'] else 'NOT_PROVEN',
        'driver_internal_submit_trace':'TEXT_MARKER_ONLY_NOT_RUNTIME_VERIFIED' if stats['internal_submit_marker_lines'] else 'NOT_PROVEN',
        'synchronization_cause':'NOT_PROVEN_FROM_TEXT_LOGS',
        'kernel_csf_trace':'KERNEL_TEXT_LINES_NOT_FULL_TRACE' if stats['kernel_context_lines'] else 'NOT_PROVEN',
        'loaded_panvk_library':'NOT_PROVEN',
        'environment_injection':'NEVER_PERFORMED',
        'profile_guidance':'See source/docs/PANVK-DIAGNOSTICS-ENV.md; enable inside actual driver process only',
        'privacy_warning':'Log snippets can include filesystem paths. Review before sharing.',
        'note':'SYNC-A3 identifies native wait-path text; neither it nor Wine Vulkan API mentions prove fence completion, CSF traces or successful internal submits.'}
    dest = root/'Android'/'DriverDiagnostics'/'PANVK-EVIDENCE.json'
    dest.parent.mkdir(parents=True,exist_ok=True)
    with dest.open('x',encoding='utf-8') as f:
        json.dump(report,f,indent=2,sort_keys=True,ensure_ascii=False)
        f.write('\n');f.flush()
    return report
