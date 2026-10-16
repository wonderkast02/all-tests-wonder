#!/usr/bin/env python3
"""GPU Lab independent Android host-side evidence collector. Standard library only.
Never opens a target game handle, runs taskkill, changes Wine/DXVK, or overwrites an archive.
"""
import argparse
import datetime as dt
import errno
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import subprocess
import sys
import time
import zipfile
from pathlib import Path
from gpu_lab_diagnostics import DiagnosticManager
from gpu_lab_evidence import analyze_session
from gpu_lab_panvk import analyze_panvk

SCHEMA = "gpu-lab-archive-v1"
FILE_LIMIT = 1024 * 1024 * 1024
TOTAL_LIMIT = 4 * 1024 * 1024 * 1024
CANDIDATE_LIMIT = 20000
MAX_DEPTH = 8
CATEGORIES = ("DXVK", "VKD3D", "WINE", "BOX64", "FEX", "VULKAN", "MESA", "PANVK", "SYSTEM", "OTHER")
LOG_SUFFIXES = (".log", ".txt", ".jsonl", ".trace", ".dmp", ".mdmp", ".crash")


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def atomic_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".partial-" + secrets.token_hex(6))
    with tmp.open("x", encoding="utf-8") as fp:
        json.dump(data, fp, indent=2, ensure_ascii=False, sort_keys=True)
        fp.write("\n")
        fp.flush()
        os.fsync(fp.fileno())
    os.replace(tmp, path)


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for part in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(part)
    return h.hexdigest()


def classify(path):
    s = str(path).lower()
    if "dxvk" in s or re.search(r"_d3d(?:9|10|11)\.log$", s) or "_dxgi.log" in s:
        return "DXVK"
    if "vkd3d" in s: return "VKD3D"
    if "box64" in s: return "BOX64"
    if "fex" in s: return "FEX"
    if "panvk" in s or "panthor" in s or "panfrost" in s: return "PANVK"
    if "vulkan" in s or "vklayer" in s: return "VULKAN"
    if "mesa" in s or "zink" in s: return "MESA"
    if "wine" in s or "winedbg" in s: return "WINE"
    if "logcat" in s or "dmesg" in s or "kernel" in s: return "SYSTEM"
    return "OTHER"


def source_roots(documents, additional=()):
    names = ("Winlator", "winlator", "Bannerlator", "bannerlator", "GameHub", "gamehub")
    roots = [Path(documents) / n for n in names]
    for x in additional:
        roots.append(Path(x))
    seen = set()
    for x in roots:
        key = str(x)
        if key not in seen and x.is_dir():
            seen.add(key)
            yield x


def relevant_file(path):
    lower = path.name.lower()
    if lower.endswith(LOG_SUFFIXES): return True
    # Vendor-specific diagnostic dumps, but no arbitrary game files or large caches.
    return any(x in lower for x in ("dxvk", "vkd3d", "wine_debug", "box64", "fex", "vulkan")) and lower.endswith((".out", ".err"))


def scan_sources(roots, exclude=()):
    """Read-only, bounded source inventory. Symlink dirs are not followed."""
    candidates = {}
    errors = []
    excluded = [os.path.realpath(x) for x in exclude]
    count = 0
    for root in roots:
        root = Path(root)
        if not root.is_dir():
            continue
        root_depth = len(root.parts)
        for folder, dirs, files in os.walk(root, followlinks=False):
            current_real = os.path.realpath(folder)
            if any(current_real == e or current_real.startswith(e + os.sep) for e in excluded):
                dirs[:] = []
                continue
            if len(Path(folder).parts) - root_depth >= MAX_DEPTH:
                dirs[:] = []
            else:
                dirs[:] = [d for d in dirs if not d.startswith(".build") and not Path(folder, d).is_symlink()]
            for name in files:
                p = Path(folder, name)
                if not relevant_file(p) or p.is_symlink():
                    continue
                count += 1
                if count > CANDIDATE_LIMIT:
                    errors.append("SOURCE_SCAN_LIMIT_REACHED")
                    return candidates, errors
                try:
                    st = p.stat()
                    if not p.is_file():
                        continue
                    candidates[str(p)] = {"size": st.st_size, "mtime_ns": st.st_mtime_ns, "category": classify(p)}
                except OSError as exc:
                    errors.append(f"STAT_FAIL {str(p)} {type(exc).__name__}")
    return candidates, errors


def select_sources(current, baseline, start_seconds, stop_seconds, game_exe=""):
    # The DXVK game log may have been opened just before the Android collector
    # completed its baseline snapshot. Accept only a strongly game-identified
    # file modified within this exact session window, never arbitrary stale logs.
    game_stem = Path(game_exe).stem.lower()
    picks = []
    omissions = []
    for name, stat in sorted(current.items()):
        before = baseline.get(name)
        modified = (stat["mtime_ns"] / 1e9) >= start_seconds - 2
        changed = before is None or before["size"] != stat["size"] or before["mtime_ns"] != stat["mtime_ns"]
        file_name=Path(name).name.lower()
        matched_game_log=(game_stem and stat["category"] in ("DXVK","VKD3D","VULKAN","WINE")
                          and (file_name.startswith(game_stem+"_") or file_name.startswith(game_stem+"-")))
        identity_attributed=(not changed and modified and matched_game_log)
        if not modified or (not changed and not identity_attributed):
            omissions.append({"file": name, "reason": "OLDER_OR_UNCHANGED"})
            continue
        if stat["mtime_ns"] / 1e9 > stop_seconds + 8:
            omissions.append({"file": name, "reason": "OUTSIDE_END_WINDOW"})
            continue
        picks.append((name, stat, "MATCHED_GAME_LOG_WITHIN_TIME_WINDOW" if identity_attributed else ("NEW" if before is None else "MODIFIED")))
    for lost in sorted(set(baseline) - set(current)):
        omissions.append({"file": lost, "reason": "SOURCE_DISAPPEARED_AFTER_BASELINE"})
    return picks, omissions


def safe_basename(name):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:90] or "unnamed.log"


