//go:build windows

package main

// R12: One Win32 window, native child STATIC controls for every dynamic value.
// The previous GDI-only renderer could report 194 successful BitBlts while
// Wine continued displaying an old AGUARDANDO frame. Native SetWindowTextW
// (plus GetWindowTextW readback) removes BitBlt from the state-text path.
// Graphics/background are cosmetic; capture never waits for any GUI call.

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const (
	WM_DESTROY        = 0x0002
	WM_CLOSE          = 0x0010
	WM_PAINT          = 0x000F
	WM_ERASEBKGND     = 0x0014
	WM_TIMER          = 0x0113
	WM_KEYDOWN        = 0x0100
	WM_HOTKEY         = 0x0312
	MOD_CONTROL       = 0x0002
	MOD_NOREPEAT      = 0x4000
	WM_COMMAND        = 0x0111
	WM_CTLCOLORSTATIC = 0x0138
	WM_SETFONT        = 0x0030
	WM_APP_REFRESH    = 0x8000 + 91
	GUI_TIMER_ID      = 2811
	WS_OVERLAPPED     = 0
	WS_CAPTION        = 0x00C00000
	WS_SYSMENU        = 0x00080000
	WS_MINIMIZEBOX    = 0x00020000
	WS_CHILD          = 0x40000000
	WS_VISIBLE        = 0x10000000
	WS_CLIPCHILDREN   = 0x02000000
	SS_LEFT           = 0
	SS_NOPREFIX       = 0x80
	SS_NOTIFY         = 0x100
	SS_CENTERIMAGE    = 0x200
	SW_SHOW           = 5
	SW_HIDE           = 0
	PS_SOLID          = 0
	TRANSPARENT       = 1
	DT_SINGLELINE     = 0x20
	DT_VCENTER        = 4
	DT_END_ELLIPSIS   = 0x8000
)

type POINT struct{ X, Y int32 }
type PAINTSTRUCT struct {
	Hdc                uintptr
	Erase              int32
	Paint              RECT
	Restore, IncUpdate int32
	Reserved           [32]byte
}
type WNDCLASSEX struct {
	Size, Style                   uint32
	WndProc                       uintptr
	ClsExtra, WndExtra            int32
	Instance, Icon, Cursor, Brush uintptr
	MenuName, ClassName           *uint16
	IconSm                        uintptr
}
type MSG struct {
	Hwnd           uintptr
	Message        uint32
	Padding        uint32
	WParam, LParam uintptr
	Time           uint32
	Pt             POINT
	Private        uint32
}

var (
	gdi                    = syscall.NewLazyDLL("gdi32.dll")
	createWindow           = u32.NewProc("CreateWindowExW")
	regClass               = u32.NewProc("RegisterClassExW")
	defProc                = u32.NewProc("DefWindowProcW")
	beginPaint             = u32.NewProc("BeginPaint")
	endPaint               = u32.NewProc("EndPaint")
	fillRect               = u32.NewProc("FillRect")
	invalidate             = u32.NewProc("InvalidateRect")
	getMessage             = u32.NewProc("GetMessageW")
	transMessage           = u32.NewProc("TranslateMessage")
	dispatchMsg            = u32.NewProc("DispatchMessageW")
	postQuit               = u32.NewProc("PostQuitMessage")
	getModule              = k32.NewProc("GetModuleHandleW")
	loadCursor             = u32.NewProc("LoadCursorW")
	updateWindow           = u32.NewProc("UpdateWindow")
	setBkMode              = gdi.NewProc("SetBkMode")
	setTextColor           = gdi.NewProc("SetTextColor")
	createFont             = gdi.NewProc("CreateFontW")
	selectObj              = gdi.NewProc("SelectObject")
	deleteObj              = gdi.NewProc("DeleteObject")
	solidBrush             = gdi.NewProc("CreateSolidBrush")
	rectRound              = gdi.NewProc("RoundRect")
	createPen              = gdi.NewProc("CreatePen")
	moveTo                 = gdi.NewProc("MoveToEx")
	lineTo                 = gdi.NewProc("LineTo")
	getKeyState            = u32.NewProc("GetKeyState")
	registerHotKey         = u32.NewProc("RegisterHotKey")
	unregisterHotKey       = u32.NewProc("UnregisterHotKey")
	destroyWindow          = u32.NewProc("DestroyWindow")
	postMessage            = u32.NewProc("PostMessageW")
	setTimer               = u32.NewProc("SetTimer")
	killTimer              = u32.NewProc("KillTimer")
	sendMessage            = u32.NewProc("SendMessageW")
	setWindowText          = u32.NewProc("SetWindowTextW")
	getWindowText          = u32.NewProc("GetWindowTextW")
	getWindowTextLen       = u32.NewProc("GetWindowTextLengthW")
	localeAPI              = k32.NewProc("GetUserDefaultLocaleName")
	showNative             = u32.NewProc("ShowWindow")
	guiClass               = syscall.StringToUTF16Ptr("GpuLabNativeRebuild1")
	guiCb                  = syscall.NewCallback(wndproc)
	guiWnd                 atomic.Uintptr
	guiLifecycle           atomic.Uint32
	guiPaintCount          atomic.Uint64
	guiTimerCount          atomic.Uint64
	guiLastPaintUnixNano   atomic.Int64
	guiRenderedPhase       atomic.Uint32
	guiBackBufferOK        atomic.Uint64
	guiBackBufferFail      atomic.Uint64
	guiLastRefreshUnixNano atomic.Int64
	finished               atomic.Bool
	stopOnce               sync.Once
	activeTab              atomic.Uint32
	guiNativeUpdates       atomic.Uint64
	guiNativeFailures      atomic.Uint64
	guiNativeMatched       atomic.Uint64
	guiNativeStatus        atomic.Value
	guiNativePID           atomic.Value
	guiLanguage            = "en"
	globalHotkeysEnabled   bool
	registeredHotkeys      []int
	manualLanguage         = "auto"
	guiLanguageFile        string
	guiLocaleMenu          bool
	guiSafeMode            bool
	guiCachedWindowTitle   string
	nativeFonts            [3]uintptr
	nativeBrushes          [4]uintptr
	nativeControls         = map[string]*nativeField{}
	nativeParent           uintptr
	nativeIDs              = map[int]string{}
)

