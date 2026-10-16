//go:build windows

package main

import (
	"os"
	"strings"
	"syscall"
	"unsafe"
)

var (
	u32             = syscall.NewLazyDLL("user32.dll")
	k32             = syscall.NewLazyDLL("kernel32.dll")
	procEnumWindows = u32.NewProc("EnumWindows")
	procVisible     = u32.NewProc("IsWindowVisible")
	procWindowPID   = u32.NewProc("GetWindowThreadProcessId")
	procWindowRect  = u32.NewProc("GetWindowRect")
)

type RECT struct{ Left, Top, Right, Bottom int32 }
type procInfo struct {
	PID, PPID uint32
	Name      string
}

func enumerateProcesses() map[uint32]procInfo {
	out := make(map[uint32]procInfo)
	h, err := syscall.CreateToolhelp32Snapshot(syscall.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return out
	}
	defer syscall.CloseHandle(h)
	var pe syscall.ProcessEntry32
	pe.Size = uint32(unsafe.Sizeof(pe))
	if syscall.Process32First(h, &pe) != nil {
		return out
	}
	for {
		p := procInfo{PID: pe.ProcessID, PPID: pe.ParentProcessID, Name: syscall.UTF16ToString(pe.ExeFile[:])}
		out[p.PID] = p
		pe.Size = uint32(unsafe.Sizeof(pe))
		if syscall.Process32Next(h, &pe) != nil {
			break
		}
	}
	return out
}

var sampledWindows map[uint32]uint64

func areaCallback(hWnd uintptr, _ uintptr) uintptr {
	v, _, _ := procVisible.Call(hWnd)
	if v == 0 {
		return 1
	}
	var pid uint32
	procWindowPID.Call(hWnd, uintptr(unsafe.Pointer(&pid)))
	if pid == 0 {
		return 1
	}
	var r RECT
	ok, _, _ := procWindowRect.Call(hWnd, uintptr(unsafe.Pointer(&r)))
	if ok == 0 {
		return 1
	}
	w, h := int64(r.Right-r.Left), int64(r.Bottom-r.Top)
	if w < 260 || h < 180 || w > 20000 || h > 20000 {
		return 1
	}
	area := uint64(w * h)
	if area > sampledWindows[pid] {
		sampledWindows[pid] = area
	}
	return 1
}

var areaCallbackFn = syscall.NewCallback(areaCallback)

func windowAreas() map[uint32]uint64 {
	sampledWindows = make(map[uint32]uint64)
	procEnumWindows.Call(areaCallbackFn, 0)
	return sampledWindows
}
func ignoredProcess(name string) bool {
	name = strings.ToLower(strings.TrimSpace(name))
	if !strings.HasSuffix(name, ".exe") {
		return true
	}
	for _, x := range []string{"explorer.exe", "cmd.exe", "conhost.exe", "services.exe", "start.exe", "svchost.exe", "rundll32.exe", "winedevice.exe", "wineboot.exe", "winefile.exe", "winecfg.exe", "wineconsole.exe", "notepad.exe", "regedit.exe", "taskmgr.exe", "gpulab.exe", "gpu-lab.exe", "winemenubuilder.exe", "msiexec.exe", "rpcss.exe", "plugplay.exe"} {
		if name == x {
			return true
		}
	}
	for _, x := range []string{"wine", "crashhandler", "crashreport", "updater", "uninstall", "launcher"} {
		if strings.HasPrefix(name, x) {
			return true
		}
	}
	return false
}
func pidAlive(pid uint32) bool {
	h, e := syscall.OpenProcess(syscall.SYNCHRONIZE, false, pid)
	if e != nil {
		return false
	}
	defer syscall.CloseHandle(h)
	r, _ := syscall.WaitForSingleObject(h, 0)
	return r == syscall.WAIT_TIMEOUT
}
func processBirth(pid uint32) uint64 {
	h, e := syscall.OpenProcess(0x1000, false, pid)
	if e != nil {
		return 0
	}
	defer syscall.CloseHandle(h)
	type ft struct{ Lo, Hi uint32 }
	var created, exit, kernel, user ft
	ok, _, _ := k32.NewProc("GetProcessTimes").Call(uintptr(h), uintptr(unsafe.Pointer(&created)), uintptr(unsafe.Pointer(&exit)), uintptr(unsafe.Pointer(&kernel)), uintptr(unsafe.Pointer(&user)))
	if ok == 0 {
		return 0
	}
	return uint64(created.Hi)<<32 | uint64(created.Lo)
}
func processPath(pid uint32) string {
	h, e := syscall.OpenProcess(0x1000, false, pid)
	if e != nil {
		return ""
	}
	defer syscall.CloseHandle(h)
	var b [4096]uint16
	n := uint32(len(b))
	ok, _, _ := k32.NewProc("QueryFullProcessImageNameW").Call(uintptr(h), 0, uintptr(unsafe.Pointer(&b[0])), uintptr(unsafe.Pointer(&n)))
	if ok == 0 || n >= uint32(len(b)) {
		return ""
	}
	return syscall.UTF16ToString(b[:n])
}

// All game handles are read-only or SYNCHRONIZE. No process termination is supported.
func processUsage(pid uint32) (float64, float64, bool) {
	h, e := syscall.OpenProcess(0x0400|0x1000, false, pid)
	if e != nil {
		return 0, 0, false
	}
	defer syscall.CloseHandle(h)
	type mem struct {
		Cb                                                                                                                                                                     uint32
		PageFaultCount                                                                                                                                                         uint32
		PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage uintptr
	}
	var m mem
	m.Cb = uint32(unsafe.Sizeof(m))
	ok, _, _ := syscall.NewLazyDLL("psapi.dll").NewProc("GetProcessMemoryInfo").Call(uintptr(h), uintptr(unsafe.Pointer(&m)), uintptr(m.Cb))
	if ok == 0 {
		return 0, 0, false
	}
	type ft struct{ Low, High uint32 }
	var cr, ex, k, u ft
	ok, _, _ = k32.NewProc("GetProcessTimes").Call(uintptr(h), uintptr(unsafe.Pointer(&cr)), uintptr(unsafe.Pointer(&ex)), uintptr(unsafe.Pointer(&k)), uintptr(unsafe.Pointer(&u)))
	if ok == 0 {
		return 0, float64(m.WorkingSetSize) / 1048576, true
	}
	cputicks := uint64(k.High)<<32 | uint64(k.Low)
	cputicks += uint64(u.High)<<32 | uint64(u.Low)
	return float64(cputicks) / 1e7, float64(m.WorkingSetSize) / 1048576, true
}
func selfPID() uint32 { return uint32(os.Getpid()) }