def capture_sources(work, picks, omitted):
    entries = []
    errors = []
    total = 0
    processed = set()
    seen_contents = {}
    for name, st, attribution in picks:
        if name in processed: raise AssertionError("duplicate source path")
        processed.add(name)
        if st["size"] > FILE_LIMIT or total + st["size"] > TOTAL_LIMIT:
            omitted.append({"file": name, "reason": "CAPTURE_SIZE_LIMIT", "bytes": st["size"]})
            continue
        cat = st["category"]
        path = Path(name)
        # Multiple Android shared-storage spellings may expose identical log
        # bytes. Keep the alias evidence, but do not store the same 78 MB twice.
        # SHA of source is recalculated, not inferred from filename.
        try:
            dedup_hash = digest(path)
            dedup_key = (cat, st["size"], dedup_hash)
            if dedup_key in seen_contents:
                canonical = seen_contents[dedup_key]
                omitted.append({"file": name, "reason": "DUPLICATE_CONTENT_ALIAS", "sha256": dedup_hash,
                                "canonical_archive_path": canonical, "bytes": st["size"]})
                continue
        except OSError as exc:
            errors.append({"file":name,"reason":"HASH_BEFORE_CAPTURE_FAILED", "detail":str(exc)})
            continue
        dst = Path(work) / "Android" / "logs" / cat / (hashlib.sha256(os.fsencode(name)).hexdigest()[:18] + "-" + safe_basename(path.name))
        dst.parent.mkdir(parents=True, exist_ok=True)
        captured = False
        for attempt in range(2):
            try:
                before = path.stat()
                if not path.is_file() or path.is_symlink():
                    raise OSError("not a regular source file")
                h = hashlib.sha256()
                with path.open("rb") as src, dst.open("wb") as target:
                    while True:
                        b = src.read(1024 * 1024)
                        if not b: break
                        h.update(b)
                        target.write(b)
                    target.flush(); os.fsync(target.fileno())
                after = path.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or dst.stat().st_size != before.st_size:
                    if attempt == 0: continue
                    raise OSError("source changed during capture")
                if digest(path) != h.hexdigest():
                    if attempt == 0: continue
                    raise OSError("source hash differs after capture")
                rel = dst.relative_to(work).as_posix()
                entries.append({"source": name, "archive_path": rel, "bytes": before.st_size, "sha256": h.hexdigest(), "category": cat, "attribution": attribution, "source_verified": True})
                seen_contents[dedup_key] = rel
                total += before.st_size
                captured = True
                break
            except (OSError, ValueError) as exc:
                if attempt == 1:
                    errors.append({"file": name, "reason": type(exc).__name__, "detail": str(exc)})
        if not captured:
            try: dst.unlink(missing_ok=True)
            except OSError: pass
    return entries, errors, total


def copy_windows_files(win_src, work):
    entries, errors = [], []
    if not win_src.is_dir():
        return entries, ["WINDOWS_SESSION_DIRECTORY_MISSING"]
    for base, dirs, files in os.walk(win_src, followlinks=False):
        dirs[:] = [d for d in dirs if not Path(base, d).is_symlink()]
        for name in sorted(files):
            src = Path(base, name)
            if src.is_symlink():
                errors.append("SYMLINK_SKIPPED " + str(src)); continue
            rel = Path("Windows") / src.relative_to(win_src)
            dst = work / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            try:
                before = src.stat(); shutil.copyfile(src, dst); after = src.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise OSError("WINDOWS_SOURCE_MUTATED_DURING_COPY")
                if digest(src) != digest(dst):
                    raise OSError("WINDOWS_COPY_HASH_MISMATCH")
                entries.append({"path": rel.as_posix(), "sha256": digest(dst), "bytes": dst.stat().st_size})
            except OSError as exc:
                errors.append(f"WIN_COPY_FAIL {rel} {exc}")
                dst.unlink(missing_ok=True)
    return entries, errors


def device_info():
    result = {"captured_at": utc(), "schema": "gpu-lab-device-v1", "properties": {}, "cpu": "UNAVAILABLE", "memory": "UNAVAILABLE", "gpu_device": "NOT_PROVEN"}
    keys = ("ro.product.manufacturer", "ro.product.model", "ro.product.device", "ro.board.platform", "ro.soc.model", "ro.build.version.release", "ro.build.version.sdk")
    for key in keys:
        try:
            q = subprocess.run(["getprop", key], text=True, capture_output=True, timeout=2)
            result["properties"][key] = q.stdout.strip() if q.returncode == 0 else "UNAVAILABLE"
        except (OSError, subprocess.TimeoutExpired): result["properties"][key] = "UNAVAILABLE"
    for key, path in (("cpu", "/proc/cpuinfo"), ("memory", "/proc/meminfo")):
        try: result[key] = Path(path).read_text(errors="replace")[:65536]
        except OSError: pass
    return result


def verified_zip(path, expected_hashes):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if len(names) != len(set(names)): raise ValueError("DUPLICATE_ZIP_ENTRY")
        if z.testzip() is not None: raise ValueError("CRC_ZIP_FAIL")
        if set(expected_hashes).difference(names): raise ValueError("ZIP_FILE_MISSING")
        for n in names:
            if n.startswith("/") or ".." in Path(n).parts or "\\" in n: raise ValueError("UNSAFE_ZIP_PATH")
        for n, hash_value in expected_hashes.items():
            if hashlib.sha256(z.read(n)).hexdigest() != hash_value:
                raise ValueError("ZIP_SOURCE_SHA_MISMATCH: " + n)
    return True