type nativeField struct {
	h     uintptr
	tab   int
	key   string
	last  string
	kind  int
	title string
}
type uiSlot struct {
	key, translation string
	x, y, w          int
}

var pageSlots = [][]uiSlot{
	{
		{"GAME", "game", 30, 147, 500}, {"PID", "PID", 573, 147, 141},
		{"FPS", "FPS", 32, 210, 160}, {"CPU_GAME", "cpu_game", 208, 210, 160},
		{"RAM_GAME", "ram_game", 384, 210, 160}, {"GPU_BUSY", "gpu_load", 560, 210, 160},
		{"CPU_HOST", "cpu_host", 32, 320, 205}, {"GPU", "identified_gpu", 251, 320, 265},
		{"SOC", "soc", 538, 320, 170},
	},
	{
		{"FPS", "FPS", 30, 155, 155}, {"AVG_FPS", "avg_fps", 205, 155, 155}, {"LOW_1", "1% LOW", 380, 155, 155}, {"LOW_01", "0.1% LOW", 555, 155, 155},
		{"FRAME_MS", "frame_time", 30, 232, 155}, {"P95", "P95", 205, 232, 155}, {"P99", "P99", 380, 232, 155}, {"P999", "P99.9", 555, 232, 155},
		{"FRAMES", "frames_tab", 30, 309, 155}, {"SUBMITS", "SUBMITS", 205, 309, 155}, {"EVENTS", "events", 380, 309, 155}, {"DROPS", "dropped", 555, 309, 155},
		{"SOURCE_FPS", "source_fps", 30, 386, 680},
	},
	{
		{"MODEL", "model", 30, 153, 156}, {"SOC", "soc", 206, 153, 156}, {"ANDROID", "ANDROID", 382, 153, 156}, {"CPU_ARCH", "architecture", 558, 153, 156},
		{"CPU_CORES", "cpu_cores", 30, 221, 156}, {"CPU_HOST", "cpu_host", 206, 221, 156}, {"RAM_HOST", "ram_available", 382, 221, 156}, {"GPU_BUSY", "gpu_load", 558, 221, 156},
		{"GPU_FREQ", "gpu_frequency", 30, 289, 156}, {"GPU_FREQ_MIN", "gpu_min", 206, 289, 156}, {"GPU_FREQ_MAX", "gpu_max", 382, 289, 156}, {"GPU_TEMP", "gpu_temperature", 558, 289, 156},
		{"SOC_TEMP", "soc_temperature", 30, 357, 156}, {"KBASE", "KBASE", 206, 357, 156}, {"GOVERNOR", "governor", 382, 357, 156}, {"KERNEL", "KERNEL", 558, 357, 156},
	},
	{
		{"API", "api_graphics", 30, 153, 156}, {"WRAPPER", "WRAPPER", 206, 153, 156}, {"DXVK", "DXVK", 382, 153, 156}, {"VKD3D", "VKD3D", 558, 153, 156},
		{"VULKAN", "VULKAN", 30, 221, 156}, {"PANVK", "PANVK", 206, 221, 156}, {"DRIVER", "DRIVER", 382, 221, 156}, {"GPU_SOURCE", "gpu_source", 558, 221, 156},
		{"WINE", "WINE", 30, 289, 156}, {"TRANSLATOR", "translator", 206, 289, 156}, {"LAUNCHER", "launcher", 382, 289, 156}, {"PRODUCT", "product", 558, 289, 156},
		{"VK_MEM", "vk_memory", 30, 357, 156}, {"VK_PEAK", "vk_peak", 206, 357, 156}, {"GPU", "gpu", 382, 357, 156}, {"DRIVER_SO_SHA256", "SO SHA256", 558, 357, 156},
	},
	{
		{"SESSION", "session", 30, 153, 325}, {"ROUND", "round", 380, 153, 330},
		{"STATE", "state", 30, 224, 325}, {"SOURCE_FPS", "source_fps", 380, 224, 330},
		{"FRAMES", "frames_tab", 30, 295, 150}, {"EVENTS", "events", 205, 295, 150}, {"SUBMITS", "SUBMITS", 380, 295, 150}, {"DROPS", "dropped", 555, 295, 150},
		{"EXE_SHA256", "EXE SHA256", 30, 370, 680},
	},
}

