//go:build windows

package main

import (
	"bufio"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type dashboard struct {
	Fields                             map[string]string
	CPUHistory, GPUHistory, FPSHistory []float64
	Running, Done, Deep                bool
	Status                             string
}

var state = struct {
	sync.RWMutex
	dashboard
}{dashboard: dashboard{Fields: map[string]string{}, Status: "AGUARDANDO JOGO"}}
var fieldKeys = []string{"GAME", "PID", "PATH", "PRODUCT", "EXE_SHA256", "API", "GPU", "GPU_SOURCE", "DRIVER", "VULKAN", "PANVK", "DXVK", "VKD3D", "WRAPPER", "WINE", "TRANSLATOR", "LAUNCHER", "ANDROID", "MODEL", "SOC", "CPU_ARCH", "CPU_CORES", "KERNEL", "KBASE", "GOVERNOR", "FPS", "AVG_FPS", "LOW_1", "LOW_01", "FRAME_MS", "P95", "P99", "P999", "CPU_GAME", "RAM_GAME", "CPU_HOST", "RAM_HOST", "GPU_BUSY", "GPU_FREQ", "GPU_FREQ_MIN", "GPU_FREQ_MAX", "GPU_TEMP", "SOC_TEMP", "VK_MEM", "VK_PEAK", "FRAMES", "SUBMITS", "EVENTS", "DROPS", "SESSION", "LOGS", "SOURCE_FPS", "STATE", "ROUND", "DRIVER_SO_SHA256"}

func init() {
	for _, k := range fieldKeys {
		state.Fields[k] = "N/D"
	}
	state.Fields["STATE"] = "AGUARDANDO"
}
func update(f func(*dashboard)) { state.Lock(); f(&state.dashboard); state.Unlock(); repaint() }
func snap() dashboard {
	state.RLock()
	defer state.RUnlock()
	d := state.dashboard
	d.Fields = make(map[string]string, len(state.Fields))
	for k, v := range state.Fields {
		d.Fields[k] = v
	}
	d.CPUHistory = append([]float64(nil), state.CPUHistory...)
	d.GPUHistory = append([]float64(nil), state.GPUHistory...)
	d.FPSHistory = append([]float64(nil), state.FPSHistory...)
	return d
}
func put(k, v string) {
	if v != "" {
		update(func(d *dashboard) { d.Fields[k] = v })
	}
}
func history(key string, value float64) {
	if value < 0 || value > 100 {
		return
	}
	update(func(d *dashboard) {
		if key == "CPU" {
			d.CPUHistory = append(d.CPUHistory, value)
			if len(d.CPUHistory) > 80 {
				d.CPUHistory = d.CPUHistory[len(d.CPUHistory)-80:]
			}
		}
	})
}

var stopRequested = make(chan struct{})
var currentSession string
var markerMu sync.Mutex
var sessionPath atomic.Value
var markerNumber atomic.Uint64

func recordMarker(shot bool) {
	markerMu.Lock()
	defer markerMu.Unlock()
	v := sessionPath.Load()
	if v == nil {
		return
	}
	dir := v.(string)
	if dir == "" {
		return
	}
	n := markerNumber.Add(1)
	name := fmt.Sprintf("marker-%04d-%d-%s", n, time.Now().UnixNano(), tokenHex())
	payload := map[string]any{"time": time.Now().Format(time.RFC3339Nano), "kind": "marker", "session": currentSession}
	if shot {
		_ = os.MkdirAll(filepath.Join(dir, "screenshots"), 0700)
		payload["kind"] = "screenshot"
		err := desktopScreenshot(filepath.Join(dir, "screenshots", name+".bmp"))
		if err != nil {
			payload["error"] = err.Error()
		}
	}
	_ = writeJSON(filepath.Join(dir, "markers", name+".json"), payload)
}
func recordDeepToggle() {
	// Deep-capture is a shared control request, not just a GUI-local toggle.
	if controlStopFile != "" {
		marker := filepath.Join(filepath.Dir(controlStopFile), "deep-capture.flag")
		if snap().Deep {
			_ = atomicWrite(marker, []byte("ON\n"))
		} else {
			_ = os.Remove(marker)
		}
	}
	markerMu.Lock()
	defer markerMu.Unlock()
	v := sessionPath.Load()
	if v == nil {
		return
	}
	dir := v.(string)
	if dir == "" {
		return
	}
	writeEvent(filepath.Join(dir, "events.jsonl"), "deep_capture_changed", map[string]any{"enabled": snap().Deep, "mode": "HIGHER_RATE_PROCESS_TELEMETRY_ONLY"})
}
func atomicWrite(path string, data []byte) error {
	if e := os.MkdirAll(filepath.Dir(path), 0700); e != nil {
		return e
	}
	var b [8]byte
	_, _ = rand.Read(b[:])
	tmp := path + ".tmp-" + hex.EncodeToString(b[:])
	f, e := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if e != nil {
		return e
	}
	_, e = f.Write(data)
	if e == nil {
		e = f.Sync()
	}
	closeErr := f.Close()
	if e == nil {
		e = closeErr
	}
	if e != nil {
		_ = os.Remove(tmp)
		return e
	}
	e = os.Rename(tmp, path)
	if e != nil {
		_ = os.Remove(tmp)
	}
	return e
}
func writeJSON(path string, value any) error {
	b, e := json.MarshalIndent(value, "", "  ")
	if e != nil {
		return e
	}
	return atomicWrite(path, append(b, '\n'))
}
func timestamp() string { return time.Now().Format(time.RFC3339Nano) }

// Display names are grounded in the executable's installed directory; the exact EXE
// is retained independently in the manifest. A launcher or generic directory
// never becomes a fabricated product name.
func titleFromExecutable(exePath, exe string) string {
	normalized := strings.ReplaceAll(exePath, `\`, "/")
	folder := strings.TrimSpace(path.Base(path.Dir(normalized)))
	if folder == "" || folder == "." || folder == "/" || strings.Contains(folder, ":") || strings.EqualFold(folder, "bin") || strings.EqualFold(folder, "x64") || strings.EqualFold(folder, "game") || strings.EqualFold(folder, "games") {
		return exe
	}
	if len([]rune(folder)) > 70 {
		folder = string([]rune(folder)[:70])
	}
	return folder
}
func safeExeName(n string) string {
	// Keep the exact detected filename extension (.exe), never use a game directory as identity.
	if len(n) > 70 {
		n = n[:70]
	}
	return regexp.MustCompile(`[^A-Za-z0-9_.-]+`).ReplaceAllString(n, "-")
}
func safeName(n string) string {
	n = strings.TrimSuffix(n, filepath.Ext(n))
	n = regexp.MustCompile(`[^A-Za-z0-9_-]+`).ReplaceAllString(n, "-")
	if len(n) > 60 {
		n = n[:60]
	}
	if n == "" {
		return "unknown"
	}
	return n
}
func writeEvent(path, typ string, more map[string]any) {
	o := map[string]any{"at": timestamp(), "type": typ}
	for k, v := range more {
		o[k] = v
	}
	b, e := json.Marshal(o)
	if e != nil {
		return
	}
	f, e := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if e != nil {
		return
	}
	defer f.Close()
	_, _ = f.Write(append(b, '\n'))
}
func filteredEnvironment() map[string]string {
	out := map[string]string{}
	allowed := []string{"DXVK_", "VKD3D_", "MESA_", "WINEDEBUG", "WINEPREFIX", "WINEESYNC", "WINEFSYNC", "BOX64_", "FEX_", "GALLIUM_", "ZINK_", "VK_", "PROTON_", "mesa_"}
	for _, pair := range os.Environ() {
		p := strings.SplitN(pair, "=", 2)
		if len(p) != 2 {
			continue
		}
		for _, prefix := range allowed {
			if strings.HasPrefix(strings.ToUpper(p[0]), strings.ToUpper(prefix)) {
				if len(p[1]) < 1000 {
					out[p[0]] = p[1]
				}
				break
			}
		}
	}
	return out
}
func fileHash(path string) string {
	f, e := os.Open(path)
	if e != nil {
		return "UNAVAILABLE"
	}
	defer f.Close()
	h := sha256.New()
	if _, e = io.Copy(h, f); e != nil {
		return "UNAVAILABLE"
	}
	return hex.EncodeToString(h.Sum(nil))
}
func tokenHex() string { var b [16]byte; _, _ = rand.Read(b[:]); return hex.EncodeToString(b[:]) }

type liveConfig struct{ Root, Run, Token string }
type liveStatus struct {
	Schema, Run, Token, State, At, Reason string
	Sessions                              int
	ObserverOnly                          bool
}

func writeRunStatus(dir string, c liveConfig, stage, reason string, sessions int) {
	_ = writeJSON(filepath.Join(dir, "run-status.json"), liveStatus{Schema: "gpu-lab-run-v1", Run: c.Run, Token: c.Token, State: stage, At: timestamp(), Reason: reason, Sessions: sessions, ObserverOnly: true})
}
func appendUIHistory(dir string, d dashboard) {
	// Independent, per-run trace of what the collector *wanted* to display and
	// what Win32 reported back, including during recording (not just at exit).
	writeEvent(filepath.Join(dir, "ui-timeline.jsonl"), "ui_snapshot", map[string]any{
		"intended_state": d.Fields["STATE"], "running": d.Running,
		"intended_game": d.Fields["GAME"], "intended_pid": d.Fields["PID"],
		"intended_cpu_game": d.Fields["CPU_GAME"], "intended_ram_game": d.Fields["RAM_GAME"],
		"native_status_readback": nativeStatusString(), "native_pid_readback": nativePIDString(),
		"native_title_readback": nativeWindowTitle(), "native_updates": guiNativeUpdates.Load(),
		"native_failures": guiNativeFailures.Load(), "native_matches": guiNativeMatched.Load(),
		"gui_lifecycle": guiLifecycle.Load(), "gui_timers": guiTimerCount.Load(), "gui_safe_mode": guiSafeMode,
	})
}
func writeUIHealth(dir string) {
	d := snap()
	appendUIHistory(dir, d)
	_ = writeJSON(filepath.Join(dir, "ui-health.json"), map[string]any{"time": timestamp(), "native_status": nativeStatusString(), "native_pid": nativePIDString(), "native_title": nativeWindowTitle(), "gui_lifecycle": guiLifecycle.Load(), "paints": guiPaintCount.Load(), "timers": guiTimerCount.Load(), "native_updates": guiNativeUpdates.Load(), "native_failures": guiNativeFailures.Load(), "native_matches": guiNativeMatched.Load(), "gui_safe_mode": guiSafeMode, "collector_state": d.Fields["STATE"], "collector_pid": d.Fields["PID"]})
}

var controlStopFile string

func stopFlag() bool {
	if controlStopFile != "" {
		if _, e := os.Stat(controlStopFile); e == nil {
			return true
		}
	}
	select {
	case <-stopRequested:
		return true
	default:
		return false
	}
}

// Before the worker is ready, Winlator may already have launched a game. A
// carefully bounded fallback can attach to such an existing game without
// selecting Wine background services or GPU Lab's own child processes.
func preexistingGameCandidate(p procInfo, exePath string) bool {
	n := strings.ToLower(strings.ReplaceAll(exePath, "/", "\\"))
	if ignoredProcess(p.Name) || strings.Contains(n, "\\windows\\") || strings.Contains(n, "\\system32\\") {
		return false
	}
	for _, hint := range []string{"\\jogos\\", "\\games\\", "\\steamapps\\", "\\gog games\\", "\\need for speed\\", "\\game\\"} {
		if strings.Contains(n, hint) {
			return true
		}
	}
	// Winlator commonly maps the user's game directory to D:. Require RAM
	// evidence, so an unrelated small helper is not silently selected.
	if len(n) > 2 && (strings.HasPrefix(n, "d:\\") || strings.HasPrefix(n, "e:\\") || strings.HasPrefix(n, "f:\\")) {
		_, mem, ok := processUsage(p.PID)
		return ok && mem >= 96
	}
	return false
}
func selectGame(baseline map[uint32]bool, seen map[string]bool, last *uint32, stability *int, allowPreexisting bool) (procInfo, string, string) {
	ps := enumerateProcesses()
	type candidate struct {
		p           procInfo
		path, key   string
		birth       uint64
		wasBaseline bool
	}
	all := make([]candidate, 0, 8)
	for pid, p := range ps {
		if pid <= 8 || pid == selfPID() || ignoredProcess(p.Name) {
			continue
		}
		exePath := processPath(pid)
		if exePath == "" || !pidAlive(pid) {
			continue
		}
		wasBaseline := baseline[pid]
		if wasBaseline && (!allowPreexisting || !preexistingGameCandidate(p, exePath)) {
			continue
		}
		birth := processBirth(pid)
		key := fmt.Sprintf("%d|%s|%d", pid, strings.ToLower(exePath), birth)
		if seen[key] {
			continue
		}
		all = append(all, candidate{p: p, path: exePath, key: key, birth: birth, wasBaseline: wasBaseline})
	}
	if len(all) == 0 {
		*last = 0
		*stability = 0
		return procInfo{}, "", ""
	}
	sort.Slice(all, func(i, j int) bool {
		// Prefer a newly launched game over previously running candidates.
		if all[i].wasBaseline != all[j].wasBaseline {
			return !all[i].wasBaseline
		}
		if all[i].birth != all[j].birth {
			return all[i].birth > all[j].birth
		}
		return all[i].p.PID > all[j].p.PID
	})
	c := all[0]
	if *last == c.p.PID {
		*stability++
	} else {
		*last = c.p.PID
		*stability = 1
	}
	if *stability < 3 {
		return procInfo{}, "", ""
	}
	return c.p, c.key, c.path
}
func writeSessionEnd(dir string, meta map[string]any, reason string) {
	meta["state"] = "finished"
	meta["reason"] = reason
	meta["ended_at"] = timestamp()
	meta["game_exit_classification"] = "UNDETERMINED"
	_ = writeJSON(filepath.Join(dir, "session.json"), meta)
}
func observeSession(runDir string, c liveConfig, p procInfo, exePath string, index int) (bool, error) {
	var entropy [7]byte
	_, _ = rand.Read(entropy[:])
	sid := fmt.Sprintf("%s-%s-p%d-%s", safeExeName(p.Name), time.Now().Format("20060102-150405"), p.PID, hex.EncodeToString(entropy[:]))
	sessionRoot := filepath.Join(c.Root, "Logs", sid)
	// Namespace reservation is exclusive: never reuse or overwrite a past session.
	if e := os.Mkdir(sessionRoot, 0700); e != nil {
		return false, e
	}
	dir := filepath.Join(sessionRoot, "Windows")
	if e := os.Mkdir(dir, 0700); e != nil {
		return false, e
	}
	markerMu.Lock()
	currentSession = sid
	sessionPath.Store(dir)
	markerMu.Unlock()
	defer func() { markerMu.Lock(); sessionPath.Store(""); currentSession = ""; markerMu.Unlock() }()
	displayTitle := titleFromExecutable(exePath, p.Name)
	_ = writeJSON(filepath.Join(runDir, "latest-session.json"), map[string]any{"session": sid, "token": c.Token, "run": c.Run})
	meta := map[string]any{"game_title": displayTitle, "schema": "gpu-lab-session-v1", "run": c.Run, "token": c.Token, "session": sid, "index": index, "pid": p.PID, "game": p.Name, "path": exePath, "state": "active", "started_at": timestamp(), "observer_only": true, "process_termination_calls": false, "fps": "N/D", "fps_source": "NO_INJECTED_VULKAN_LAYER"}
	if e := writeJSON(filepath.Join(dir, "session.json"), meta); e != nil {
		return false, e
	}
	_ = writeJSON(filepath.Join(dir, "environment.json"), map[string]any{"source": "GPU_LAB_PROBE_ENV_NOT_GAME_ENV", "variables": filteredEnvironment()})
	_ = writeJSON(filepath.Join(dir, "process.json"), map[string]any{"pid": p.PID, "ppid": p.PPID, "exe": p.Name, "image_path": exePath, "sha256": "UNPROVEN_AVOID_GAME_FILE_IO"})
	_ = os.MkdirAll(filepath.Join(dir, "telemetry"), 0700)
	telemetry, e := os.OpenFile(filepath.Join(dir, "telemetry", "game.csv"), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if e != nil {
		return false, e
	}
	defer telemetry.Close()
	fmt.Fprintln(telemetry, "time,cpu_percent_one_core,ram_mb,pid_alive,source")
	timeline := filepath.Join(dir, "events.jsonl")
	writeEvent(timeline, "game_detected", map[string]any{"pid": p.PID, "exe": p.Name, "path": exePath})
	update(func(d *dashboard) {
		d.Running = true
		d.Done = false
		d.Status = "GRAVANDO"
		d.Fields["STATE"] = "GRAVANDO"
		d.Fields["GAME"] = displayTitle
		d.Fields["PRODUCT"] = p.Name
		d.Fields["PID"] = strconv.FormatUint(uint64(p.PID), 10)
		d.Fields["PATH"] = exePath
		d.Fields["SESSION"] = sid
		d.Fields["FPS"] = "N/D"
		d.Fields["SOURCE_FPS"] = "N/D — sem fonte confiável"
	})
	// GUI writes its own health; worker has no UI callbacks.
	var previousCPU float64
	prevTime := time.Now()
	first := true
	missing := 0
	lastPulse := time.Time{}
	for {
		if stopFlag() {
			writeEvent(timeline, "observer_stopped_game_unaffected", nil)
			writeSessionEnd(dir, meta, "OBSERVER_STOPPED_GAME_NOT_TOUCHED")
			return false, nil
		}
		// A Wine process handle may remain waitable after its process
		// disappears from the Toolhelp enumeration. Require three
		// consecutive misses of either independent observation.
		alive := pidAlive(p.PID)
		ps := enumerateProcesses()
		listed, present := ps[p.PID]
		snapshotMissing := len(ps) != 0 && (!present || !strings.EqualFold(listed.Name, p.Name))
		if !alive || snapshotMissing {
			missing++
		} else {
			missing = 0
		}
		cpu, ram, ok := processUsage(p.PID)
		pct := "N/D"
		ramStr := "N/D"
		now := time.Now()
		if ok {
			ramStr = fmt.Sprintf("%.1fMB", ram)
			if !first {
				dt := now.Sub(prevTime).Seconds()
				if dt > 0 {
					v := (cpu - previousCPU) / dt * 100
					if v >= 0 && v <= 10000 {
						pct = fmt.Sprintf("%.1f", v)
						history("CPU", v)
					}
				}
			}
			previousCPU = cpu
			prevTime = now
			first = false
		}
		fmt.Fprintf(telemetry, "%s,%s,%s,%t,READ_ONLY_WINDOWS_API\n", now.Format(time.RFC3339Nano), pct, ramStr, alive)
		_ = telemetry.Sync()
		update(func(d *dashboard) {
			d.Fields["CPU_GAME"] = pct + func() string {
				if pct == "N/D" {
					return ""
				}
				return "%"
			}()
			d.Fields["RAM_GAME"] = ramStr
		})
		if time.Since(lastPulse) > 5*time.Second {
			writeRunStatus(runDir, c, "capturing", "OBSERVING_GAME_NO_CONTROL", index)
			// GUI writes its own health; worker must not overwrite UI evidence.
			lastPulse = time.Now()
		}
		if missing >= 3 {
			writeEvent(timeline, "game_process_disappeared", map[string]any{"pid": p.PID, "causality": "NOT_ESTABLISHED", "exit_status": "UNAVAILABLE"})
			writeSessionEnd(dir, meta, "PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN")
			// GUI writes its own health; worker must not overwrite UI evidence.
			return true, nil
		}
		delay := 750 * time.Millisecond
		sharedDeep := filepath.Join(runDir, "deep-capture.flag")
		if _, e := os.Stat(sharedDeep); e == nil {
			delay = 350 * time.Millisecond
		}
		time.Sleep(delay)
	}
}
func hostReader(c liveConfig, runDir string) {
	dir := filepath.Join(c.Root, "Logs", "_control", c.Run, "android")
	for !stopFlag() {
		if b, e := os.ReadFile(filepath.Join(dir, "host-live.json")); e == nil {
			var data map[string]any
			if json.Unmarshal(b, &data) == nil && data["token"] == c.Token && data["run"] == c.Run {
				update(func(d *dashboard) {
					for source, key := range map[string]string{"cpu_host": "CPU_HOST", "ram_host": "RAM_HOST", "gpu_busy": "GPU_BUSY", "gpu_freq": "GPU_FREQ", "gpu_temp": "GPU_TEMP", "soc_temp": "SOC_TEMP", "dxvk": "DXVK", "gpu": "GPU", "wrapper": "WRAPPER", "driver": "DRIVER", "api": "API", "gpu_source": "GPU_SOURCE"} {
						if v, ok := data[source].(string); ok && v != "" {
							d.Fields[key] = v
						}
					}
				})
			}
		}
		if b, e := os.ReadFile(filepath.Join(dir, "android-device.json")); e == nil {
			var device struct {
				Properties map[string]string `json:"properties"`
			}
			if json.Unmarshal(b, &device) == nil {
				update(func(d *dashboard) {
					if v := device.Properties["ro.product.model"]; v != "" && v != "UNAVAILABLE" {
						d.Fields["MODEL"] = v
					}
					if v := device.Properties["ro.soc.model"]; v != "" && v != "UNAVAILABLE" {
						d.Fields["SOC"] = v
					}
					if v := device.Properties["ro.build.version.release"]; v != "" && v != "UNAVAILABLE" {
						d.Fields["ANDROID"] = v
					}
				})
			}
		}
		time.Sleep(2500 * time.Millisecond)
	}
}

func monitor(c liveConfig) {
	runDir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows")
	_ = os.MkdirAll(runDir, 0700)
	_ = os.MkdirAll(filepath.Join(runDir, "sessions"), 0700)
	errFile, _ := os.OpenFile(filepath.Join(runDir, "observer.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if errFile != nil {
		defer errFile.Close()
	}
	go hostReader(c, runDir)
	writeRunStatus(runDir, c, "ready", "WINDOWS_MONITOR_STARTED_GUI_STATUS_IN_UI_HEALTH", 0)
	_ = writeJSON(filepath.Join(runDir, "observer-identity.json"), map[string]any{"schema": "gpu-lab-identity-v1", "exe": "GPU-Lab.exe", "build": "R5-ISOLATED-WORKER-GUI", "windows_amd64": true, "observer_only": true, "run": c.Run, "token": c.Token})
	baseline := map[uint32]bool{}
	for pid := range enumerateProcesses() {
		baseline[pid] = true
	}
	seen := map[string]bool{}
	var last uint32
	stable := 0
	count := 0
	lastStatus := time.Time{}
	lastCompleted := time.Time{}
	monitorStarted := time.Now()
	preexistingAnnounced := false
	_ = atomicWrite(filepath.Join(runDir, "started.ok"), []byte(c.Token+"\n"))
	for !stopFlag() {
		// Single-test default: preserve a short post-exit log window, then
		// signal normal completion without waiting indefinitely for a new game.
		// The worker stops only after observeSession confirmed process exit.
		if !lastCompleted.IsZero() && time.Since(lastCompleted) >= 15*time.Second {
			writeEvent(filepath.Join(runDir, "observer-detection.jsonl"), "post_game_idle_auto_finish", map[string]any{"idle_seconds": 15, "completed_sessions": count})
			break
		}
		if time.Since(lastStatus) > 2*time.Second {
			writeRunStatus(runDir, c, "waiting", "WAIT_NEW_GAME", count)
			// GUI writes its own health; worker must not overwrite UI evidence.
			lastStatus = time.Now()
		}
		allowExisting := time.Since(monitorStarted) > 8*time.Second
		if allowExisting && !preexistingAnnounced {
			writeEvent(filepath.Join(runDir, "observer-detection.jsonl"), "preexisting_game_fallback_armed", map[string]any{"grace_seconds": 8, "game_path_or_ram_required": true})
			preexistingAnnounced = true
		}
		p, key, path := selectGame(baseline, seen, &last, &stable, allowExisting)
		if p.PID == 0 {
			time.Sleep(600 * time.Millisecond)
			continue
		}
		seen[key] = true
		if baseline[p.PID] {
			writeEvent(filepath.Join(runDir, "observer-detection.jsonl"), "preexisting_game_attached", map[string]any{"pid": p.PID, "exe": p.Name, "path": path})
		}
		count++
		writeRunStatus(runDir, c, "capturing", "PROCESS_OBSERVED", count)
		if errFile != nil {
			fmt.Fprintf(errFile, "%s START %s pid=%d path=%s\n", timestamp(), p.Name, p.PID, path)
			_ = errFile.Sync()
		}
		exited, e := observeSession(runDir, c, p, path, count)
		if e != nil {
			writeEvent(filepath.Join(runDir, "observer-errors.jsonl"), "session_error", map[string]any{"error": e.Error()})
			if errFile != nil {
				fmt.Fprintln(errFile, "ERROR "+e.Error())
			}
		}
		writeRunStatus(runDir, c, "waiting", "GAME_EXIT_OR_OBSERVER_STOP", count)
		// GUI writes its own health; worker has no UI callbacks.
		update(func(d *dashboard) {
			d.Running = false
			d.Done = true
			d.Status = "SESSÃO FINALIZADA"
			d.Fields["STATE"] = "FINALIZADO"
		})
		if exited && !stopFlag() {
			lastCompleted = time.Now()
			time.Sleep(2200 * time.Millisecond)
			update(func(d *dashboard) {
				d.Done = false
				d.Fields["STATE"] = "AGUARDANDO"
				d.Fields["PID"] = "N/D"
				// Keep the last game's readable title instead of erasing it.
				d.Status = "AGUARDANDO NOVO JOGO"
			})
		}
	}
	writeRunStatus(runDir, c, "stopped", "OBSERVER_STOPPED_GUI_CLOSED_OR_WINDOW_LOOP_EXIT_GAME_UNTOUCHED", count)
	// GUI writes its own health; worker has no UI callbacks.
	_ = atomicWrite(filepath.Join(runDir, "finished.ok"), []byte(c.Token+"\n"))
	finished.Store(true)
	if h := guiWnd.Load(); h != 0 {
		postMessage.Call(h, WM_CLOSE, 0, 0)
	}
}

// No process/window writes in this journal: it cannot interfere with the game or GUI.
func startupGUIJournal(c liveConfig) {
	runDir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows")
	for i, delay := range []time.Duration{0, 2 * time.Second, 3 * time.Second, 5 * time.Second, 10 * time.Second, 20 * time.Second} {
		if delay > 0 {
			time.Sleep(delay)
		}
		if stopFlag() {
			return
		}
		writeEvent(filepath.Join(runDir, "startup-timeline.jsonl"), "gui_startup_observation", map[string]any{
			"sample": i, "gui_lifecycle": guiLifecycle.Load(),
			"timers": guiTimerCount.Load(), "paints": guiPaintCount.Load(),
			"native_updates":  guiNativeUpdates.Load(),
			"native_failures": guiNativeFailures.Load(),
			"readback":        nativeStatusString(),
			"observer_note":   "PASSIVE_NO_WIN32_CALLS",
		})
	}
}

// R5 isolation: the observer/worker and GUI are separate processes of the SAME executable.
// A Wine/Win32 callback exception in the GUI cannot kill the worker or discard session data.
// The controller is itself GUI-free and supervises both children, with throttled GUI recovery.
func argsFor(c liveConfig, role string) []string {
	return []string{"--role", role, "--run", c.Run, "--token", c.Token, "--root", c.Root}
}
func startChild(c liveConfig, role string, recoverySafeMode bool) (*exec.Cmd, error) {
	me, e := os.Executable()
	if e != nil {
		return nil, e
	}
	cmd := exec.Command(me, argsFor(c, role)...)
	if role == "gui" {
		// GUI-only environment; NEVER alter the game or the independent worker.
		// Explicit GPU_LAB_GUI_SAFE_MODE=1 also enables this from the first attempt.
		// Recovery automatically uses fewer Win32/GDI calls after a GUI crash.
		safe := recoverySafeMode || os.Getenv("GPU_LAB_GUI_SAFE_MODE") == "1"
		if safe {
			cmd.Env = append(os.Environ(), "GPU_LAB_GUI_SAFE_MODE=1")
		}
		if os.Getenv("GPU_LAB_GUI_TRACE_SEH") == "1" {
			env := cmd.Env
			if env == nil {
				env = os.Environ()
			}
			// Wine's SEH channel is only enabled in the GUI process. It may
			// not produce a usable native stack on all Bannerlator builds.
			cmd.Env = append(env, "WINEDEBUG=-all,+seh")
		}
	}
	dir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows")
	_ = os.MkdirAll(dir, 0700)
	// Avoid shared append descriptors and user-visible terminal noise from children.
	errlog, e := os.OpenFile(filepath.Join(dir, role+"-stderr.txt"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if e != nil {
		return nil, e
	}
	outlog, e := os.OpenFile(filepath.Join(dir, role+"-stdout.txt"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if e != nil {
		errlog.Close()
		return nil, e
	}
	cmd.Stderr = errlog
	cmd.Stdout = outlog
	e = cmd.Start()
	errlog.Close()
	outlog.Close()
	if e != nil {
		return nil, e
	}
	return cmd, nil
}

// A completed game session is not a reason to relaunch a broken Wine GUI.
// This is read-only and requires a matching worker-produced exit record.
func gameSessionAlreadyFinished(c liveConfig) bool {
	dir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows")
	var latest struct{ Session, Token, Run string }
	data, err := os.ReadFile(filepath.Join(dir, "latest-session.json"))
	if err != nil || json.Unmarshal(data, &latest) != nil ||
		latest.Token != c.Token || latest.Run != c.Run || latest.Session == "" ||
		filepath.Base(latest.Session) != latest.Session || strings.Contains(latest.Session, "..") {
		return false
	}
	var session struct{ Token, Run, Session, State, Reason string }
	data, err = os.ReadFile(filepath.Join(c.Root, "Logs", latest.Session, "Windows", "session.json"))
	if err != nil || json.Unmarshal(data, &session) != nil {
		return false
	}
	return session.Run == c.Run && session.Token == c.Token &&
		session.Session == latest.Session && session.State == "finished" &&
		(session.Reason == "PROCESS_NO_LONGER_RUNNING_EXIT_CAUSE_UNKNOWN" || session.Reason == "GAME_PROCESS_EXITED")
}

func controller(c liveConfig) {
	dir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows")
	_ = os.MkdirAll(dir, 0700)
	worker, e := startChild(c, "worker", false)
	if e != nil {
		writeEvent(filepath.Join(dir, "controller-events.jsonl"), "worker_start_failed", map[string]any{"error": e.Error()})
		return
	}
	workerDone := make(chan error, 1)
	go func() { workerDone <- worker.Wait() }()
	failures := 0
	for {
		if stopFlag() {
			break
		}
		gui, e := startChild(c, "gui", failures > 0)
		if e != nil {
			writeEvent(filepath.Join(dir, "controller-events.jsonl"), "gui_start_failed", map[string]any{"error": e.Error()})
			failures++
		} else {
			writeEvent(filepath.Join(dir, "controller-events.jsonl"), "gui_started", map[string]any{"pid": gui.Process.Pid, "attempt": failures + 1})
			guiDone := make(chan error, 1)
			go func() { guiDone <- gui.Wait() }()
			select {
			case er := <-workerDone:
				writeEvent(filepath.Join(dir, "controller-events.jsonl"), "worker_exited", map[string]any{"error": fmt.Sprint(er)})
				_ = atomicWrite(filepath.Join(dir, "worker-exited.marker"), []byte(fmt.Sprint(er)+"\n"))
				select {
				case <-guiDone:
				case <-time.After(6 * time.Second):
				}
				return
			case er := <-guiDone:
				writeEvent(filepath.Join(dir, "controller-events.jsonl"), "gui_exited", map[string]any{"error": fmt.Sprint(er), "intentional_stop": stopFlag(), "recovery_safe_mode": failures > 0 || os.Getenv("GPU_LAB_GUI_SAFE_MODE") == "1"})
				// Journal exactly what is known, not an invented Win32 stack.
				// This is written by the controller AFTER the GUI has exited;
				// a crash cannot prevent the worker's independent logging.
				_ = writeJSON(filepath.Join(dir, "gui-last-exit.json"), map[string]any{
					"schema": "gpu-lab-gui-exit-v1", "at": timestamp(),
					"exit_status": fmt.Sprint(er), "root_cause": "NOT_PROVEN",
					"intentional_stop":               stopFlag(),
					"next_gui_recovery_safe_mode":    !stopFlag(),
					"seh_trace_requested":            os.Getenv("GPU_LAB_GUI_TRACE_SEH") == "1",
					"worker_continues_independently": true,
					"note":                           "Native Wine access violation requires a Wine backtrace or crash dump to identify its origin.",
				})
				if stopFlag() {
					break
				}
				if gameSessionAlreadyFinished(c) {
					writeEvent(filepath.Join(dir, "controller-events.jsonl"), "postgame_gui_relaunch_suppressed", map[string]any{"reason": "CONFIRMED_COMPLETED_GAME;WAIT_WORKER_OR_ANDROID_FALLBACK"})
					select {
					case wer := <-workerDone:
						writeEvent(filepath.Join(dir, "controller-events.jsonl"), "worker_exited_after_gui_postgame", map[string]any{"error": fmt.Sprint(wer)})
					case <-time.After(23 * time.Second):
						writeEvent(filepath.Join(dir, "controller-events.jsonl"), "postgame_worker_timeout_host_fallback_expected", nil)
					}
					return
				}
				failures++
			}
		}
		if stopFlag() {
			break
		}
		wait := time.Duration(2+failures*2) * time.Second
		if wait > 30*time.Second {
			wait = 30 * time.Second
		}
		writeEvent(filepath.Join(dir, "controller-events.jsonl"), "gui_recovery_pending", map[string]any{"backoff_seconds": wait.Seconds(), "count": failures})
		select {
		case er := <-workerDone:
			writeEvent(filepath.Join(dir, "controller-events.jsonl"), "worker_exited_during_recovery", map[string]any{"error": fmt.Sprint(er)})
			return
		case <-time.After(wait):
		}
	}
	select {
	case er := <-workerDone:
		writeEvent(filepath.Join(dir, "controller-events.jsonl"), "worker_stopped", map[string]any{"error": fmt.Sprint(er)})
	case <-time.After(20 * time.Second):
		writeEvent(filepath.Join(dir, "controller-events.jsonl"), "worker_shutdown_timeout", nil)
	}
}
func viewState(c liveConfig) {
	dir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows")
	previous := ""
	for {
		if stopFlag() {
			finished.Store(true)
			if hwnd := guiWnd.Load(); hwnd != 0 {
				postMessage.Call(hwnd, WM_CLOSE, 0, 0)
			}
			return
		}
		if _, e := os.Stat(filepath.Join(dir, "worker-exited.marker")); e == nil {
			update(func(d *dashboard) { d.Status = "MONITOR ENCERRADO"; d.Fields["STATE"] = "FALHA NO MONITOR" })
			finished.Store(true)
			if hwnd := guiWnd.Load(); hwnd != 0 {
				postMessage.Call(hwnd, WM_CLOSE, 0, 0)
			}
			return
		}
		if _, e := os.Stat(filepath.Join(dir, "finished.ok")); e == nil {
			finished.Store(true)
			if hwnd := guiWnd.Load(); hwnd != 0 {
				postMessage.Call(hwnd, WM_CLOSE, 0, 0)
			}
			return
		}
		var latest struct{ Session, Token, Run string }
		if b, e := os.ReadFile(filepath.Join(dir, "latest-session.json")); e == nil && json.Unmarshal(b, &latest) == nil && latest.Token == c.Token && latest.Run == c.Run {
			location := filepath.Join(c.Root, "Logs", latest.Session, "Windows")
			var meta struct {
				Game, Game_title, Path, State string
				PID                           uint32
			}
			if b, e := os.ReadFile(filepath.Join(location, "session.json")); e == nil && json.Unmarshal(b, &meta) == nil {
				cpu, ram := "N/D", "N/D"
				if fp, e := os.Open(filepath.Join(location, "telemetry", "game.csv")); e == nil {
					fi, er := fp.Stat()
					if er == nil {
						start := fi.Size() - 4096
						if start < 0 {
							start = 0
						}
						_, _ = fp.Seek(start, io.SeekStart)
						sc := bufio.NewScanner(fp)
						for sc.Scan() {
							line := sc.Text()
							fields := strings.Split(line, ",")
							if len(fields) >= 4 && fields[1] != "cpu_percent_one_core" {
								cpu = fields[1]
								ram = fields[2]
							}
						}
					}
					_ = fp.Close()
				}
				markerMu.Lock()
				currentSession = latest.Session
				sessionPath.Store(location)
				markerMu.Unlock()
				update(func(d *dashboard) {
					d.Fields["GAME"] = meta.Game_title
					if d.Fields["GAME"] == "" {
						d.Fields["GAME"] = meta.Game
					}
					d.Fields["PRODUCT"] = meta.Game
					d.Fields["PID"] = strconv.FormatUint(uint64(meta.PID), 10)
					d.Fields["PATH"] = meta.Path
					d.Fields["SESSION"] = latest.Session
					if cpu != "N/D" {
						d.Fields["CPU_GAME"] = cpu + "%"
					} else {
						d.Fields["CPU_GAME"] = "N/D"
					}
					d.Fields["RAM_GAME"] = ram
					d.Running = meta.State == "active"
					d.Done = meta.State == "finished"
					if d.Running {
						d.Fields["STATE"] = "GRAVANDO"
						d.Status = "GRAVANDO"
					} else {
						d.Fields["STATE"] = "FINALIZADO"
						d.Status = "SESSÃO FINALIZADA"
					}
				})
				if latest.Session != previous {
					previous = latest.Session
					writeEvent(filepath.Join(dir, "gui-read-events.jsonl"), "session_attached", map[string]any{"session": previous})
				}
			}
		}
		// Read-only Android host bridge; any missing metric remains N/D.
		if b, e := os.ReadFile(filepath.Join(c.Root, "Logs", "_control", c.Run, "android", "host-live.json")); e == nil {
			var data map[string]any
			if json.Unmarshal(b, &data) == nil && data["token"] == c.Token && data["run"] == c.Run {
				update(func(d *dashboard) {
					for k, v := range map[string]string{"cpu_host": "CPU_HOST", "ram_host": "RAM_HOST", "gpu_busy": "GPU_BUSY", "gpu_freq": "GPU_FREQ", "gpu_temp": "GPU_TEMP", "soc_temp": "SOC_TEMP", "dxvk": "DXVK", "gpu": "GPU", "wrapper": "WRAPPER", "driver": "DRIVER", "api": "API", "gpu_source": "GPU_SOURCE"} {
						if x, ok := data[k].(string); ok && x != "" {
							d.Fields[v] = x
						}
					}
				})
			}
		}
		// The worker process owns a separate in-memory dashboard. Previously it
		// populated MODEL/SOC/ANDROID only in that worker; the GUI could never
		// see those fields. Read the same passive device snapshot in the GUI.
		if b, e := os.ReadFile(filepath.Join(c.Root, "Logs", "_control", c.Run, "android", "android-device.json")); e == nil {
			var device struct {
				Properties map[string]string `json:"properties"`
			}
			if json.Unmarshal(b, &device) == nil {
				valid := func(s string) bool { return s != "" && s != "UNAVAILABLE" && s != "N/D" }
				update(func(d *dashboard) {
					if v := device.Properties["ro.product.model"]; valid(v) {
						d.Fields["MODEL"] = v
					}
					if v := device.Properties["ro.soc.model"]; valid(v) {
						d.Fields["SOC"] = v
					} else if v := device.Properties["ro.board.platform"]; valid(v) {
						d.Fields["SOC"] = v
					}
					if v := device.Properties["ro.build.version.release"]; valid(v) {
						d.Fields["ANDROID"] = v
					}
				})
			}
		}
		writeUIHealth(dir)
		time.Sleep(1500 * time.Millisecond)
	}
}
func main() {
	runtime.GOMAXPROCS(2)
	c := liveConfig{}
	role := "controller"
	for i := 1; i+1 < len(os.Args); i++ {
		switch os.Args[i] {
		case "--role":
			role = os.Args[i+1]
		case "--run":
			c.Run = os.Args[i+1]
		case "--token":
			c.Token = os.Args[i+1]
		case "--root":
			c.Root = os.Args[i+1]
		}
	}
	if c.Root == "" {
		if me, e := os.Executable(); e == nil {
			c.Root = filepath.Dir(me)
		}
	}
	if c.Root == "" {
		return
	}
	if c.Run == "" {
		c.Run = "manual-" + time.Now().Format("20060102-150405") + "-" + tokenHex()
	}
	if c.Token == "" {
		c.Token = tokenHex()
	}
	if len(c.Run) > 128 || (strings.Contains(c.Run, "/") || strings.Contains(c.Run, "\\")) || strings.Contains(c.Run, "..") {
		return
	}
	controlStopFile = filepath.Join(c.Root, "Logs", "_control", c.Run, "windows", "stop.request")
	switch role {
	case "worker":
		monitor(c)
	case "gui":
		go viewState(c)
		startGUI()
	case "controller":
		lockDir := filepath.Join(c.Root, "Logs", "_control", c.Run, "windows", "controller.lock")
		if e := os.Mkdir(lockDir, 0700); e != nil {
			// Exit 88 distinguishes duplicate launch from a real controller exit.
			// The BAT must not publish bat-exit.txt for a duplicate launcher.
			writeEvent(filepath.Join(c.Root, "Logs", "_control", c.Run, "windows", "observer-detection.jsonl"), "duplicate_controller_refused", map[string]any{"error": e.Error()})
			os.Exit(88)
		}
		controller(c)
	}
}