def pack_immutable(work, dest_dir, desired):
    """Never overwrites an existing archive; links if available, otherwise exclusive copy."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    files = {}
    for base, _dirs, fnames in os.walk(work):
        for name in fnames:
            path = Path(base, name)
            if path.is_symlink(): raise ValueError("SYMLINK_IN_STAGING")
            rel = path.relative_to(work).as_posix()
            if rel in files: raise ValueError("DUPLICATE_STAGING_PATH")
            files[rel] = digest(path)
    (work / "META").mkdir(exist_ok=True)
    inventory = [{"path": k, "sha256": v, "bytes": (work / k).stat().st_size} for k, v in sorted(files.items())]
    atomic_json(work / "META" / "FILE-INVENTORY.json", {"schema": SCHEMA, "files": inventory})
    files["META/FILE-INVENTORY.json"] = digest(work / "META" / "FILE-INVENTORY.json")
    with (work / "META" / "SHA256SUMS.txt").open("x", encoding="utf-8") as fp:
        for n, v in sorted(files.items()): fp.write(v + "  " + n + "\n")
    files["META/SHA256SUMS.txt"] = digest(work / "META" / "SHA256SUMS.txt")
    # Same session must NEVER be published twice with a second suffix.
    # An existing matching ZIP is an idempotent replay; divergent bytes are BLOCKED.
    canonical = dest_dir / desired
    if canonical.exists():
        verified_zip(canonical,files)
        return canonical,digest(canonical),len(files)
    for tries in range(8):
        dest = dest_dir / desired
        if dest.exists():
            verified_zip(dest,files)
            return dest,digest(dest),len(files)
        staging = dest_dir / ("." + dest.name + ".partial-" + secrets.token_hex(6))
        try:
            with staging.open("xb") as outf:
                with zipfile.ZipFile(outf, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=3, allowZip64=True) as z:
                    for rel in sorted(files):
                        z.write(work / rel, arcname=rel)
                outf.flush(); os.fsync(outf.fileno())
            verified_zip(staging, files)
            # Termux Python builds can omit os.link altogether. FUSE /sdcard
            # can also reject it even when the Python symbol exists.
            hardlink = getattr(os, "link", None)
            published = False
            if hardlink is not None:
                try:
                    hardlink(staging, dest)
                    published = True
                except FileExistsError:
                    raise ValueError("EXCLUSIVE_ARCHIVE_NAME_COLLISION")
                except OSError as link_error:
                    if link_error.errno not in (
                        errno.EPERM, errno.EOPNOTSUPP, errno.ENOSYS,
                        errno.EXDEV, errno.EACCES, errno.EINVAL
                    ):
                        raise
            if not published:
                # Exclusive creation works on Android shared storage.
                # Never truncate or replace a prior archive; remove incomplete
                # own destination if any operation fails.
                try:
                    with staging.open("rb") as src, dest.open("xb") as dst:
                        for chunk in iter(lambda: src.read(1024*1024), b""):
                            dst.write(chunk)
                        dst.flush()
                        os.fsync(dst.fileno())
                    verified_zip(dest, files)
                except FileExistsError:
                    raise ValueError("EXCLUSIVE_ARCHIVE_NAME_COLLISION")
                except BaseException:
                    dest.unlink(missing_ok=True)
                    raise
            verified_zip(dest, files)
            return dest, digest(dest), len(files)
        finally:
            staging.unlink(missing_ok=True)
    raise FileExistsError("ARCHIVE_NAME_COLLISION_RETRY_EXHAUSTED")




def read_cpu_ticks():
    try:
        nums = [int(n) for n in Path("/proc/stat").read_text().splitlines()[0].split()[1:]]
        return sum(nums), sum(nums[3:5])
    except (OSError, IndexError, ValueError):
        return None


def host_metrics(last=None):
    result = {"captured_at": utc(), "cpu_host": "N/D", "ram_host": "N/D", "gpu_busy": "N/D", "gpu_freq": "N/D", "gpu_temp": "N/D", "soc_temp": "N/D"}
    ticks = read_cpu_ticks()
    if ticks and last and last[0]:
        total, idle = ticks[0] - last[0][0], ticks[1] - last[0][1]
        if total > 0 and 0 <= idle <= total:
            result["cpu_host"] = f"{100*(total-idle)/total:.1f}%"
    try:
        vals = {}
        for line in Path("/proc/meminfo").read_text().splitlines():
            key, v = line.split(":", 1)
            if key in ("MemTotal", "MemAvailable"):
                vals[key] = int(v.split()[0])
        if len(vals) == 2:
            result["ram_host"] = f"{vals['MemAvailable']/1024:.0f}MB / {vals['MemTotal']/1024:.0f}MB"
    except (OSError, ValueError, IndexError): pass
    # Android unprivileged Termux may see sysfs directories but is denied
    # listing them. GPU metrics are optional: report N/D, never abort logs.
    thermal_root = Path("/sys/class/thermal")
    try:
        for idx, zone in enumerate(thermal_root.glob("thermal_zone*")):
            if idx >= 100: break
            try:
                typ = (zone / "type").read_text().strip().lower()
                raw = float((zone / "temp").read_text().strip())
                celsius = raw / 1000 if raw > 400 else raw
                if not (-20 <= celsius <= 150): continue
                if "gpu" in typ and result["gpu_temp"] == "N/D":
                    result["gpu_temp"] = f"{celsius:.1f}°C"
                elif any(x in typ for x in ("soc", "ap_ntc", "cpu")) and result["soc_temp"] == "N/D":
                    result["soc_temp"] = f"{celsius:.1f}°C"
            except (OSError, ValueError):
                continue
    except OSError:
        pass
    devfreq_root = Path("/sys/class/devfreq")
    try:
        for idx, device in enumerate(devfreq_root.iterdir()):
            if idx >= 80: break
            if not any(x in device.name.lower() for x in ("gpu", "mali", "3d")):
                continue
            try:
                raw = (device / "cur_freq").read_text().strip()
                v = int(raw)
                if v > 0:
                    result["gpu_freq"] = f"{v/1000000:.0f}MHz" if v > 10000000 else f"{v/1000:.0f}MHz"
            except (OSError, ValueError):
                pass
            break
    except OSError:
        pass
    return result, ticks


def matching_dxvk_identity(sources, game, started):
    stem = Path(game or "").stem.lower()
    if not stem:return {}
    for source, st in sorted(sources.items(), key=lambda x: x[1]["mtime_ns"], reverse=True):
        if st["category"] != "DXVK" or stem not in Path(source).name.lower() or st["mtime_ns"]/1e9 < started-2:
            continue
        try:
            with open(source, "rb") as fp:
                text = fp.read(65536).decode("utf-8", errors="replace")
            version = re.search(r"DXVK\s*:\s*v?([0-9]+\.[0-9]+(?:\.[0-9]+)?[A-Za-z0-9._-]*)", text, flags=re.I)
            gpu = re.search(r"(?:Device name\s*:\s*:?|Found device\s*:)\s*([^\r\n]+)", text, flags=re.I)
            device = gpu.group(1).strip()[:100] if gpu else "N/D"
            driver = re.search(r"Driver\s*:\s*(Wrapper driver [^\r\n]+)", text, flags=re.I)
            return {"dxvk": version.group(1) if version else "N/D", "gpu":device,
                    "gpu_source": "DXVK log da sessão", "api": "D3D9 → Vulkan" if "_d3d9.log" in source.lower() else "DXVK → Vulkan",
                    "wrapper": "Wrapper" if "Wrapper(" in device else "N/D",
                    "driver": driver.group(1)[:80] if driver else "N/D",
                    "identity_source": "DXVK_LOG_FILENAME_TIME_MATCH_NOT_ACTIVE_DRIVER_HASH"}
        except OSError:continue
    return {}

class Collector:
    def __init__(self, root, run, token, documents, extra_roots=(), grace=4):
        self.root = Path(root)
        self.run = run
        self.token = token
        self.win = self.root / "Logs" / "_control" / run / "windows"
        self.host = self.root / "Logs" / "_control" / run / "android"
        self.archives = self.root / "Logs"
        self.returns = self.root.parent / "Log Termux"
        self.return_path = None
        self.roots = tuple(source_roots(documents, extra_roots))
        self.grace = grace
        self.baseline, errors = scan_sources(self.roots, exclude=(self.root,))
        self.host.mkdir(parents=True, exist_ok=True)
        self.seen = {}
        self.archived = set()
        self.ignored_foreign_sessions = set()
        self.collector_errors = errors
        self._ticks = None
        self._last_metrics = 0
        self._last_identity_scan = 0
        self._last_identity = {}
        self._last_identity_session = None
        # Independent read-only diagnostic subsystem; exceptions never kill Win32 GUI or game.
        self.diag = None
        try:
            self.diag = DiagnosticManager(self.root, self.run, self.token)
        except Exception as exc:
            self.collector_errors.append('DIAGNOSTICS_START_UNAVAILABLE:'+type(exc).__name__)
        atomic_json(self.host / "android-device.json", device_info())
        self.log("collector_ready", {"sources_initial": len(self.baseline), "roots": [str(x) for x in self.roots]})

    def log(self, typ, data):
        event = {"time": utc(), "type": typ, **data}
        with (self.host / "collector-events.jsonl").open("a", encoding="utf-8") as fp:
            fp.write(json.dumps(event, ensure_ascii=False, sort_keys=True) + "\n")

    def get_sessions(self):
        parent = self.root / "Logs"
        if not parent.is_dir(): return []
        # A session's directory is named from the detected EXE, not "run-...".
        # Supervisor internals live in Logs/_control and are excluded.
        result = []
        for folder in sorted(parent.iterdir()):
            if folder.name.startswith("_") or not folder.is_dir() or folder.is_symlink():
                continue
            win = folder / "Windows"
            if (win / "session.json").is_file() and not win.is_symlink():
                result.append(win)
        return result

    def tick(self):
        if time.monotonic() - self._last_metrics > 2:
            measurements, ticks = host_metrics((self._ticks,))
            self._ticks = ticks
            measurements["token"] = self.token
            measurements["run"] = self.run
            active = []
            for directory in self.get_sessions():
                try:
                    meta=json.loads((directory / "session.json").read_text())
                    if meta.get("state")=="active" and meta.get("token")==self.token:
                        active.append(meta)
                except (OSError,ValueError):pass
            if active:
                latest=active[-1]
                sid=latest.get("session")
                if self._last_identity_session != sid:
                    self._last_identity_session=sid
                    self._last_identity={}
                    self._last_identity_scan=0
                if time.monotonic() - self._last_identity_scan > 5 and self._last_identity.get("gpu", "N/D") == "N/D":
                    self._last_identity_scan = time.monotonic()
                    sources, _err = scan_sources(self.roots, exclude=(self.root,))
                    try: started=dt.datetime.fromisoformat(latest["started_at"].replace("Z","+00:00")).timestamp()
                    except (KeyError,ValueError):started=time.time()
                    self._last_identity=matching_dxvk_identity(sources, latest.get("game"), started)
                measurements.update(self._last_identity)
            else:
                self._last_identity={}; self._last_identity_session=None
            atomic_json(self.host / "host-live.json",measurements)
            self._last_metrics = time.monotonic()
        if self.diag is not None:
            try:self.diag.tick(self.get_sessions())
            except Exception as exc:
                self.collector_errors.append('DIAGNOSTICS_TICK_PARTIAL:'+type(exc).__name__)
        for directory in self.get_sessions():
            sid = directory.parent.name
            if sid in self.archived: continue
            meta_path = directory / "session.json"
            try: meta = json.loads(meta_path.read_text())
            except (OSError, ValueError) as exc:
                self.log("session_metadata_unreadable", {"session": sid, "reason": str(exc)})
                continue
            if meta.get("token") != self.token or meta.get("run") != self.run or meta.get("session") != sid:
                if sid not in self.ignored_foreign_sessions:
                    self.ignored_foreign_sessions.add(sid)
                    self.log("session_token_mismatch_ignored", {"session": sid})
                continue
            if sid not in self.seen:
                self.seen[sid] = {"discovered": time.time(), "baseline": dict(self.baseline), "end_seen": None}
                self.log("session_discovered", {"session": sid, "game": meta.get("game", "UNAVAILABLE")})
            state = self.seen[sid]
            if meta.get("state") == "finished":
                if state["end_seen"] is None: state["end_seen"] = time.monotonic()
                if time.monotonic() - state["end_seen"] >= self.grace:
                    result = self.archive_session(directory, meta, state["baseline"])
                    self.archived.add(sid)
                    self.log("session_archived", result)
                    self.baseline, errs = scan_sources(self.roots, exclude=(self.root,))
                    self.collector_errors.extend(errs)

    def archive_session(self, directory, meta, baseline, forced_partial=False):
        sid = directory.parent.name
        work = self.host / (".build-" + sid + "-" + secrets.token_hex(4))
        work.mkdir(mode=0o700)
        warnings = []
        try:
            _winfiles, winerr = copy_windows_files(directory, work)
            warnings.extend(winerr)
            current, scanerr = scan_sources(self.roots, exclude=(self.root,))
            warnings.extend(scanerr)
            try:
                start = dt.datetime.fromisoformat(meta["started_at"].replace("Z", "+00:00")).timestamp()
            except (ValueError, KeyError):
                start = time.time()
                warnings.append("INVALID_WINDOWS_START_TIME")
            try:
                end = dt.datetime.fromisoformat(meta["ended_at"].replace("Z", "+00:00")).timestamp()
            except (ValueError, KeyError): end = time.time()
            picked, skipped = select_sources(current, baseline, start, end, meta.get("game", ""))
            entries, copy_err, total = capture_sources(work, picked, skipped)
            warnings.extend([x["file"] + " " + x["detail"] for x in copy_err])
            shutil.copyfile(self.host / "android-device.json", work / "Android" / "device.json") if (work / "Android").is_dir() else self._place_device(work)
            if (self.host / "host-live.json").exists():
                shutil.copyfile(self.host / "host-live.json", work / "Android" / "host-live.json")
            if (self.host / "collector-events.jsonl").exists():
                (work / "Android" / "supervisor").mkdir(parents=True, exist_ok=True)
                shutil.copyfile(self.host / "collector-events.jsonl", work / "Android" / "supervisor" / "collector-events.jsonl")
            for source, target in ((self.win / "controller-events.jsonl", work / "Control" / "controller-events.jsonl"), (self.win / "gui-read-events.jsonl", work / "Control" / "gui-read-events.jsonl"), (self.win / "worker-stderr.txt", work / "Control" / "worker-stderr.txt"), (self.win / "gui-stderr.txt", work / "Control" / "gui-stderr.txt"), (self.win / "gui-last-exit.json", work / "Control" / "gui-last-exit.json"), (self.win / "gui-stdout.txt", work / "Control" / "gui-stdout.txt"), (self.win / "worker-stdout.txt", work / "Control" / "worker-stdout.txt"), (self.win / "stop.request", work / "Control" / "stop.request"), (self.win / "latest-session.json", work / "Control" / "latest-session.json"), (self.win / "observer.log", work / "Control" / "observer.log"), (self.win / "ui-health.json", work / "Control" / "ui-health.json"), (self.win / "run-status.json", work / "Control" / "run-status.json"), (self.win / "observer-identity.json", work / "Control" / "observer-identity.json"), (self.win / "ui-timeline.jsonl", work / "Control" / "ui-timeline.jsonl"), (self.win / "startup-timeline.jsonl", work / "Control" / "startup-timeline.jsonl"), (self.win / "probe-stderr.txt", work / "Control" / "probe-stderr.txt"), (self.win / "probe-stdout.txt", work / "Control" / "probe-stdout.txt")):
                if source.exists():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    try: shutil.copyfile(source, target)
                    except OSError as exc: warnings.append("CONTROL_COPY_FAIL " + str(exc))
            # Export session-scoped Android logcat/kernel/sysfs diagnostics BEFORE ZIP finalization.
            if self.diag is not None:
                try:
                    self.diag.export_session(sid, work)
                except Exception as exc:
                    warnings.append('DRIVER_DIAGNOSTICS_PARTIAL:'+type(exc).__name__)
            else:
                warnings.append('DRIVER_DIAGNOSTICS_UNAVAILABLE')
            # Offline-only analysis: log mentions, VkResult tokens, and optional
            # candidate driver identity. Never touches live game/driver processes.
            try:
                analyze_session(work, sid, entries)
            except Exception as exc:
                warnings.append('DRIVER_EVIDENCE_REPORT_PARTIAL:'+type(exc).__name__)
            # PanVK-specific coverage report. This never injects logging or
            # treats Wine Vulkan thunk mentions as native driver trace.
            try:
                analyze_panvk(work, sid, entries)
            except Exception as exc:
                warnings.append('PANVK_EVIDENCE_PARTIAL:'+type(exc).__name__)
            if meta.get("state") != "finished" or forced_partial:
                warnings.append("WINDOWS_SESSION_NOT_FINISHED_OR_INTERRUPTED")
            if meta.get("reason") not in ("PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN", "GAME_PROCESS_EXITED"):
                warnings.append("GAME_EXIT_NOT_OBSERVED_OR_SESSION_INTERRUPTED")
            coverage = {cat: sum(1 for e in entries if e["category"] == cat) for cat in CATEGORIES}
            result = {
                "schema": SCHEMA, "project": "GPU Lab", "session": sid,
                "run": self.run, "token": self.token, "game": meta.get("game"), "game_title":meta.get("game_title",meta.get("game")),
                "started_at": meta.get("started_at"), "ended_at": meta.get("ended_at"),
                "windows_state": meta.get("state"), "windows_reason": meta.get("reason", "UNAVAILABLE"),
                "game_exit_cause": "NOT_PROVEN", "no_target_termination": True,
                "source_roots_scanned": [str(p) for p in self.roots],
                "categories_copied": coverage, "files_copied": entries,
                "copied_total_bytes": total, "sources_skipped_or_unavailable": skipped,
                "warnings": warnings, "initial_collector_errors": self.collector_errors,
                "archive_integrity": "VALIDATION_PENDING_AT_ASSEMBLY",
                "collection_completeness": "PARTIAL" if warnings or forced_partial else "BOUNDED_SOURCES_ONLY",
                "external_logs_are_not_universal": True,
            }
            atomic_json(work / "META" / "COLLECTION-REPORT.json", result)
            (work / "README.txt").write_text("GPU Lab | archival integrity is independent of coverage and game outcome.\nEvidence is read-only. No driver/game process modification.\nFPS absent without trustworthy layer: N/D.\n", encoding="utf-8")
            # Exact same basename for physical session directory and ZIP.
            # Session name is anchored to the automatically discovered executable.
            exe = re.sub(r"[^A-Za-z0-9_.-]", "-", str(meta.get("game") or "unknown.exe"))
            if not sid.startswith(exe + "-"):
                raise ValueError("SESSION_NAME_NOT_ANCHORED_TO_DETECTED_EXE")
            desired = sid + ".zip"
            archive, sha, count = pack_immutable(work, self.archives, desired)
            # Store a browsable, per-session evidence tree next to Windows telemetry.
            # Do not replace an existing file in the session folder.
            physical = directory.parent
            for top in ("Android", "Control", "META"):
                origin = work / top
                if not origin.exists():continue
                for source in origin.rglob("*"):
                    if not source.is_file():continue
                    rel = source.relative_to(work)
                    target = physical / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        with source.open("rb") as inp, target.open("xb") as out:
                            shutil.copyfileobj(inp,out,1024*1024)
                            out.flush();os.fsync(out.fileno())
                    except FileExistsError:
                        # Never overwrite an existing evidence file.
                        if digest(target) != digest(source):
                            raise ValueError("PHYSICAL_EVIDENCE_CONFLICT:"+str(rel))
            if not (physical / "META" / "SHA256SUMS.txt").is_file():
                raise ValueError("SESSION_PHYSICAL_INVENTORY_MISSING")
            outcome = {"session": sid, "game": meta.get("game"), "game_title":meta.get("game_title",meta.get("game")), "archive": str(archive), "sha256": sha, "archive_integrity": "PASS", "entries": count, "captured_external": len(entries), "warnings": warnings, "coverage": result["collection_completeness"]}
            atomic_json(self.host / ("ARCHIVED-" + sid + ".json"), outcome)
            return outcome
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def _place_device(self, work):
        (work / "Android").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.host / "android-device.json", work / "Android" / "device.json")

    def completed_current_sessions(self, now=None, min_age_seconds=22):
        """Host safety net only AFTER valid completed Windows sessions.

        Refuse to stop while any session is active/unarchived or token mismatches.
        No handle/kill is ever used on target games. The worker receives only
        its own cooperative stop.request in the run's private control folder.
        """
        now = time.time() if now is None else now
        if not self.archived or 'NO_GAME_DETECTED' in self.archived or not self.seen:
            return None
        if set(self.seen) != set(self.archived):
            return None
        maximum_end = 0.0
        sessions=[]
        for sid in sorted(self.seen):
            meta_path = self.archives / sid / 'Windows' / 'session.json'
            try:
                meta=json.loads(meta_path.read_text(encoding='utf8'))
                if not (meta.get('token')==self.token and meta.get('run')==self.run
                        and meta.get('session')==sid and meta.get('state')=='finished'
                        and meta.get('reason') in ('PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN','GAME_PROCESS_EXITED')):
                    return None
                ended=dt.datetime.fromisoformat(meta['ended_at'].replace('Z','+00:00')).timestamp()
                if ended > now + 2:return None
            except (OSError,ValueError,KeyError,TypeError):return None
            maximum_end=max(maximum_end,ended)
            sessions.append(sid)
        age=now-maximum_end
        if age < min_age_seconds:return None
        return {'sessions':sessions,'age_since_last_windows_game_exit_seconds':round(age,2),
                'proof':'WINDOWS_SESSION_FINISHED_RUN_TOKEN_MATCH_AND_ARCHIVE_EXISTS',
                'not_a_process_exit_status':True}

    def request_worker_stop_after_game(self, evidence):
        """A bounded cooperative request ONLY for this finished run's worker."""
        path=self.win/'stop.request'
        try:
            path.parent.mkdir(parents=True,exist_ok=True)
            with path.open('x',encoding='utf-8') as f:
                f.write('HOST_COMPLETED_SESSION_ONLY\nTOKEN='+self.token+'\n')
                f.flush();os.fsync(f.fileno())
            self.log('host_postgame_worker_stop_requested',evidence)
        except FileExistsError:
            self.log('host_postgame_stop_already_requested',evidence)
        except OSError as exc:
            self.log('host_postgame_stop_request_failed',{'error':type(exc).__name__})

    def diagnostics_only(self):
        if self.archived: return
        work = self.host / (".build-no-game-" + secrets.token_hex(5))
        work.mkdir(mode=0o700)
        try:
            (work / "Android").mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.host / "android-device.json",work / "Android" / "device.json")
            for filename in ("collector-events.jsonl", "host-live.json"):
                if (self.host / filename).is_file():
                    shutil.copyfile(self.host / filename,work / "Android" / filename)
            (work / "Control").mkdir(parents=True, exist_ok=True)
            for filename in ("run-status.json", "ui-health.json", "ui-timeline.jsonl", "startup-timeline.jsonl", "observer.log", "observer-errors.jsonl", "started.ok", "finished.ok", "bat-launch.txt", "bat-exit.txt", "probe-stderr.txt", "probe-stdout.txt", "controller-events.jsonl", "gui-read-events.jsonl", "worker-stderr.txt", "gui-stderr.txt", "worker-stdout.txt", "gui-stdout.txt", "latest-session.json", "stop.request"):
                if (self.win / filename).is_file():
                    shutil.copyfile(self.win / filename,work / "Control" / filename)
            bat_rc = "NOT_REPORTED"
            if (self.win / "bat-exit.txt").is_file():
                try: bat_rc = (self.win / "bat-exit.txt").read_text(errors="replace").strip().replace("EXIT_CODE=", "")
                except OSError: bat_rc = "UNREADABLE"
            crash_code = "ACCESS_VIOLATION_0xC0000005" if bat_rc in ("-1073741819", "3221225477") else "UNDETERMINED"
            atomic_json(work / "META" / "COLLECTION-REPORT.json", {"schema": SCHEMA, "run": self.run, "token":self.token, "session": "NO_GAME_DETECTED", "collection_completeness": "PARTIAL", "archive_integrity": "VALIDATION_PENDING_AT_ASSEMBLY", "game_exit_cause": "NOT_APPLICABLE", "reason": "NO_COMPLETED_WINDOWS_GAME_SESSION", "windows_process_exit_code":bat_rc, "windows_process_exit_class":crash_code, "crash_root_cause":"NOT_PROVEN", "no_target_termination":True})
            archive,sha,n=pack_immutable(work,self.archives,"NO-GAME-"+self.run+"-GPU-Lab-Logs.zip")
            result={"session":"NO_GAME_DETECTED", "archive":str(archive), "sha256":sha, "entries":n,"archive_integrity":"PASS", "coverage":"PARTIAL"}
            atomic_json(self.host / "ARCHIVED-NO-GAME.json",result)
            self.archived.add("NO_GAME_DETECTED")
            self.log("diagnostics_archive_created",result)
        finally:
            shutil.rmtree(work,ignore_errors=True)

    def finalize(self):
        for directory in self.get_sessions():
            if directory.parent.name in self.archived: continue
            try:
                meta = json.loads((directory / "session.json").read_text())
                if meta.get("token") != self.token or meta.get("run") != self.run:continue
                forced = meta.get("state") != "finished"
                self.log("finalizing_unarchived_session", {"session": directory.parent.name, "partial": forced})
                if not forced and self.grace > 0:
                    time.sleep(min(self.grace, 8))
                b = self.seen.get(directory.parent.name, {}).get("baseline", self.baseline)
                outcome = self.archive_session(directory, meta, b, forced_partial=forced)
                self.archived.add(directory.parent.name)
                self.log("partial_archive_created", outcome)
            except Exception as exc:
                self.log("partial_archive_failed", {"session": directory.parent.name, "error": str(exc)})
        if self.diag is not None:
            try:self.diag.finish()
            except Exception as exc:self.log('diag_shutdown_error', {'error':type(exc).__name__})
        try:
            self.diagnostics_only()
        except Exception as exc:
            self.log("diagnostics_archive_failed", {"type": type(exc).__name__, "error": str(exc)})
            raise