func wineLocaleName() string {
	var b [96]uint16
	n, _, _ := localeAPI.Call(uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	if n > 1 && n < uintptr(len(b)) {
		return syscall.UTF16ToString(b[:])
	}
	return ""
}
func initUILanguage() {
	if p, e := os.Executable(); e == nil {
		guiLanguageFile = filepath.Join(filepath.Dir(p), "GPU-Lab.language")
		if b, er := os.ReadFile(guiLanguageFile); er == nil {
			v := strings.TrimSpace(string(b))
			if v == "auto" || localeCode(v) != "" {
				manualLanguage = v
			}
		}
	}
	refreshUILanguage()
}
func refreshUILanguage() {
	a := os.Getenv("ATW_ANDROID_LOCALE")
	if localeCode(a) != "" {
		guiLanguage = detectLocale(manualLanguage, a, "", "", "")
	} else {
		guiLanguage = detectLocale(manualLanguage, wineLocaleName(), os.Getenv("LC_ALL"), os.Getenv("LANGUAGE"), os.Getenv("LANG"))
	}
}
func chooseUILanguage(v string) {
	if v != "auto" && localeCode(v) == "" {
		return
	}
	manualLanguage = v
	refreshUILanguage()
	if guiLanguageFile != "" {
		_ = os.WriteFile(guiLanguageFile, []byte(v+"\n"), 0600)
	}
	guiLocaleMenu = false
	refreshAllText(true)
}
func T(k string) string          { return localize(guiLanguage, k) }
func rgb(r, g, b uint32) uintptr { return uintptr(r | (g << 8) | (b << 16)) }
func rect(x, y, w, h int) RECT   { return RECT{int32(x), int32(y), int32(x + w), int32(y + h)} }
func gdiFill(dc uintptr, r RECT, c uintptr) {
	b, _, _ := solidBrush.Call(c)
	fillRect.Call(dc, uintptr(unsafe.Pointer(&r)), b)
	deleteObj.Call(b)
}
func gdiRound(dc uintptr, x, y, w, h int, c uintptr) {
	b, _, _ := solidBrush.Call(c)
	old, _, _ := selectObj.Call(dc, b)
	p, _, _ := createPen.Call(PS_SOLID, 0, c)
	op, _, _ := selectObj.Call(dc, p)
	rectRound.Call(dc, uintptr(x), uintptr(y), uintptr(x+w), uintptr(y+h), 12, 12)
	selectObj.Call(dc, old)
	selectObj.Call(dc, op)
	deleteObj.Call(p)
	deleteObj.Call(b)
}
func graph(dc uintptr, v []float64, x, y, w, h int) {
	if len(v) < 2 {
		return
	}
	p, _, _ := createPen.Call(PS_SOLID, 2, rgb(62, 208, 177))
	old, _, _ := selectObj.Call(dc, p)
	for i, a := range v {
		if a < 0 {
			a = 0
		}
		if a > 100 {
			a = 100
		}
		xx := x + i*w/(len(v)-1)
		yy := y + h - int(a*float64(h)/100)
		if i == 0 {
			moveTo.Call(dc, uintptr(xx), uintptr(yy), 0)
		} else {
			lineTo.Call(dc, uintptr(xx), uintptr(yy))
		}
	}
	selectObj.Call(dc, old)
	deleteObj.Call(p)
}
func setNative(h uintptr, v string) bool {
	if h == 0 {
		return false
	}
	u := syscall.StringToUTF16Ptr(v)
	ok, _, _ := setWindowText.Call(h, uintptr(unsafe.Pointer(u)))
	return ok != 0
}
func readNative(h uintptr) string {
	if h == 0 {
		return ""
	}
	n, _, _ := getWindowTextLen.Call(h)
	if n > 2048 {
		n = 2048
	}
	b := make([]uint16, n+2)
	getWindowText.Call(h, uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	return syscall.UTF16ToString(b)
}
func createNative(key string, tab, x, y, w, h, font, kind int, title string, notify bool) uintptr {
	st := uintptr(WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE)
	if notify {
		st |= SS_NOTIFY
	}
	id := 1000 + len(nativeControls)
	class := syscall.StringToUTF16Ptr("STATIC")
	s := syscall.StringToUTF16Ptr("")
	hh, _, _ := createWindow.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(s)), st, uintptr(x), uintptr(y), uintptr(w), uintptr(h), nativeParent, uintptr(id), 0, 0)
	if hh != 0 {
		sendMessage.Call(hh, WM_SETFONT, nativeFonts[font], 1)
		nativeControls[key] = &nativeField{h: hh, tab: tab, key: key, last: "", kind: kind, title: title}
		nativeIDs[id] = key
	}
	return hh
}
func nativeText(key, val string) {
	if f := nativeControls[key]; f != nil {
		if f.last != val {
			if setNative(f.h, val) {
				f.last = val
				guiNativeUpdates.Add(1)
			} else {
				guiNativeFailures.Add(1)
			}
		}
	}
}
func nativeTab(tab int) {
	activeTab.Store(uint32(tab))
	for _, f := range nativeControls {
		if f.tab < 0 {
			continue
		}
		if f.tab == tab {
			showNative.Call(f.h, SW_SHOW)
		} else {
			showNative.Call(f.h, SW_HIDE)
		}
	}
	refreshAllText(true)
}
func createUIControls() {
	createNative("header:title", -1, 27, 19, 163, 37, 2, 0, "", false)
	createNative("header:sub", -1, 185, 22, 138, 33, 0, 0, "", false)
	createNative("header:status", -1, 364, 24, 215, 29, 1, 1, "", false)
	createNative("header:locale", -1, 591, 24, 144, 29, 0, 1, "", true)
	createNative("footer:one", -1, 24, 451, 710, 20, 0, 0, "", false)
	createNative("footer:two", -1, 24, 473, 710, 20, 0, 0, "", false)
	for i := 0; i < 5; i++ {
		createNative(fmt.Sprintf("tab:%d", i), -1, 27+i*146, 87, 134, 28, 0, 1, "", true)
	}
	for tab, ss := range pageSlots {
		for _, slot := range ss {
			pre := fmt.Sprintf("v:%d:%s", tab, slot.key)
			kind := 0
			if tab == 0 && slot.y == 210 {
				kind = 3
			}
			createNative(pre+":name", tab, slot.x, slot.y, slot.w, 19, 0, func() int {
				if kind == 3 {
					return 3
				}
				return 2
			}(), slot.translation, false)
			createNative(pre+":value", tab, slot.x, slot.y+20, slot.w, 31, 1, kind, slot.key, false)
		}
	}
	for i := 0; i < len(languageOrder); i++ {
		k := fmt.Sprintf("lang:%d", i)
		x := 42 + (i/10)*349
		y := 105 + (i%10)*34
		createNative(k, -2, x, y, 315, 30, 1, 1, "", true)
	}
	for _, f := range nativeControls {
		if f.tab >= 0 || f.tab == -2 {
			showNative.Call(f.h, SW_HIDE)
		}
	}
	nativeTab(0)
}
func refreshAllText(force bool) {
	d := snap()
	phase := renderCapturePhase(d.Running, d.Done, d.Fields["PID"], d.Fields["STATE"])
	nativeText("header:title", "GPU Lab")
	nativeText("header:sub", "MONITOR")
	nativeText("header:status", "● "+visualPhaseText(guiLanguage, phase))
	ln := languageName(guiLanguage)
	if manualLanguage == "auto" {
		ln = "AUTO · " + ln
	}
	nativeText("header:locale", ln+" ▾")
	ts := []string{T("overview"), T("frames_tab"), T("hardware"), T("runtime"), T("evidence")}
	for i, name := range ts {
		nativeText(fmt.Sprintf("tab:%d", i), name)
	}
	nativeText("footer:one", "L CTRL+A  "+T("tabs")+"    L CTRL+B  "+T("mark")+"    L CTRL+C  "+T("deep"))
	nativeText("footer:two", "L CTRL+D  "+T("screenshot")+"    L CTRL+E  "+T("stop")+" ("+T("optional")+")    ·    "+T("auto_capture"))
	t := int(activeTab.Load()) % len(pageSlots)
	for _, s := range pageSlots[t] {
		pre := fmt.Sprintf("v:%d:%s", t, s.key)
		title := s.translation
		if strings.ToUpper(title) != title {
			title = T(title)
		}
		nativeText(pre+":name", title)
		val := d.Fields[s.key]
		if val == "" {
			val = "N/D"
		}
		nativeText(pre+":value", val)
	}
	for i, code := range languageOrder {
		nm := languageName(code)
		if code == "auto" {
			nm = T("language_auto")
		}
		nativeText(fmt.Sprintf("lang:%d", i), nm)
	}
	// A second, OS-owned title-bar state supplies independent Wine presentation.
	pid := d.Fields["PID"]
	if pid == "" || pid == "N/D" {
		pid = "—"
	}
	windowTitle := fmt.Sprintf("GPU Lab · %s · PID %s", visualPhaseText(guiLanguage, phase), pid)
	if windowTitle != guiCachedWindowTitle {
		if setNative(nativeParent, windowTitle) {
			guiCachedWindowTitle = windowTitle
		}
	}
	if guiSafeMode {
		guiNativeTitle.Store("UNVERIFIED_SAFE_MODE")
	} else {
		guiNativeTitle.Store(readNative(nativeParent))
	}
	guiRenderedPhase.Store(visualPhaseCode(phase))
	guiLastPaintUnixNano.Store(time.Now().UnixNano())
	guiPaintCount.Add(1)
	if guiSafeMode {
		// Disable optional synchronous GetWindowText[Length] readbacks in
		// post-crash recovery; reporting MUST NOT pretend readbacks occurred.
		guiNativeStatus.Store("UNVERIFIED_SAFE_MODE")
		guiNativePID.Store("UNVERIFIED_SAFE_MODE")
	} else {
		status := readNative(nativeControls["header:status"].h)
		guiNativeStatus.Store(status)
		guiNativePID.Store(readNative(nativeControls["v:0:PID:value"].h))
		if status == "● "+visualPhaseText(guiLanguage, phase) {
			guiNativeMatched.Add(1)
		}
	}
	// Layout only: a graph background can fail to repaint without stale value text.
	// Avoid repainting the entire Win32 window on every timer tick.
	// Data labels are owned by native STATIC controls and update independently.
	if force && t == 0 {
		invalidate.Call(nativeParent, 0, 0)
	}
	_ = force
}
func showLanguageMenu() {
	guiLocaleMenu = !guiLocaleMenu
	for i := range languageOrder {
		f := nativeControls[fmt.Sprintf("lang:%d", i)]
		if f == nil {
			continue
		}
		if guiLocaleMenu {
			showNative.Call(f.h, SW_SHOW)
		} else {
			showNative.Call(f.h, SW_HIDE)
		}
	}
	invalidate.Call(nativeParent, 0, 0)
}