def create_return(collector, reason):
    """Publish exactly one diagnostic RETURN per runner to Download/Log Termux.

    Game ZIPs are never copied into RETURN. Evidence of their names, size and SHA
    is recorded, so a maintainer can request the full game ZIP from GPU-Lab/Logs.
    Each RETURN is exclusive and immutable, CRC checked, independent of
    pack_immutable and does not require hardlink support on Android/FUSE.
    """
    collector.returns.mkdir(parents=True, exist_ok=True)
    dest = collector.returns / ("GPU-Lab-RETURN-" + collector.run + ".zip")
    if dest.exists():
        with zipfile.ZipFile(dest) as z:
            if z.testzip() is not None: raise ValueError("EXISTING_RETURN_CORRUPTED")
        collector.return_path = dest
        return dest
    archive_rows = []
    errors = []
    for metadata in sorted(collector.host.glob("ARCHIVED-*.json")):
        try:
            status = json.loads(metadata.read_text(encoding="utf-8"))
            location = Path(status["archive"])
            # Only refer to game archives actually inside the GPU-Lab/Logs
            # directory. Do not accept paths injected into metadata.
            if location.parent.resolve() != collector.archives.resolve():
                errors.append({"file": metadata.name, "error": "ARCHIVE_OUTSIDE_LOGS"})
                continue
            if not location.is_file():
                errors.append({"file": metadata.name, "error": "SESSION_ZIP_MISSING"})
                continue
            actual_sha = digest(location)
            if actual_sha != status.get("sha256"):
                errors.append({"file": metadata.name, "error": "SESSION_ZIP_SHA_MISMATCH"})
                continue
            with zipfile.ZipFile(location) as z:
                if z.testzip() is not None:
                    errors.append({"file": metadata.name, "error": "SESSION_ZIP_CRC_FAILED"})
                    continue
            archive_rows.append({
                "session": status.get("session"),
                "file": location.name,
                "relative_path": "GPU-Lab/Logs/" + location.name,
                "sha256": actual_sha,
                "bytes": location.stat().st_size,
                "integrity": status.get("archive_integrity", "UNPROVEN"),
                "coverage": status.get("coverage", "UNPROVEN"),
            })
        except (OSError, ValueError, KeyError, zipfile.BadZipFile) as e:
            errors.append({"file": metadata.name, "error": type(e).__name__, "detail": str(e)[:500]})

    # Partial or no-game state is preserved in RETURN; never claim that an
    # archive's CRC automatically proves that capture was complete.
    summary = {
        "schema": "gpu-lab-return-v1", "run": collector.run,
        "created_at": utc(), "trigger": reason,
        "role": "TERMUX_DIAGNOSTIC_RETURN_ONLY",
        "game_zip_folder": "GPU-Lab/Logs",
        "game_zips_embedded": False,
        "archives": archive_rows,
        "archive_reference_errors": errors,
        "sessions_reported": len(archive_rows),
        "assessment": "ARCHIVE_VALIDITY_DOES_NOT_PROVE_GAME_COMPLETENESS",
    }
    # Limit captured diagnostics, never indiscriminately include game files.
    selected = []
    omitted = []
    used = 0
    max_total = 32 * 1024 * 1024
    max_file = 8 * 1024 * 1024
    for category, folder in (("Android", collector.host), ("Control", collector.win)):
        if not folder.is_dir(): continue
        for base, dirs, filenames in os.walk(folder, followlinks=False):
            dirs[:] = [x for x in dirs if not x.startswith(".build-") and not Path(base, x).is_symlink()]
            for name in sorted(filenames):
                path = Path(base) / name
                if path.is_symlink() or not path.is_file(): continue
                arcname = category + "/" + path.relative_to(folder).as_posix()
                try:
                    size = path.stat().st_size
                    if size > max_file or size + used > max_total:
                        omitted.append({"file": arcname, "reason": "RETURN_SIZE_LIMIT"})
                        continue
                    selected.append((arcname,path))
                    used += size
                except OSError as e:
                    omitted.append({"file": arcname, "reason": type(e).__name__})
    summary["diagnostic_omissions"] = omitted
    temp = collector.returns / ("." + dest.name + ".partial-" + secrets.token_hex(6))
    published = False
    try:
        with temp.open("xb") as file:
            with zipfile.ZipFile(file, "w", zipfile.ZIP_DEFLATED, compresslevel=3, allowZip64=True) as z:
                for name, path in selected:
                    try: z.write(path, name)
                    except OSError as e:
                        omitted.append({"file": name, "reason": type(e).__name__})
                z.writestr("META/RETURN-INDEX.json", json.dumps(summary, ensure_ascii=False, indent=2))
            file.flush()
            os.fsync(file.fileno())
        with zipfile.ZipFile(temp) as z:
            if z.testzip() is not None: raise ValueError("RETURN_CRC_FAILED")
        # Android emulated storage may lack os.link/hardlinks; xb does not
        # overwrite, even when a previous RETURN already exists.
        with temp.open("rb") as src, dest.open("xb") as out:
            for part in iter(lambda:src.read(1024*1024),b""):
                out.write(part)
            out.flush()
            os.fsync(out.fileno())
        published = True
        with zipfile.ZipFile(dest) as z:
            if z.testzip() is not None: raise ValueError("RETURN_POST_PUBLISH_CRC_FAILED")
        collector.return_path = dest
        return dest
    except FileExistsError:
        with zipfile.ZipFile(dest) as z:
            if z.testzip() is not None: raise ValueError("EXISTING_RETURN_CORRUPTED")
        collector.return_path = dest
        return dest
    except BaseException:
        if published: dest.unlink(missing_ok=True)
        raise
    finally:
        temp.unlink(missing_ok=True)


def emergency_archive(collector, reason):
    """Independent last-resort RETURN; no hardlinks, no pack_immutable dependency.

    A partial diagnostics archive is preferable to losing every log when the
    normal archiver raises. Never claim game coverage or session success.
    """
    collector.returns.mkdir(parents=True, exist_ok=True)
    # Limit emergency capture so a bad shared directory cannot exhaust space.
    max_total = 64 * 1024 * 1024
    max_file = 16 * 1024 * 1024
    captured = 0
    missed = []
    for _ in range(8):
        output = collector.returns / ("GPU-Lab-RETURN-EMERGENCY-" + collector.run + "-" + secrets.token_hex(5) + ".zip")
        try:
            with output.open("xb") as fp:
                with zipfile.ZipFile(fp, "w", zipfile.ZIP_DEFLATED, compresslevel=3, allowZip64=True) as z:
                    for root_name, root in (("Android", collector.host), ("Control", collector.win)):
                        if not root.is_dir(): continue
                        for base, folders, filenames in os.walk(root, followlinks=False):
                            folders[:] = [name for name in folders if not Path(base, name).is_symlink() and not name.startswith(".build-")]
                            for name in sorted(filenames):
                                file = Path(base, name)
                                if file.is_symlink(): continue
                                arc_name = root_name + "/" + file.relative_to(root).as_posix()
                                try:
                                    size = file.stat().st_size
                                    if size > max_file or captured + size > max_total:
                                        missed.append({"path": arc_name, "reason": "EMERGENCY_SIZE_LIMIT"})
                                        continue
                                    z.write(file, arc_name)
                                    captured += size
                                except OSError as exc:
                                    missed.append({"path": arc_name, "reason": type(exc).__name__})
                    z.writestr("META/EMERGENCY-STATUS.json", json.dumps({
                        "schema": "gpu-lab-emergency-v1", "run": collector.run,
                        "status": "PARTIAL_NOT_A_SUCCESS", "reason": str(reason)[:500],
                        "file_bytes": captured, "unavailable": missed,
                        "captured_at": utc(), "session_validation": "NOT_PROVEN"
                    }, ensure_ascii=False, indent=2))
                fp.flush(); os.fsync(fp.fileno())
            with zipfile.ZipFile(output) as z:
                if z.testzip() is not None: raise ValueError("EMERGENCY_ZIP_CRC_FAILED")
            return output
        except FileExistsError:
            continue
        except BaseException:
            output.unlink(missing_ok=True)
            raise
    raise FileExistsError("EMERGENCY_ARCHIVE_NAME_RETRIES_EXHAUSTED")