// The opt-in global mode can intercept Ctrl+A..E inside Wine games. It is
// disabled unless GPU_LAB_GLOBAL_HOTKEYS=1 is explicitly set by the tester.
func shortcutAction(action string, source string) {
	if action == "" {
		return
	}
	if controlStopFile != "" {
		writeEvent(filepath.Join(filepath.Dir(controlStopFile), "hotkey-events.jsonl"), "shortcut", map[string]any{"action": action, "source": source})
	}
	switch action {
	case "panel":
		nativeTab((int(activeTab.Load()) + 1) % 5)
	case "marker":
		recordMarker(false)
	case "deep":
		update(func(d *dashboard) { d.Deep = !d.Deep })
		recordDeepToggle()
	case "screenshot":
		recordMarker(true)
	case "stop":
		if controlStopFile != "" {
			_ = atomicWrite(controlStopFile, []byte("USER_HOTKEY_STOP\n"))
		}
		stopOnce.Do(func() { close(stopRequested) })
		update(func(d *dashboard) { d.Status = "FINALIZANDO PARCIAL"; d.Fields["STATE"] = "FINALIZANDO" })
	}
}
func setupGlobalHotkeys(h uintptr) {
	// R5.3 crashed inside syscall.(*DLL).FindProc while RegisterHotKey
	// was called under Wine. Preserve local L-Ctrl shortcuts, but do not
	// execute the unsupported global registration in this stabilization.
	requested := os.Getenv("GPU_LAB_GLOBAL_HOTKEYS") == "1"
	globalHotkeysEnabled = false
	registeredHotkeys = nil
	status := map[string]any{"requested": requested, "mode": "LOCAL_FOREGROUND_ONLY", "registered": []string{}, "failed": []string{}, "left_ctrl_exclusive": true, "global_unavailable_reason": "DISABLED_AFTER_WINE_NATIVE_CRASH_IN_R5_3"}
	if controlStopFile != "" {
		_ = writeJSON(filepath.Join(filepath.Dir(controlStopFile), "hotkey-capabilities.json"), status)
	}
}
func keyRegisteredGlobally(key uint32) bool {
	if key < 'A' || key > 'E' {
		return false
	}
	expected := 4100 + int(key-'A')
	for _, id := range registeredHotkeys {
		if id == expected {
			return true
		}
	}
	return false
}
func teardownGlobalHotkeys(h uintptr) {
	for _, id := range registeredHotkeys {
		unregisterHotKey.Call(h, uintptr(id))
	}
	registeredHotkeys = nil
}
func wndproc(h uintptr, msg uint32, w, l uintptr) uintptr {
	switch msg {
	case WM_TIMER:
		if w == GUI_TIMER_ID {
			guiTimerCount.Add(1)
			refreshAllText(false)
			return 0
		}
	case WM_APP_REFRESH:
		// Native labels refresh on the GUI thread, never on the collector goroutine.
		refreshAllText(false)
		return 0
	case WM_CTLCOLORSTATIC:
		child := l
		kind := 2
		for _, f := range nativeControls {
			if f.h == child {
				kind = f.kind
				break
			}
		}
		color := rgb(230, 242, 250)
		if kind == 2 {
			color = rgb(137, 166, 195)
		}
		if statusControl := nativeControls["header:status"]; statusControl != nil && child == statusControl.h {
			color = rgb(111, 223, 175)
		}
		setBkMode.Call(w, TRANSPARENT)
		setTextColor.Call(w, color)
		return nativeBrushes[kind]
	case WM_ERASEBKGND:
		return 1
	case WM_PAINT:
		var ps PAINTSTRUCT
		dc, _, _ := beginPaint.Call(h, uintptr(unsafe.Pointer(&ps)))
		if dc != 0 {
			gdiFill(dc, rect(0, 0, 752, 514), rgb(10, 17, 29))
			if !guiSafeMode {
				gdiRound(dc, 13, 12, 726, 58, rgb(22, 34, 52))
				gdiRound(dc, 15, 133, 724, 303, rgb(22, 34, 52))
				if activeTab.Load() == 0 {
					for i := 0; i < 4; i++ {
						gdiRound(dc, 25+i*176, 196, 167, 76, rgb(31, 49, 72))
					}
					graph(dc, snap().CPUHistory, 33, 315, 493, 88)
				}
			}
		}
		endPaint.Call(h, uintptr(unsafe.Pointer(&ps)))
		return 0
	case WM_COMMAND:
		id := int(w & 0xffff)
		name := nativeIDs[id]
		if strings.HasPrefix(name, "tab:") {
			var n int
			fmt.Sscanf(name, "tab:%d", &n)
			if n >= 0 && n < 5 {
				guiLocaleMenu = false
				for i := range languageOrder {
					if f := nativeControls[fmt.Sprintf("lang:%d", i)]; f != nil {
						showNative.Call(f.h, SW_HIDE)
					}
				}
				nativeTab(n)
			}
			return 0
		}
		if name == "header:locale" {
			showLanguageMenu()
			return 0
		}
		if strings.HasPrefix(name, "lang:") {
			var n int
			fmt.Sscanf(name, "lang:%d", &n)
			if n >= 0 && n < len(languageOrder) {
				chooseUILanguage(languageOrder[n])
				for i := range languageOrder {
					if f := nativeControls[fmt.Sprintf("lang:%d", i)]; f != nil {
						showNative.Call(f.h, SW_HIDE)
					}
				}
				nativeTab(int(activeTab.Load()))
			}
			return 0
		}
	case WM_HOTKEY:
		if globalHotkeysEnabled && w >= 4100 && w < 4105 {
			shortcutAction(optionalShortcut(uint32('A'+(w-4100)), true), "REGISTER_HOTKEY")
			return 0
		}
	case WM_KEYDOWN:
		// Wine may report generic VK_CONTROL rather than VK_LCONTROL.
		// When global hotkeys are active, do not execute a second local action.
		if !globalHotkeysEnabled || !keyRegisteredGlobally(uint32(w)) {
			left, _, _ := getKeyState.Call(0xA2)
			anyCtrl, _, _ := getKeyState.Call(0x11)
			shortcutAction(optionalShortcut(uint32(w), (left|anyCtrl)&0x8000 != 0), "FOREGROUND_WM_KEYDOWN")
		}
		return 0
	case WM_CLOSE:
		if !finished.Load() {
			// Only a user-requested close stops the independent monitor.
			// A native GUI crash does not produce this token-bound stop marker.
			if controlStopFile != "" {
				_ = atomicWrite(controlStopFile, []byte("USER_GUI_CLOSE\n"))
			}
			stopOnce.Do(func() { close(stopRequested) })
		}
		destroyWindow.Call(h)
		return 0
	case WM_DESTROY:
		killTimer.Call(h, GUI_TIMER_ID)
		teardownGlobalHotkeys(h)
		postQuit.Call(0)
		return 0
	}
	v, _, _ := defProc.Call(h, uintptr(msg), w, l)
	return v
}
func makeFont(height, weight int) uintptr {
	name := syscall.StringToUTF16Ptr("Segoe UI")
	v, _, _ := createFont.Call(uintptr(int32(height)), 0, 0, 0, uintptr(weight), 0, 0, 0, 1, 0, 0, 5, 0, uintptr(unsafe.Pointer(name)))
	return v
}
func startGUI() {
	guiLifecycle.Store(1)
	defer guiLifecycle.Store(4)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	initUILanguage()
	guiSafeMode = os.Getenv("GPU_LAB_GUI_SAFE_MODE") == "1"
	instance, _, _ := getModule.Call(0)
	cursor, _, _ := loadCursor.Call(0, 32512)
	cls := WNDCLASSEX{Size: uint32(unsafe.Sizeof(WNDCLASSEX{})), WndProc: guiCb, Instance: instance, Cursor: cursor, ClassName: guiClass}
	ok, _, er := regClass.Call(uintptr(unsafe.Pointer(&cls)))
	if ok == 0 && er != syscall.Errno(1410) {
		guiLifecycle.Store(3)
		return
	}
	for i, x := range []struct{ height, weight int }{{-14, 400}, {-16, 600}, {-23, 700}} {
		nativeFonts[i] = makeFont(x.height, x.weight)
	}
	for i, c := range []uintptr{rgb(22, 34, 52), rgb(28, 46, 65), rgb(22, 34, 52), rgb(31, 49, 72)} {
		nativeBrushes[i], _, _ = solidBrush.Call(c)
	}
	title := syscall.StringToUTF16Ptr("GPU Lab")
	h, _, _ := createWindow.Call(0, uintptr(unsafe.Pointer(guiClass)), uintptr(unsafe.Pointer(title)), WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN, 110, 92, 752, 545, 0, 0, instance, 0)
	if h == 0 {
		guiLifecycle.Store(3)
		return
	}
	nativeParent = h
	guiWnd.Store(h)
	guiLifecycle.Store(2)
	createUIControls()
	showNative.Call(h, SW_SHOW)
	updateWindow.Call(h)
	refreshAllText(true)
	setupGlobalHotkeys(h)
	setTimer.Call(h, GUI_TIMER_ID, 1200, 0)
	if finished.Load() {
		postMessage.Call(h, WM_CLOSE, 0, 0)
	}
	var m MSG
	for {
		v, _, _ := getMessage.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if v == 0 || v == ^uintptr(0) {
			break
		}
		transMessage.Call(uintptr(unsafe.Pointer(&m)))
		dispatchMsg.Call(uintptr(unsafe.Pointer(&m)))
	}
	guiWnd.Store(0)
	nativeParent = 0
	nativeControls = map[string]*nativeField{}
	nativeIDs = map[int]string{}
	for _, f := range nativeFonts {
		if f != 0 {
			deleteObj.Call(f)
		}
	}
	for _, b := range nativeBrushes {
		if b != 0 {
			deleteObj.Call(b)
		}
	}
}
func repaint() {
	now := time.Now().UnixNano()
	for {
		before := guiLastRefreshUnixNano.Load()
		if now-before < int64(180*time.Millisecond) {
			return
		}
		if guiLastRefreshUnixNano.CompareAndSwap(before, now) {
			break
		}
	}
	if h := guiWnd.Load(); h != 0 {
		postMessage.Call(h, WM_APP_REFRESH, 0, 0)
	}
}
func nativeStatusString() string {
	if x := guiNativeStatus.Load(); x != nil {
		return x.(string)
	}
	return "UNAVAILABLE"
}
func nativePIDString() string {
	if x := guiNativePID.Load(); x != nil {
		return x.(string)
	}
	return "UNAVAILABLE"
}