def guarded_finalize(collector, reason):
    collector._finalized_attempted = True
    finalize_error = None
    try:
        collector.finalize()
    except Exception as exc:
        finalize_error = exc
        print("GPU_LAB_NORMAL_FINALIZE_FAILED=" + repr(exc), file=sys.stderr, flush=True)
    # Generate diagnostic RETURN for BOTH successful and partial runs.
    # This step must not depend on the normal per-game ZIP archiver.
    try:
        p = create_return(collector, reason + ("; FINALIZE_FAILED=" + repr(finalize_error) if finalize_error else ""))
        print("GPU_LAB_RETURN=" + str(p), flush=True)
    except Exception as exc:
        print("GPU_LAB_RETURN_FAILED=" + repr(exc), file=sys.stderr, flush=True)
        try:
            p = emergency_archive(collector, str(reason) + "; RETURN=" + repr(exc))
            print("GPU_LAB_EMERGENCY_RETURN=" + str(p), file=sys.stderr, flush=True)
        except Exception as last:
            print("GPU_LAB_EMERGENCY_RETURN_FAILED=" + repr(last), file=sys.stderr, flush=True)
            if finalize_error is None: raise
    if finalize_error is not None: raise finalize_error


def run_loop(args):
    c = Collector(args.root, args.run, args.token, args.documents, args.log_root, args.grace)
    c.log("windows_bat_required", {"path": str(args.bat) if args.bat else "UNAVAILABLE"})
    print("GPU LAB — ESPERANDO INÍCIO DO EXECUTAR-GPU-LAB-<RUN>.bat", flush=True)
    previous = time.monotonic()
    last_stale_report = 0.0
    postgame_deadline = None
    try:
        while True:
            try:
                c.tick()
            except (OSError, ValueError) as exc:
                c.log("collector_tick_partial_error", {"type": type(exc).__name__, "error": str(exc)})
            if (c.win / "finished.ok").is_file():
                if (c.win / "finished.ok").read_text(errors="replace").strip() == args.token:
                    c.log("windows_observer_finished", {})
                    guarded_finalize(c,"WINDOWS_FINISHED")
                    break
            if (c.win / "bat-exit.txt").is_file():
                c.log("windows_batch_exit_without_normal_final_marker", {"bat_exit":(c.win / "bat-exit.txt").read_text(errors="replace")[:200]})
                guarded_finalize(c,"BAT_EXIT")
                break
            # Host-side completion fallback: when Windows has proved the game
            # disappeared and the matching ZIP exists, do not wait forever for
            # a crashed/hung GUI or its controller. NEVER infer game exit from
            # absence of heartbeat. A fresh running session vetoes the fallback.
            safe = c.completed_current_sessions()
            if safe is not None and postgame_deadline is None:
                c.request_worker_stop_after_game(safe)
                postgame_deadline = time.monotonic() + 7
            if postgame_deadline is not None and safe is None:
                c.log('host_postgame_fallback_cancelled_active_or_unverified_session', {})
                postgame_deadline = None
            if postgame_deadline is not None and time.monotonic() >= postgame_deadline:
                c.log('host_postgame_complete_without_windows_final_marker', safe)
                guarded_finalize(c,'HOST_POSTGAME_CONFIRMED_GAME_EXIT_WINDOWS_FINAL_MARKER_MISSING')
                break
            # Do not wait forever for a dead/unresponsive Windows observer.
            # This is a diagnostic PARTIAL outcome, not a claim the game exited.
            status_path = c.win / "run-status.json"
            if (c.win / "started.ok").is_file() and status_path.is_file():
                try:
                    stale_s = time.time() - status_path.stat().st_mtime
                    if stale_s > 90:
                        c.log("windows_observer_heartbeat_stale", {"seconds": round(stale_s), "result": "PARTIAL_UNVERIFIED_GAME_STATE"})
                        guarded_finalize(c,"WINDOWS_OBSERVER_STALE_NO_GAME_EXIT_PROOF")
                        break
                except OSError: pass
            if time.monotonic() - previous > 25:
                print("GPU_LAB_STATUS=OBSERVANDO | ZIPs=" + str(len(c.archived)) + " | GUI=" + ("ABERTA" if (c.win / "started.ok").exists() else "AGUARDANDO"), flush=True)
                previous = time.monotonic()
            time.sleep(1)
    except KeyboardInterrupt:
        c.log("termux_interrupted_observer_not_terminated", {})
        guarded_finalize(c,"KEYBOARD_INTERRUPT")
    except Exception as exc:
        c.log("host_collector_exception", {"type": type(exc).__name__, "error": str(exc)})
        if not getattr(c, "_finalized_attempted", False):
            guarded_finalize(c,"EXCEPTION:"+str(exc))
        raise
    print("GPU_LAB_SESSIONS_ARCHIVED=" + str(len(c.archived)), flush=True)
    print("GPU_LAB_ARCHIVES=" + str(c.archives), flush=True)
    print("GPU_LAB_RETURNS=" + str(c.returns), flush=True)
    print("TERMUX_SESSION=PRESERVADA", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True)
    p.add_argument("--run", required=True)
    p.add_argument("--token", required=True)
    p.add_argument("--documents", default="/sdcard/Documents")
    p.add_argument("--log-root", action="append", default=[])
    p.add_argument("--bat")
    p.add_argument("--grace", type=float, default=4)
    a = p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,128}", a.run): p.error("invalid run id")
    if not re.fullmatch(r"[0-9a-f]{32,128}", a.token): p.error("invalid token")
    run_loop(a)

if __name__ == "__main__": main()