var guiNativeTitle atomic.Value

func nativeWindowTitle() string {
	if x := guiNativeTitle.Load(); x != nil {
		return x.(string)
	}
	return "UNAVAILABLE"
}
func statText(v float64, unit string) string {
	if math.IsNaN(v) || math.IsInf(v, 0) || v < 0 {
		return "N/D"
	}
	return fmt.Sprintf("%.1f%s", v, unit)
}
func desktopScreenshot(dest string) error {
	getDC := u32.NewProc("GetDC")
	releaseDC := u32.NewProc("ReleaseDC")
	metrics := u32.NewProc("GetSystemMetrics")
	compatibleDC := gdi.NewProc("CreateCompatibleDC")
	compatibleBitmap := gdi.NewProc("CreateCompatibleBitmap")
	getBits := gdi.NewProc("GetDIBits")
	bitblt := gdi.NewProc("BitBlt")
	deleteDC := gdi.NewProc("DeleteDC")
	w, _, _ := metrics.Call(0)
	h, _, _ := metrics.Call(1)
	if w == 0 || h == 0 || w > 8192 || h > 8192 {
		return fmt.Errorf("invalid screenshot size")
	}
	screen, _, _ := getDC.Call(0)
	if screen == 0 {
		return fmt.Errorf("GetDC failed")
	}
	defer releaseDC.Call(0, screen)
	mem, _, _ := compatibleDC.Call(screen)
	if mem == 0 {
		return fmt.Errorf("CreateCompatibleDC failed")
	}
	defer deleteDC.Call(mem)
	bmp, _, _ := compatibleBitmap.Call(screen, w, h)
	if bmp == 0 {
		return fmt.Errorf("CreateCompatibleBitmap failed")
	}
	defer deleteObj.Call(bmp)
	old, _, _ := selectObj.Call(mem, bmp)
	defer selectObj.Call(mem, old)
	ok, _, _ := bitblt.Call(mem, 0, 0, w, h, screen, 0, 0, 0x00CC0020)
	if ok == 0 {
		return fmt.Errorf("BitBlt failed")
	}
	stride := ((int(w)*3 + 3) / 4) * 4
	pixels := make([]byte, stride*int(h))
	type bmi struct {
		Size                                                        uint32
		Width, Height                                               int32
		Planes, BitCount                                            uint16
		Compression, ImageSize, XPels, YPels, ClrUsed, ClrImportant uint32
	}
	hdr := bmi{Size: 40, Width: int32(w), Height: int32(h), Planes: 1, BitCount: 24, ImageSize: uint32(len(pixels))}
	got, _, _ := getBits.Call(mem, bmp, 0, h, uintptr(unsafe.Pointer(&pixels[0])), uintptr(unsafe.Pointer(&hdr)), 0)
	if got == 0 {
		return fmt.Errorf("GetDIBits failed")
	}
	f, e := os.OpenFile(dest, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if e != nil {
		return e
	}
	defer f.Close()
	_ = binary.Write(f, binary.LittleEndian, []byte{'B', 'M'})
	_ = binary.Write(f, binary.LittleEndian, uint32(len(pixels)+54))
	_ = binary.Write(f, binary.LittleEndian, uint32(0))
	_ = binary.Write(f, binary.LittleEndian, uint32(54))
	_ = binary.Write(f, binary.LittleEndian, hdr)
	_, e = f.Write(pixels)
	return e
}
